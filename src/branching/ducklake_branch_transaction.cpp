#include "branching/ducklake_branch.hpp"

#include "duckdb/common/file_system.hpp"
#include "duckdb/planner/tableref/bound_at_clause.hpp"
#include "duckdb/storage/object_cache.hpp"
#include "storage/ducklake_catalog.hpp"
#include "storage/ducklake_delete.hpp"
#include "storage/ducklake_metadata_manager.hpp"
#include "storage/ducklake_schema_entry.hpp"
#include "storage/ducklake_table_entry.hpp"
#include "storage/ducklake_transaction.hpp"
#include "storage/ducklake_transaction_changes.hpp"
#include "storage/ducklake_transaction_state.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// Branch state of a transaction
//===--------------------------------------------------------------------===//
optional_ptr<DuckLakeBranchTransactionState> DuckLakeBranchManager::GetState(DuckLakeTransaction &transaction) {
	return transaction.branch_state.get();
}

DuckLakeBranchTransactionState &DuckLakeBranchManager::GetOrCreateState(DuckLakeTransaction &transaction) {
	if (!transaction.branch_state) {
		transaction.branch_state = make_shared_ptr<DuckLakeBranchTransactionState>();
	}
	return *transaction.branch_state;
}

DuckLakeTransactionState &DuckLakeBranchManager::GetTransactionState(DuckLakeTransaction &transaction) {
	return *transaction.state;
}

bool DuckLakeBranchManager::IsOnBranch(DuckLakeTransaction &transaction) {
	auto state = GetState(transaction);
	return state && state->branch_id.IsValid();
}

bool DuckLakeBranchManager::IsMergingBranch(DuckLakeTransaction &transaction) {
	auto state = GetState(transaction);
	return state && state->merge;
}

void DuckLakeBranchManager::SetBranch(DuckLakeTransaction &transaction, idx_t branch_id, string branch_name) {
	auto &state = GetOrCreateState(transaction);
	state.branch_id = branch_id;
	state.branch_name = std::move(branch_name);
}

void DuckLakeBranchManager::SetBranchMerge(DuckLakeTransaction &transaction, unique_ptr<DuckLakeBranchMerge> merge) {
	GetOrCreateState(transaction).merge = std::move(merge);
}

void DuckLakeBranchManager::EnsureNotOnBranch(DuckLakeTransaction &transaction, const string &operation) {
	if (IsOnBranch(transaction)) {
		throw NotImplementedException("%s is not supported on a branch yet (current branch: \"%s\")", operation,
		                              GetState(transaction)->branch_name);
	}
}

//===--------------------------------------------------------------------===//
// Whether the branch metadata tables exist, cached per attached catalog
//===--------------------------------------------------------------------===//
namespace {

class DuckLakeBranchTablesCacheEntry : public ObjectCacheEntry {
public:
	static string ObjectType() {
		return "ducklake_branch_tables";
	}
	string GetObjectType() override {
		return ObjectType();
	}
	optional_idx GetEstimatedCacheMemory() const override {
		// never evicted
		return optional_idx();
	}

	atomic<bool> exists {false};
};

DuckLakeBranchTablesCacheEntry &GetBranchTablesCache(DuckLakeTransaction &transaction) {
	auto key = "ducklake_branch_tables_" + to_string(transaction.GetCatalog().GetOid());
	auto &db = transaction.GetCatalog().GetAttached().GetDatabase();
	return *db.GetObjectCache().GetOrCreate<DuckLakeBranchTablesCacheEntry>(key);
}

} // namespace

bool DuckLakeBranchManager::IsBranchTablesCached(DuckLakeTransaction &transaction) {
	return GetBranchTablesCache(transaction).exists;
}

void DuckLakeBranchManager::SetHasBranchTables(DuckLakeTransaction &transaction, bool value) {
	GetBranchTablesCache(transaction).exists = value;
}

