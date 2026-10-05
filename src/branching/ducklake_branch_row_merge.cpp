#include "branching/ducklake_branch.hpp"

#include "branching/ducklake_branch_merge_rows.hpp"
#include "branching/ducklake_branch_util.hpp"
#include "common/ducklake_util.hpp"
#include "common/index.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/main/client_context.hpp"
#include "storage/ducklake_catalog.hpp"
#include "storage/ducklake_delete.hpp"
#include "storage/ducklake_delete_filter.hpp"
#include "storage/ducklake_metadata_manager.hpp"
#include "storage/ducklake_schema_entry.hpp"
#include "storage/ducklake_table_entry.hpp"
#include "storage/ducklake_transaction.hpp"
#include "storage/ducklake_transaction_changes.hpp"
#include "storage/ducklake_transaction_state.hpp"

namespace duckdb {

// A table both sides changed rows of merges row by row: rows are matched by row id, and a row only one side changed
// takes that side's change. A row both changed merges when both ended at the same value, and conflicts otherwise.
// "Changed" compares with the fork, so a rewrite with the old values is no change.

static bool Contains(const set<TableIndex> &tables, TableIndex table_id) {
	return tables.find(table_id) != tables.end();
}

SnapshotChangeInformation DuckLakeBranchManager::MainChangesSince(DuckLakeTransaction &transaction,
                                                                  DuckLakeSnapshot snapshot) {
	auto &metadata_manager = transaction.GetMetadataManager();
	auto executor = [&](string query) -> unique_ptr<QueryResult> {
		auto result = metadata_manager.Query(snapshot, query);
		if (result->HasError()) {
			result->GetErrorObject().Throw("Failed to read main's changes since the fork of a DuckLake branch: ");
		}
		return result;
	};
	SnapshotAndStats snapshot_and_stats;
	auto changes = DuckLakeMetadataManager::GetSnapshotAndStatsAndChanges(
	    snapshot_and_stats, executor, transaction.GetCatalog().SupportsV1_1Metadata());
	return SnapshotChangeInformation::ParseChangesMade(changes.changes_made);
}

set<TableIndex> DuckLakeBranchManager::RenamedOnMain(DuckLakeTransaction &transaction, bool views,
                                                     const set<TableIndex> &ids, idx_t fork_snapshot_id) {
	set<TableIndex> result;
	auto table = views ? "ducklake_view" : "ducklake_table";
	auto id_column = views ? "view_id" : "table_id";
	vector<idx_t> id_values;
	for (auto &id : ids) {
		id_values.push_back(id.index);
	}
	auto query_result =
	    RunBranchQuery(transaction,
	                   StringUtil::Format("SELECT DISTINCT %s FROM {METADATA_CATALOG}.%s WHERE %s IN (%s) AND "
	                                      "begin_snapshot > %d",
	                                      id_column, table, id_column, BranchIdList(id_values), fork_snapshot_id),
	                   "Failed to read main's renames since the fork of a DuckLake branch: ");
	for (auto &row : *query_result) {
		result.insert(TableIndex(row.GetValue<idx_t>(0)));
	}
	return result;
}

bool DuckLakeBranchManager::KeepsTableLevelRules(TableIndex table_id, bool branch_deleted, bool branch_deleted_inlined,
                                                 const SnapshotChangeInformation &main_changes) {
	if (Contains(main_changes.dropped_tables, table_id) || Contains(main_changes.altered_tables, table_id)) {
		return true;
	}
	// positions in files main compacted no longer match the branch's deletes
	if (branch_deleted && (Contains(main_changes.tables_merge_adjacent, table_id) ||
	                       Contains(main_changes.tables_rewrite_delete, table_id))) {
		return true;
	}
	return branch_deleted_inlined && Contains(main_changes.tables_flushed_inlined, table_id);
}

map<TableIndex, DuckLakeRowMergeTable>
DuckLakeBranchManager::FindRowMergeTables(DuckLakeTransaction &transaction, const DuckLakeLoadedBranch &loaded,
                                          DuckLakeSnapshot fork_snapshot,
                                          const SnapshotChangeInformation &main_changes) {
	auto &state = *transaction.state;
	set<TableIndex> branch_tables, branch_deleted, branch_deleted_inlined;
	for (auto &entry : state.local_changes.Changes()) {
		auto table_id = entry.GetTableIndex();
		auto &changes = entry.GetTableChanges();
		branch_tables.insert(table_id);
		if (!changes.new_delete_files.empty()) {
			branch_deleted.insert(table_id);
		}
		if (!changes.new_inlined_data_deletes.empty()) {
			branch_deleted.insert(table_id);
			branch_deleted_inlined.insert(table_id);
		}
	}
	for (auto &dropped : loaded.dropped_files) {
		branch_tables.insert(dropped.second);
		branch_deleted.insert(dropped.second);
	}
	set<TableIndex> candidates;
	for (auto &table_id : branch_tables) {
		auto main_inserted = Contains(main_changes.inserted_tables, table_id) ||
		                     Contains(main_changes.tables_inserted_inlined, table_id);
		auto main_deleted = Contains(main_changes.tables_deleted_from, table_id) ||
		                    Contains(main_changes.tables_deleted_inlined, table_id);
		auto deleted = Contains(branch_deleted, table_id);
		// inserts on both sides merge under DuckLake's own rules
		if ((!main_inserted && !main_deleted) || (!deleted && !main_deleted)) {
			continue;
		}
		if (KeepsTableLevelRules(table_id, deleted, Contains(branch_deleted_inlined, table_id), main_changes)) {
			continue;
		}
		candidates.insert(table_id);
	}
	map<TableIndex, DuckLakeRowMergeTable> result;
	if (candidates.empty()) {
		return result;
	}
	auto renamed_on_main = RenamedOnMain(transaction, false, candidates, fork_snapshot.snapshot_id);
	auto head = transaction.GetSnapshot();
	for (auto &table_id : candidates) {
		auto entry = transaction.GetCatalog().GetEntryById(transaction, head, table_id);
		if (Contains(renamed_on_main, table_id) || !entry) {
			continue;
		}
		auto &schema = entry->ParentSchema().Cast<DuckLakeSchemaEntry>();
		if (schema.ParentDuckLakeSchema()) {
			// tables in nested schemas keep the table-level rules
			continue;
		}
		DuckLakeRowMergeTable table;
		table.table_id = table_id;
		table.schema_name = schema.name.GetIdentifierName();
		table.main_name = entry->name.GetIdentifierName();
		table.branch_name = table.main_name;
		result.emplace(table_id, std::move(table));
	}
	return result;
}

namespace {

//! One row as one side has it
struct RowCopy {
	bool present = false;
	string file;
	idx_t position = 0;
	vector<Value> values;
};

bool SameRow(const RowCopy &a, const RowCopy &b) {
	if (a.present != b.present) {
		return false;
	}
	for (idx_t column = 0; column < a.values.size(); column++) {
		if (!Value::NotDistinctFrom(a.values[column], b.values[column])) {
			return false;
		}
	}
	return true;
}

//! Reads the given rows of one side; "located" reads also return each row's file and position
void ReadRows(Connection &connection, const string &query, const set<int64_t> &row_ids, bool located,
              map<int64_t, RowCopy> &result) {
	static constexpr idx_t IDS_PER_QUERY = 2048;
	vector<int64_t> ids(row_ids.begin(), row_ids.end());
	for (idx_t start = 0; start < ids.size(); start += IDS_PER_QUERY) {
		vector<int64_t> batch(ids.begin() + start, ids.begin() + MinValue<idx_t>(start + IDS_PER_QUERY, ids.size()));
		auto rows = RunBranchSql(connection, query + " WHERE rowid IN (" + BranchIdList(batch) + ")");
		auto first_value = located ? 3 : 1;
		for (auto &row : *rows) {
			RowCopy copy;
			copy.present = true;
			if (located) {
				copy.file = row.GetValue<string>(1);
				copy.position = row.GetValue<idx_t>(2);
			}
			for (idx_t column = first_value; column < rows->ColumnCount(); column++) {
				copy.values.push_back(row.GetBaseValue(column));
			}
			result[row.GetValue<int64_t>(0)] = std::move(copy);
		}
	}
}

} // namespace

void DuckLakeBranchManager::PlanRowMerge(ClientContext &context, const string &catalog_name, const string &branch_name,
                                         idx_t fork_snapshot_id, idx_t head_snapshot_id, DuckLakeRowMergeTable &table) {
	auto main_table = BranchTableSql(catalog_name, table.schema_name, table.main_name);
	auto branch_table = BranchTableSql(catalog_name, table.schema_name, table.branch_name);
	auto main_connection = make_uniq<Connection>(*context.db);
	auto branch_connection = BranchSqlConnection(context, catalog_name, branch_name);

	// what the branch touched: the rows it deleted, and the rows it rewrote into its own files
	set<int64_t> branch_touched;
	DuckLakeBranchRowCursor fork_rows, branch_rows;
	fork_rows.Start(RunBranchSql(*main_connection, StringUtil::Format("SELECT rowid FROM %s AT (VERSION => %d) ORDER "
	                                                                  "BY rowid",
	                                                                  main_table, fork_snapshot_id)));
	branch_rows.Start(
	    RunBranchSql(*branch_connection, "SELECT rowid, snapshot_id IS NULL FROM " + branch_table + " ORDER BY rowid"));
	for (; fork_rows.Valid(); fork_rows.Next()) {
		auto row_id = fork_rows.RowId();
		while (branch_rows.Valid() && branch_rows.RowId() < row_id) {
			branch_rows.Next();
		}
		// a row the branch rewrote lives in a branch file, which has no snapshot id yet
		if (!branch_rows.Valid() || branch_rows.RowId() != row_id || branch_rows.Get(1).GetValue<bool>()) {
			branch_touched.insert(row_id);
		}
	}
	if (branch_touched.empty()) {
		return;
	}
	// what main touched: every row it deleted since the fork, the old copies of the rows it updated included
	auto deletions = RunBranchSql(
	    *main_connection,
	    StringUtil::Format("SELECT DISTINCT rowid FROM ducklake_table_deletions(%s, %s, %s, %d, %d)",
	                       DuckLakeUtil::SQLLiteralToString(catalog_name),
	                       DuckLakeUtil::SQLLiteralToString(table.schema_name),
	                       DuckLakeUtil::SQLLiteralToString(table.main_name), fork_snapshot_id + 1, head_snapshot_id));
	for (auto &row : *deletions) {
		auto row_id = row.GetValue<int64_t>(0);
		if (branch_touched.find(row_id) != branch_touched.end()) {
			table.overlap.insert(row_id);
		}
	}
	if (table.overlap.empty()) {
		return;
	}
	// the rows both touched, as they were at the fork and as each side has them now
	map<int64_t, RowCopy> at_fork, on_branch, on_main;
	ReadRows(*main_connection,
	         StringUtil::Format("SELECT rowid, * FROM %s AT (VERSION => %d)", main_table, fork_snapshot_id),
	         table.overlap, false, at_fork);
	ReadRows(*branch_connection, "SELECT rowid, filename, file_row_number, * FROM " + branch_table, table.overlap, true,
	         on_branch);
	ReadRows(*main_connection,
	         StringUtil::Format("SELECT rowid, filename, file_row_number, * FROM %s AT (VERSION => %d)", main_table,
	                            head_snapshot_id),
	         table.overlap, true, on_main);
	for (auto &row_id : table.overlap) {
		auto &fork = at_fork[row_id];
		auto &branch = on_branch[row_id];
		auto &main = on_main[row_id];
		auto branch_changed = !SameRow(branch, fork);
		auto main_changed = !SameRow(main, fork);
		if (branch_changed && main_changed && !SameRow(branch, main)) {
			table.conflicts.push_back(row_id);
			continue;
		}
		if (branch_changed && !main_changed) {
			// main only rewrote the row with its old values: the branch's change wins, and main's copy goes
			table.main_rows_to_drop[main.file].emplace_back(main.position, row_id);
			continue;
		}
		// main's copy stays, and the branch's copy goes
		if (branch_changed) {
			table.same_as_main.insert(row_id);
		}
		if (branch.present) {
			table.branch_rows_to_drop[branch.file].insert(branch.position);
		}
	}
}

string DuckLakeBranchManager::RowConflictMessage(const string &branch_name, const DuckLakeRowMergeTable &table) {
	static constexpr idx_t ROWS_SHOWN = 10;
	vector<int64_t> shown(table.conflicts.begin(),
	                      table.conflicts.begin() + MinValue<idx_t>(ROWS_SHOWN, table.conflicts.size()));
	auto rows = BranchIdList(shown);
	if (table.conflicts.size() > ROWS_SHOWN) {
		rows += ", ...";
	}
	return StringUtil::Format("Transaction conflict - branch \"%s\" and main changed %d row(s) of table \"%s\" "
	                          "differently since the fork (rowid %s); ducklake_merge_branch with dry_run => true and "
	                          "table_name => '%s' shows them",
	                          branch_name, table.conflicts.size(), table.main_name, rows, table.branch_name);
}

void DuckLakeBranchManager::ApplyRowMerge(DuckLakeTransaction &transaction, DuckLakeBranchMerge &merge) {
	auto context_ref = transaction.context.lock();
	auto &context = *context_ref;
	auto &catalog = transaction.GetCatalog();
	auto &local_changes = transaction.state->local_changes;
	auto &fs = FileSystem::GetFileSystem(context);
	for (auto &entry : merge.row_merge) {
		auto &table = entry.second;
		if (!table.conflicts.empty()) {
			throw TransactionException(RowConflictMessage(merge.loaded.info.name, table));
		}
		auto table_entry = transaction.GetCatalog().GetEntryById(transaction, merge.head_snapshot, table.table_id);
		auto &table_data = table_entry->Cast<DuckLakeTableEntry>();
		auto &schema = table_data.ParentSchema().Cast<DuckLakeSchemaEntry>();
		bool use_deletion_vectors =
		    catalog.WriteDeletionVectors(schema.GetSchemaId(), table.table_id, &table_data.GetTableOptions());
		// what to change, read under the lock: the files themselves are read and written outside it
		struct BranchFileDrop {
			string data_file;
			set<idx_t> positions;
			optional_idx old_delete_id;
			DuckLakeFileData old_delete;
		};
		vector<BranchFileDrop> drops;
		{
			lock_guard<mutex> guard(local_changes.lock);
			auto changes = local_changes.changes.find(table.table_id);
			if (changes == local_changes.changes.end()) {
				// the branch only emptied main files of the table: none of its own files hold rows to drop
				D_ASSERT(table.branch_rows_to_drop.empty());
				continue;
			}
			// main deleted every row both sides touched: the branch's deletes of inlined ones would repeat main's
			for (auto &inlined : changes->second.new_inlined_data_deletes) {
				for (auto &row_id : table.overlap) {
					inlined.second->rows.erase(static_cast<idx_t>(row_id));
				}
			}
			for (auto &file_rows : table.branch_rows_to_drop) {
				BranchFileDrop drop;
				drop.data_file = file_rows.first;
				drop.positions = file_rows.second;
				bool found = false;
				for (auto &file : changes->second.new_data_files) {
					if (file.file_name != file_rows.first) {
						continue;
					}
					found = true;
					if (!file.delete_files.empty()) {
						auto &old_delete = file.delete_files.back();
						drop.old_delete_id = merge.loaded.delete_files[old_delete.file_name];
						drop.old_delete.path = old_delete.file_name;
						drop.old_delete.encryption_key = old_delete.encryption_key;
						drop.old_delete.file_size_bytes = old_delete.file_size_bytes;
						drop.old_delete.footer_size = old_delete.footer_size;
						drop.old_delete.format = old_delete.format;
					}
				}
				if (!found) {
					throw InternalException("A branch row merged row by row is not in one of the branch's files");
				}
				drops.push_back(std::move(drop));
			}
		}
		// the branch's copies of rows main's copy stays for: added to the delete files of their branch files
		vector<pair<string, DuckLakeDeleteFile>> written_files;
		for (auto &drop : drops) {
			if (drop.old_delete_id.IsValid()) {
				auto old_positions = DuckLakeDeleteFilter::ScanDeleteFile(context, drop.old_delete);
				drop.positions.insert(old_positions.deleted_rows.begin(), old_positions.deleted_rows.end());
				// main does not take over the branch's own delete file
				merge.files_to_schedule.emplace_back(drop.old_delete_id.GetIndex(), drop.old_delete.path);
			}
			WriteDeleteFileInput input {context,
			                            transaction,
			                            fs,
			                            table_data.DataPath(),
			                            catalog.GenerateEncryptionKey(context),
			                            drop.data_file,
			                            std::move(drop.positions),
			                            DeleteFileSource::REGULAR};
			written_files.emplace_back(drop.data_file,
			                           DuckLakeDeleteFileWriter::Write(context, input, use_deletion_vectors));
		}
		for (auto &written : written_files) {
			local_changes.TransactionLocalDelete(context, table.table_id, written.first, std::move(written.second));
		}
	}
}

void DuckLakeBranchManager::ExcludeFromInsertDeleteRules(TableIndex table_id, TransactionChangeInformation &changes) {
	// row-by-row merging decided these tables; DuckLake's rules on dropped, altered and compacted tables still apply
	// through the checks that kept them out of the row-by-row merge, and CheckRowMergeTablesUnchanged
	changes.tables_inserted_into.erase(table_id);
	changes.tables_deleted_from.erase(table_id);
	changes.tables_delete_attempted.erase(table_id);
	changes.tables_inserted_inlined.erase(table_id);
	changes.tables_deleted_inlined.erase(table_id);
}

void DuckLakeBranchManager::CheckRowMergeTablesUnchanged(DuckLakeTransaction &transaction,
                                                         const DuckLakeBranchMerge &merge) {
	if (merge.row_merge.empty()) {
		return;
	}
	auto changes = MainChangesSince(transaction, merge.head_snapshot);
	for (auto &entry : merge.row_merge) {
		auto table_id = entry.first;
		for (auto tables : {&changes.inserted_tables, &changes.tables_deleted_from, &changes.tables_inserted_inlined,
		                    &changes.tables_deleted_inlined, &changes.altered_tables, &changes.dropped_tables,
		                    &changes.tables_compacted, &changes.tables_merge_adjacent, &changes.tables_rewrite_delete,
		                    &changes.tables_flushed_inlined}) {
			if (Contains(*tables, table_id)) {
				throw TransactionException("Transaction conflict - main changed table \"%s\" while branch \"%s\" was "
				                           "merged into it - retry MERGE BRANCH",
				                           entry.second.main_name, merge.loaded.info.name);
			}
		}
	}
}

} // namespace duckdb