//===--------------------------------------------------------------------===//
// Reading a branch
//===--------------------------------------------------------------------===//
static void ClearBranchSelection(DuckLakeTransaction &transaction, idx_t branch_id) {
	auto context_ref = transaction.context.lock();
	if (!context_ref) {
		return;
	}
	auto branch_state = context_ref->registered_state->Get<DuckLakeBranchState>(DuckLakeBranchState::KEY);
	if (branch_state) {
		branch_state->ClearIfSelected(transaction.GetCatalog().GetOid(), branch_id);
	}
}

DuckLakeSnapshot DuckLakeBranchManager::GetBranchSnapshot(DuckLakeTransaction &transaction) {
	auto &state = *GetState(transaction);
	{
		lock_guard<mutex> guard(transaction.snapshot_lock);
		if (transaction.snapshot) {
			return *transaction.snapshot;
		}
		if (state.loading_fork_snapshot && state.loading_thread == std::this_thread::get_id()) {
			// looked up while this thread loads the branch - the fork snapshot is all that is needed
			return *state.loading_fork_snapshot;
		}
	}
	lock_guard<mutex> load_guard(state.load_lock);
	{
		lock_guard<mutex> guard(transaction.snapshot_lock);
		if (transaction.snapshot) {
			return *transaction.snapshot;
		}
	}
	auto branch_id = state.branch_id.GetIndex();
	auto branch = GetBranch(transaction, branch_id);
	unique_ptr<DuckLakeSnapshot> fork_snapshot;
	if (branch && branch->IsActive()) {
		BoundAtClause fork_clause(Identifier("version"), Value::UBIGINT(branch->fork_snapshot_id));
		try {
			fork_snapshot = transaction.GetMetadataManager().GetSnapshot(fork_clause, SnapshotBound::UPPER_BOUND);
		} catch (InvalidInputException &) {
			// the fork snapshot was expired
		}
	}
	if (!fork_snapshot) {
		ClearBranchSelection(transaction, branch_id);
		throw InvalidInputException("Branch \"%s\" no longer exists - this connection is back on main",
		                            state.branch_name);
	}
	{
		lock_guard<mutex> guard(transaction.snapshot_lock);
		state.loading_fork_snapshot = make_uniq<DuckLakeSnapshot>(*fork_snapshot);
		state.loading_thread = std::this_thread::get_id();
	}
	auto &transaction_state = GetTransactionState(transaction);
	auto loaded = make_uniq<DuckLakeLoadedBranch>();
	try {
		LoadBranch(transaction, *branch, *fork_snapshot, *loaded);
	} catch (...) {
		lock_guard<mutex> guard(transaction.snapshot_lock);
		transaction_state.local_changes.Clear();
		transaction_state.dropped_files.clear();
		transaction_state.dropped_file_stats.clear();
		transaction_state.tables_deleted_from.clear();
		state.loading_fork_snapshot.reset();
		state.loading_thread = std::thread::id();
		throw;
	}
	state.loaded_branch = std::move(loaded);
	lock_guard<mutex> guard(transaction.snapshot_lock);
	transaction.snapshot = std::move(fork_snapshot);
	state.loading_fork_snapshot.reset();
	state.loading_thread = std::thread::id();
	return *transaction.snapshot;
}

//===--------------------------------------------------------------------===//
// Committing a branch transaction or a merge
//===--------------------------------------------------------------------===//
void DuckLakeBranchManager::CommitToBranch(DuckLakeTransaction &transaction) {
	auto &state = *GetState(transaction);
	if (!state.loaded_branch) {
		// the branch was never read or written - only branch metadata (if any) needs committing
		auto &transaction_state = GetTransactionState(transaction);
		if (transaction_state.local_changes.HasChanges() || !transaction_state.dropped_files.empty()) {
			throw InternalException("Branch transaction has changes but no loaded branch state");
		}
		if (transaction.connection) {
			transaction.connection->Commit();
		}
		return;
	}
	CommitBranch(transaction, *state.loaded_branch);
}

string DuckLakeBranchManager::ChangesFingerprint(DuckLakeTransaction &transaction) {
	auto &state = GetTransactionState(transaction);
	auto result = StringUtil::Format("%d/%d/%d/%d/%d/%d", state.SchemaChangesMade(), state.dropped_files.size(),
	                                 state.flushed_inlined_tables.size(), transaction.new_name_maps.name_maps.size(),
	                                 state.tables_deleted_from.size(), state.tables_delete_attempted.size());
	for (auto &entry : state.local_changes.Changes()) {
		auto &changes = entry.GetTableChanges();
		idx_t delete_files = 0;
		for (auto &file : changes.new_delete_files) {
			delete_files += file.second.size();
		}
		idx_t inlined_deletes = 0;
		for (auto &deletes : changes.new_inlined_data_deletes) {
			inlined_deletes += deletes.second->rows.size();
		}
		idx_t inlined_rows = 0;
		if (changes.new_inlined_data) {
			auto &inlined = *changes.new_inlined_data;
			inlined_rows = inlined.data ? inlined.data->Count() : inlined.external_row_count;
		}
		result += StringUtil::Format(";%d:%d/%d/%d/%d/%d/%d", entry.GetTableIndex().index,
		                             changes.new_data_files.size(), delete_files, inlined_rows, inlined_deletes,
		                             changes.new_inlined_file_deletes ? 1 : 0, changes.compactions.size());
	}
	return result;
}

void DuckLakeBranchManager::CommitMerge(DuckLakeTransaction &transaction) {
	auto &merge = *GetState(transaction)->merge;
	if (ChangesFingerprint(transaction) != merge.changes_fingerprint) {
		// the statement changed more after the merge was prepared - a failed commit never reaches Rollback
		auto &connection = transaction.connection;
		if (connection && connection->context->transaction.HasActiveTransaction()) {
			connection->Rollback();
		}
		GetTransactionState(transaction).CleanupFiles();
		throw InvalidInputException("MERGE BRANCH must be the only change in its transaction");
	}
	if (transaction.ChangesMade()) {
		auto retry_config = DuckLakeRetryConfig::FromContext(*transaction.context.lock());
		auto transaction_changes = transaction.GetTransactionChanges();
		transaction.RunCommitLoop(merge.fork_snapshot, transaction_changes, retry_config);
		return;
	}
	// the branch changed nothing - only record that it was merged
	auto &metadata_connection = transaction.GetConnection();
	auto result = transaction.GetMetadataManager().Execute(MergeBookkeepingSql(transaction, merge, false));
	if (result->HasError()) {
		metadata_connection.Rollback();
		result->GetErrorObject().Throw(
		    StringUtil::Format("Failed to merge branch \"%s\" - retry: ", merge.loaded.info.name));
	}
	metadata_connection.Commit();
}

//===--------------------------------------------------------------------===//
// Local changes of a branch transaction
//===--------------------------------------------------------------------===//
void DuckLakeBranchManager::ForgetFile(DuckLakeTransaction &transaction, TableIndex table_id, const string &path) {
	auto &local_changes = GetTransactionState(transaction).local_changes;
	lock_guard<mutex> guard(local_changes.lock);
	auto entry = local_changes.changes.find(table_id);
	if (entry == local_changes.changes.end()) {
		throw InternalException("ForgetFile called for a table without transaction-local files");
	}
	auto &table_files = entry->second.new_data_files;
	for (idx_t i = 0; i < table_files.size(); i++) {
		if (table_files[i].file_name == path) {
			table_files.erase_at(i);
			if (entry->second.IsEmpty()) {
				local_changes.changes.erase(entry);
			}
			return;
		}
	}
	throw InternalException("ForgetFile could not find the transaction-local file");
}

void DuckLakeBranchManager::ForgetDeleteFiles(DuckLakeTransaction &transaction, TableIndex table_id,
                                              const string &data_file_path) {
	auto &local_changes = GetTransactionState(transaction).local_changes;
	lock_guard<mutex> guard(local_changes.lock);
	auto entry = local_changes.changes.find(table_id);
	if (entry == local_changes.changes.end()) {
		return;
	}
	entry->second.new_delete_files.erase(data_file_path);
	if (entry->second.IsEmpty()) {
		local_changes.changes.erase(entry);
	}
}

bool DuckLakeBranchManager::IsLoadedBranchFile(DuckLakeTransaction &transaction, TableIndex table_id,
                                               const string &path) {
	auto &local_changes = GetTransactionState(transaction).local_changes;
	lock_guard<mutex> guard(local_changes.lock);
	auto entry = local_changes.changes.find(table_id);
	if (entry == local_changes.changes.end()) {
		return false;
	}
	for (auto &file : entry->second.new_data_files) {
		if (file.file_name == path) {
			return !file.created_by_ducklake;
		}
	}
	return false;
}

void DuckLakeBranchManager::FlushBranchDelete(const DuckLakeDelete &op, DuckLakeTransaction &transaction,
                                              ClientContext &context,
                                              unordered_map<string, DuckLakeDeleteFile> &written_files,
                                              const string &filename,
                                              const DuckLakeFileListExtendedEntry &data_file_info, set<idx_t> deletes,
                                              DuckLakeDeleteFile &delete_file) {
	auto &table = op.table;
	auto table_id = table.GetTableId();
	auto fork_snapshot = transaction.GetSnapshot();
	auto has_branch_delete = transaction.HasLocalDeleteForFile(table_id, filename);
	auto existing_delete_data = op.delete_map->GetDeleteData(filename);
	if (existing_delete_data) {
		if (has_branch_delete) {
			// the branch's own delete file already holds everything deleted on the branch
			deletes.insert(existing_delete_data->deleted_rows.begin(), existing_delete_data->deleted_rows.end());
		} else {
			// main's delete file can hold deletes made after the fork - keep only the ones visible at the fork
			idx_t fallback_snapshot = 0;
			if (!existing_delete_data->HasEmbeddedSnapshots() && data_file_info.delete_file_begin_snapshot.IsValid()) {
				fallback_snapshot = data_file_info.delete_file_begin_snapshot.GetIndex();
			}
			set<PositionWithSnapshot> existing_deletes;
			MergeDeletesWithSnapshots(*existing_delete_data, fallback_snapshot, existing_deletes);
			for (auto &entry : existing_deletes) {
				if (static_cast<idx_t>(entry.snapshot_id) <= fork_snapshot.snapshot_id) {
					deletes.insert(static_cast<idx_t>(entry.position));
				}
			}
		}
		op.delete_map->ClearDeletes(filename);
		delete_file.overwrites_existing_delete = true;
	}
	if (!has_branch_delete) {
		// main's inlined file deletions are not part of its delete file
		auto inlined_deletes = transaction.GetMetadataManager().ReadInlinedFileDeletions(table_id, fork_snapshot);
		auto entry = inlined_deletes.find(data_file_info.file_id.index);
		if (entry != inlined_deletes.end()) {
			deletes.insert(entry->second.begin(), entry->second.end());
		}
	}
	if (op.TryDropFullyDeletedFile(transaction, delete_file, data_file_info, deletes.size())) {
		return;
	}
	auto &fs = FileSystem::GetFileSystem(context);
	auto &catalog = table.catalog.Cast<DuckLakeCatalog>();
	auto &schema = table.ParentSchema().Cast<DuckLakeSchemaEntry>();
	bool use_deletion_vectors = catalog.WriteDeletionVectors(schema.GetSchemaId(), table_id, &table.GetTableOptions());
	WriteDeleteFileInput input {context,           transaction, fs,      table.DataPath(),
	                            op.encryption_key, filename,    deletes, DeleteFileSource::REGULAR};
	auto written_file = DuckLakeDeleteFileWriter::Write(context, input, use_deletion_vectors);
	written_file.data_file_id = delete_file.data_file_id;
	written_file.overwrites_existing_delete = delete_file.overwrites_existing_delete;
	written_files.emplace(filename, std::move(written_file));
}

} // namespace duckdb
