#include "branching/ducklake_branch.hpp"

#include "common/ducklake_util.hpp"
#include "duckdb/common/types/blob.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/transaction/transaction_context.hpp"
#include "storage/ducklake_catalog.hpp"
#include "storage/ducklake_metadata_info.hpp"
#include "storage/ducklake_metadata_manager.hpp"
#include "storage/ducklake_table_entry.hpp"
#include "storage/ducklake_transaction.hpp"
#include "storage/ducklake_transaction_state.hpp"

#include <chrono>

namespace duckdb {

//===--------------------------------------------------------------------===//
// Per-connection selection
//===--------------------------------------------------------------------===//
bool DuckLakeBranchState::TryGetSelection(idx_t catalog_oid, Selection &result) const {
	lock_guard<mutex> guard(lock);
	auto entry = selections.find(catalog_oid);
	if (entry == selections.end()) {
		return false;
	}
	result = entry->second;
	return true;
}

void DuckLakeBranchState::Select(idx_t catalog_oid, idx_t branch_id, string name) {
	lock_guard<mutex> guard(lock);
	selections[catalog_oid] = Selection {branch_id, std::move(name)};
}

void DuckLakeBranchState::Clear(idx_t catalog_oid) {
	lock_guard<mutex> guard(lock);
	selections.erase(catalog_oid);
}

void DuckLakeBranchState::ClearIfSelected(idx_t catalog_oid, idx_t branch_id) {
	lock_guard<mutex> guard(lock);
	auto entry = selections.find(catalog_oid);
	if (entry != selections.end() && entry->second.branch_id == branch_id) {
		selections.erase(entry);
	}
}

//===--------------------------------------------------------------------===//
// Helpers
//===--------------------------------------------------------------------===//
static unique_ptr<QueryResult> RunBranchQuery(DuckLakeTransaction &transaction, string query, const string &error) {
	auto result = transaction.Query(std::move(query));
	if (result->HasError()) {
		result->GetErrorObject().Throw(error);
	}
	return result;
}

static string OptionalToSQL(const optional_idx &value) {
	return value.IsValid() ? to_string(value.GetIndex()) : "NULL";
}

static string LoadBranchPath(DuckLakeCatalog &catalog, const string &base_path, const string &path, bool relative) {
	auto result = relative ? base_path + path : path;
	auto &separator = catalog.Separator();
	if (separator != "/") {
		result = StringUtil::Replace(result, "/", separator);
	}
	return result;
}

static optional_idx OptionalIndex(const Value &value) {
	if (value.IsNull()) {
		return optional_idx();
	}
	return value.GetValue<idx_t>();
}

static vector<DuckLakeBranchInfo> ReadBranches(DuckLakeTransaction &transaction, const string &filter) {
	if (!DuckLakeBranchManager::HasBranchTables(transaction)) {
		return vector<DuckLakeBranchInfo>();
	}
	auto result = RunBranchQuery(transaction,
	                             "SELECT branch_id, branch_name, fork_snapshot_id, head_seq, next_file_seq, status, "
	                             "created_at FROM {METADATA_CATALOG}.ducklake_branching_branch WHERE " +
	                                 filter + " ORDER BY branch_id",
	                             "Failed to read DuckLake branches: ");
	vector<DuckLakeBranchInfo> branches;
	for (auto &row : *result) {
		DuckLakeBranchInfo info;
		info.branch_id = row.GetValue<idx_t>(0);
		info.name = row.GetValue<string>(1);
		info.fork_snapshot_id = row.GetValue<idx_t>(2);
		info.head_seq = row.GetValue<idx_t>(3);
		info.next_file_seq = row.GetValue<idx_t>(4);
		info.status = row.GetValue<string>(5);
		info.created_at = row.GetBaseValue(6);
		branches.push_back(std::move(info));
	}
	return branches;
}

//===--------------------------------------------------------------------===//
// Metadata tables
//===--------------------------------------------------------------------===//
static constexpr const char *BRANCH_TABLES_SQL = R"(
CREATE TABLE IF NOT EXISTS {METADATA_CATALOG}.ducklake_branching_branch(branch_id BIGINT PRIMARY KEY, branch_name VARCHAR, fork_snapshot_id BIGINT, head_seq BIGINT, next_file_seq BIGINT, status VARCHAR, created_at TIMESTAMPTZ);
CREATE TABLE IF NOT EXISTS {METADATA_CATALOG}.ducklake_branching_name(branch_name VARCHAR PRIMARY KEY, branch_id BIGINT);
CREATE TABLE IF NOT EXISTS {METADATA_CATALOG}.ducklake_branching_commit(branch_id BIGINT, commit_seq BIGINT, commit_time TIMESTAMPTZ, author VARCHAR, commit_message VARCHAR, commit_extra_info VARCHAR, changes_made VARCHAR, PRIMARY KEY(branch_id, commit_seq));
CREATE TABLE IF NOT EXISTS {METADATA_CATALOG}.ducklake_branching_data_file(branch_file_id BIGINT PRIMARY KEY, branch_id BIGINT, table_id BIGINT, begin_seq BIGINT, end_seq BIGINT, path VARCHAR, path_is_relative BOOLEAN, file_format VARCHAR, record_count BIGINT, file_size_bytes BIGINT, footer_size BIGINT, row_group_count BIGINT, partition_id BIGINT, encryption_key VARCHAR);
CREATE TABLE IF NOT EXISTS {METADATA_CATALOG}.ducklake_branching_file_column_stats(branch_file_id BIGINT, branch_id BIGINT, table_id BIGINT, column_id BIGINT, column_size_bytes BIGINT, value_count BIGINT, null_count BIGINT, min_value VARCHAR, max_value VARCHAR, contains_nan BOOLEAN, extra_stats VARCHAR, min_is_exact BOOLEAN, max_is_exact BOOLEAN);
CREATE TABLE IF NOT EXISTS {METADATA_CATALOG}.ducklake_branching_file_partition_value(branch_file_id BIGINT, branch_id BIGINT, table_id BIGINT, partition_key_index BIGINT, partition_value VARCHAR);
CREATE TABLE IF NOT EXISTS {METADATA_CATALOG}.ducklake_branching_delete_file(branch_file_id BIGINT PRIMARY KEY, branch_id BIGINT, table_id BIGINT, begin_seq BIGINT, end_seq BIGINT, target_data_file_id BIGINT, target_branch_file_id BIGINT, path VARCHAR, path_is_relative BOOLEAN, format VARCHAR, delete_count BIGINT, file_size_bytes BIGINT, footer_size BIGINT, row_group_count BIGINT, encryption_key VARCHAR);
CREATE TABLE IF NOT EXISTS {METADATA_CATALOG}.ducklake_branching_inlined_delete(branch_id BIGINT, begin_seq BIGINT, table_id BIGINT, inlined_table_name VARCHAR, row_id BIGINT);
CREATE TABLE IF NOT EXISTS {METADATA_CATALOG}.ducklake_branching_dropped_file(branch_id BIGINT, begin_seq BIGINT, table_id BIGINT, data_file_id BIGINT);
)";

bool DuckLakeBranchManager::HasBranchTables(DuckLakeTransaction &transaction) {
	if (IsBranchTablesCached(transaction)) {
		return true;
	}
	auto probe = transaction.Query("SELECT NULL FROM {METADATA_CATALOG}.ducklake_branching_branch LIMIT 1");
	if (probe->HasError()) {
		if (probe->GetErrorObject().Type() == ExceptionType::CATALOG) {
			return false;
		}
		probe->GetErrorObject().Throw("Failed to probe DuckLake branch tables: ");
	}
	SetHasBranchTables(transaction, true);
	return true;
}

bool DuckLakeBranchManager::CreateTables(DuckLakeTransaction &transaction) {
	if (HasBranchTables(transaction)) {
		return false;
	}
	RunBranchQuery(transaction, BRANCH_TABLES_SQL, "Failed to create DuckLake branch tables: ");
	SetHasBranchTables(transaction, true);
	return true;
}

//===--------------------------------------------------------------------===//
// Branch lifecycle
//===--------------------------------------------------------------------===//
void DuckLakeBranchManager::ValidateBranchName(const string &name) {
	if (name.empty()) {
		throw InvalidInputException("Branch name cannot be empty");
	}
	if (StringUtil::CIEquals(name, MAIN_BRANCH_NAME)) {
		throw InvalidInputException("\"%s\" is reserved for the main line of history", MAIN_BRANCH_NAME);
	}
}

void DuckLakeBranchManager::EnsureAutoCommit(ClientContext &context, const string &statement) {
	if (!context.transaction.IsAutoCommit()) {
		throw TransactionException("%s cannot be used inside an explicit transaction", statement);
	}
}

unique_ptr<DuckLakeBranchInfo> DuckLakeBranchManager::GetBranch(DuckLakeTransaction &transaction, idx_t branch_id) {
	auto branches = ReadBranches(transaction, StringUtil::Format("branch_id = %d", branch_id));
	if (branches.empty()) {
		return nullptr;
	}
	return make_uniq<DuckLakeBranchInfo>(std::move(branches[0]));
}

unique_ptr<DuckLakeBranchInfo> DuckLakeBranchManager::GetActiveBranch(DuckLakeTransaction &transaction,
                                                                      const string &name) {
	auto branches = ReadBranches(transaction, StringUtil::Format("branch_name = %s AND status = 'active'",
	                                                             DuckLakeUtil::SQLLiteralToString(name)));
	if (branches.empty()) {
		return nullptr;
	}
	return make_uniq<DuckLakeBranchInfo>(std::move(branches[0]));
}

vector<DuckLakeBranchInfo> DuckLakeBranchManager::GetBranches(DuckLakeTransaction &transaction) {
	return ReadBranches(transaction, "status = 'active'");
}

DuckLakeBranchInfo DuckLakeBranchManager::CreateBranch(DuckLakeTransaction &transaction, const string &name) {
	ValidateBranchName(name);
	auto created_tables = CreateTables(transaction);
	if (GetActiveBranch(transaction, name)) {
		throw InvalidInputException("Branch \"%s\" already exists", name);
	}
	auto id_result = RunBranchQuery(
	    transaction, "SELECT COALESCE(MAX(branch_id), 0) + 1 FROM {METADATA_CATALOG}.ducklake_branching_branch",
	    "Failed to allocate a DuckLake branch id: ");
	DuckLakeBranchInfo info;
	for (auto &row : *id_result) {
		info.branch_id = row.GetValue<idx_t>(0);
	}
	// a branch forks from the head of main
	auto head = transaction.GetMetadataManager().GetSnapshot();
	info.name = name;
	info.fork_snapshot_id = head->snapshot_id;
	info.status = "active";
	auto name_literal = DuckLakeUtil::SQLLiteralToString(name);
	auto result = transaction.Query(StringUtil::Format(
	    "INSERT INTO {METADATA_CATALOG}.ducklake_branching_name VALUES (%s, %d);\n"
	    "INSERT INTO {METADATA_CATALOG}.ducklake_branching_branch VALUES (%d, %s, %d, 0, 0, 'active', NOW());",
	    name_literal, info.branch_id, info.branch_id, name_literal, info.fork_snapshot_id));
	if (result->HasError()) {
		if (created_tables) {
			// the tables are rolled back with this transaction
			SetHasBranchTables(transaction, false);
		}
		result->GetErrorObject().Throw(
		    StringUtil::Format("Failed to create branch \"%s\" - another branch may have been created concurrently, "
		                       "retry: ",
		                       name));
	}
	return info;
}

void DuckLakeBranchManager::DropBranch(DuckLakeTransaction &transaction, const DuckLakeBranchInfo &branch) {
	auto id = branch.branch_id;
	// marking the branch dropped first locks its row, so a branch commit in flight fails instead of adding rows
	auto update_result = RunBranchQuery(
	    transaction,
	    StringUtil::Format("UPDATE {METADATA_CATALOG}.ducklake_branching_branch SET status = 'dropped' WHERE "
	                       "branch_id = %d AND status = 'active'",
	                       id),
	    "Failed to drop DuckLake branch: ");
	idx_t updated_rows = 0;
	for (auto &row : *update_result) {
		updated_rows = row.GetValue<idx_t>(0);
	}
	if (updated_rows != 1) {
		throw TransactionException("Branch \"%s\" was changed or dropped by another transaction - retry", branch.name);
	}
	string query = StringUtil::Format(R"(
INSERT INTO {METADATA_CATALOG}.ducklake_files_scheduled_for_deletion
SELECT branch_file_id, path, path_is_relative, NOW() FROM {METADATA_CATALOG}.ducklake_branching_data_file WHERE branch_id = %d
UNION ALL
SELECT branch_file_id, path, path_is_relative, NOW() FROM {METADATA_CATALOG}.ducklake_branching_delete_file WHERE branch_id = %d;
)",
	                                  id, id);
	vector<string> branch_tables {"ducklake_branching_data_file",
	                              "ducklake_branching_delete_file",
	                              "ducklake_branching_file_column_stats",
	                              "ducklake_branching_file_partition_value",
	                              "ducklake_branching_inlined_delete",
	                              "ducklake_branching_dropped_file",
	                              "ducklake_branching_commit",
	                              "ducklake_branching_name"};
	for (auto &table : branch_tables) {
		query += StringUtil::Format("DELETE FROM {METADATA_CATALOG}.%s WHERE branch_id = %d;\n", table, id);
	}
	RunBranchQuery(transaction, std::move(query), "Failed to drop DuckLake branch: ");
}

//===--------------------------------------------------------------------===//
// Load
//===--------------------------------------------------------------------===//
struct MainFileReference {
	TableIndex table_id;
	string path;
	idx_t row_count = 0;
	idx_t file_size_bytes = 0;
};

static unordered_map<idx_t, MainFileReference> ResolveMainFiles(DuckLakeTransaction &transaction,
                                                                const set<idx_t> &file_ids,
                                                                DuckLakeSnapshot fork_snapshot,
                                                                const DuckLakeBranchInfo &branch) {
	unordered_map<idx_t, MainFileReference> result;
	if (file_ids.empty()) {
		return result;
	}
	string id_list;
	for (auto &id : file_ids) {
		if (!id_list.empty()) {
			id_list += ", ";
		}
		id_list += to_string(id);
	}
	auto query_result = RunBranchQuery(
	    transaction,
	    StringUtil::Format("SELECT data_file_id, table_id, path, path_is_relative, record_count, file_size_bytes FROM "
	                       "{METADATA_CATALOG}.ducklake_data_file WHERE data_file_id IN (%s) AND %d >= begin_snapshot "
	                       "AND (%d < end_snapshot OR end_snapshot IS NULL)",
	                       id_list, fork_snapshot.snapshot_id, fork_snapshot.snapshot_id),
	    "Failed to read the main data files of a DuckLake branch: ");
	auto &catalog = transaction.GetCatalog();
	unordered_map<idx_t, string> table_paths;
	for (auto &row : *query_result) {
		auto file_id = row.GetValue<idx_t>(0);
		TableIndex table_id(row.GetValue<idx_t>(1));
		auto table_path = table_paths.find(table_id.index);
		if (table_path == table_paths.end()) {
			auto entry = catalog.GetEntryById(transaction, fork_snapshot, table_id);
			if (!entry) {
				throw InvalidInputException("Branch \"%s\" references table %d which does not exist at its fork",
				                            branch.name, table_id.index);
			}
			table_path = table_paths.emplace(table_id.index, entry->Cast<DuckLakeTableEntry>().DataPath()).first;
		}
		MainFileReference reference;
		reference.table_id = table_id;
		reference.path = LoadBranchPath(catalog, table_path->second, row.GetValue<string>(2), row.GetValue<bool>(3));
		reference.row_count = row.GetValue<idx_t>(4);
		reference.file_size_bytes = row.GetValue<idx_t>(5);
		result.emplace(file_id, std::move(reference));
	}
	for (auto &id : file_ids) {
		if (result.find(id) == result.end()) {
			throw InvalidInputException("Branch \"%s\" references data file %d, which is no longer visible at its fork "
			                            "snapshot %d",
			                            branch.name, id, fork_snapshot.snapshot_id);
		}
	}
	return result;
}

static DuckLakeDeleteFile ReadDeleteFileRow(DuckLakeCatalog &catalog, const vector<Value> &row) {
	DuckLakeDeleteFile delete_file;
	delete_file.file_name =
	    LoadBranchPath(catalog, catalog.DataPath(), row[4].GetValue<string>(), row[5].GetValue<bool>());
	delete_file.format = DeleteFileFormatFromString(row[6].GetValue<string>());
	delete_file.delete_count = row[7].GetValue<idx_t>();
	delete_file.file_size_bytes = row[8].GetValue<idx_t>();
	delete_file.footer_size = row[9].IsNull() ? 0 : row[9].GetValue<idx_t>();
	delete_file.row_group_count = OptionalIndex(row[10]);
	delete_file.encryption_key = row[11].IsNull() ? string() : Blob::FromBase64(row[11].GetValue<string>());
	return delete_file;
}

void DuckLakeBranchManager::LoadBranch(DuckLakeTransaction &transaction, const DuckLakeBranchInfo &branch,
                                       DuckLakeSnapshot fork_snapshot, DuckLakeLoadedBranch &loaded) {
	auto &catalog = transaction.GetCatalog();
	auto &state = *transaction.state;
	auto &local_changes = state.local_changes;
	loaded.info = branch;
	auto head = branch.head_seq;
	auto visible = StringUtil::Format("branch_id = %d AND begin_seq <= %d AND (end_seq IS NULL OR end_seq > %d)",
	                                  branch.branch_id, head, head);

	// the branch's own data files, in the order they were written so row ids stay stable
	map<TableIndex, vector<DuckLakeDataFile>> files_per_table;
	unordered_map<idx_t, pair<TableIndex, idx_t>> file_positions;
	auto data_result = RunBranchQuery(
	    transaction,
	    "SELECT branch_file_id, table_id, path, path_is_relative, record_count, file_size_bytes, footer_size, "
	    "row_group_count, partition_id, encryption_key FROM {METADATA_CATALOG}.ducklake_branching_data_file WHERE " +
	        visible + " ORDER BY branch_file_id",
	    "Failed to read DuckLake branch data files: ");
	for (auto &row : *data_result) {
		auto file_id = row.GetValue<idx_t>(0);
		TableIndex table_id(row.GetValue<idx_t>(1));
		DuckLakeDataFile file;
		file.file_name = LoadBranchPath(catalog, catalog.DataPath(), row.GetValue<string>(2), row.GetValue<bool>(3));
		file.row_count = row.GetValue<idx_t>(4);
		file.file_size_bytes = row.GetValue<idx_t>(5);
		file.footer_size = OptionalIndex(row.GetBaseValue(6));
		file.row_group_count = OptionalIndex(row.GetBaseValue(7));
		file.partition_id = OptionalIndex(row.GetBaseValue(8));
		auto encryption_key = row.GetBaseValue(9);
		file.encryption_key = encryption_key.IsNull() ? string() : Blob::FromBase64(encryption_key.GetValue<string>());
		loaded.data_files[file.file_name] = file_id;
		auto &table_files = files_per_table[table_id];
		file_positions[file_id] = make_pair(table_id, table_files.size());
		table_files.push_back(std::move(file));
	}

	// delete files - on the branch's own files or on main's files
	struct MainDelete {
		TableIndex table_id;
		idx_t data_file_id;
		DuckLakeDeleteFile delete_file;
	};
	vector<MainDelete> main_deletes;
	set<idx_t> main_file_ids;
	auto delete_result = RunBranchQuery(
	    transaction,
	    "SELECT branch_file_id, table_id, target_data_file_id, target_branch_file_id, path, path_is_relative, format, "
	    "delete_count, file_size_bytes, footer_size, row_group_count, encryption_key FROM "
	    "{METADATA_CATALOG}.ducklake_branching_delete_file WHERE " +
	        visible + " ORDER BY branch_file_id",
	    "Failed to read DuckLake branch delete files: ");
	for (auto &row : *delete_result) {
		vector<Value> values;
		for (idx_t col = 0; col < 12; col++) {
			values.push_back(row.GetBaseValue(col));
		}
		auto delete_file = ReadDeleteFileRow(catalog, values);
		loaded.delete_files[delete_file.file_name] = values[0].GetValue<idx_t>();
		TableIndex table_id(values[1].GetValue<idx_t>());
		if (!values[3].IsNull()) {
			auto position = file_positions.find(values[3].GetValue<idx_t>());
			if (position == file_positions.end()) {
				throw InvalidInputException("Branch \"%s\" has a delete file for a missing branch data file",
				                            branch.name);
			}
			auto &target = files_per_table[position->second.first][position->second.second];
			if (!target.delete_files.empty()) {
				throw InvalidInputException("Branch \"%s\" has more than one active delete file for \"%s\"",
				                            branch.name, target.file_name);
			}
			delete_file.data_file_path = target.file_name;
			target.delete_files.push_back(std::move(delete_file));
		} else {
			auto data_file_id = values[2].GetValue<idx_t>();
			main_file_ids.insert(data_file_id);
			main_deletes.push_back(MainDelete {table_id, data_file_id, std::move(delete_file)});
		}
	}

	// main files that were fully deleted on the branch
	vector<pair<TableIndex, idx_t>> dropped;
	auto dropped_result = RunBranchQuery(
	    transaction,
	    StringUtil::Format("SELECT table_id, data_file_id FROM {METADATA_CATALOG}.ducklake_branching_dropped_file "
	                       "WHERE branch_id = %d AND begin_seq <= %d",
	                       branch.branch_id, head),
	    "Failed to read DuckLake branch dropped files: ");
	for (auto &row : *dropped_result) {
		auto data_file_id = row.GetValue<idx_t>(1);
		dropped.emplace_back(TableIndex(row.GetValue<idx_t>(0)), data_file_id);
		main_file_ids.insert(data_file_id);
	}

	auto main_files = ResolveMainFiles(transaction, main_file_ids, fork_snapshot, branch);
	unordered_set<string> main_delete_targets;
	for (auto &main_delete : main_deletes) {
		auto &reference = main_files[main_delete.data_file_id];
		if (!main_delete_targets.insert(reference.path).second) {
			throw InvalidInputException("Branch \"%s\" has more than one active delete file for \"%s\"", branch.name,
			                            reference.path);
		}
		auto &delete_file = main_delete.delete_file;
		delete_file.data_file_id = DataFileIndex(main_delete.data_file_id);
		delete_file.data_file_path = reference.path;
		delete_file.created_by_ducklake = false;
		vector<DuckLakeDeleteFile> delete_files;
		delete_files.push_back(std::move(delete_file));
		local_changes.AppendDeleteFiles(main_delete.table_id, reference.path, std::move(delete_files));
	}
	for (auto &entry : dropped) {
		auto &reference = main_files[entry.second];
		transaction.DropFile(entry.first, DataFileIndex(entry.second), reference.path, reference.row_count,
		                     reference.file_size_bytes);
		loaded.dropped_files.emplace(entry.second, entry.first);
	}
	for (auto &entry : files_per_table) {
		for (auto &file : entry.second) {
			file.created_by_ducklake = false;
			for (auto &delete_file : file.delete_files) {
				delete_file.created_by_ducklake = false;
			}
		}
		local_changes.AppendFiles(entry.first, std::move(entry.second));
	}

	// deletes of main's inlined rows
	auto inlined_result = RunBranchQuery(
	    transaction,
	    StringUtil::Format(
	        "SELECT table_id, inlined_table_name, row_id FROM "
	        "{METADATA_CATALOG}.ducklake_branching_inlined_delete WHERE branch_id = %d AND begin_seq <= %d",
	        branch.branch_id, head),
	    "Failed to read DuckLake branch inlined deletes: ");
	for (auto &row : *inlined_result) {
		TableIndex table_id(row.GetValue<idx_t>(0));
		loaded.inlined_deletes[table_id][row.GetValue<string>(1)].insert(row.GetValue<idx_t>(2));
	}
	for (auto &table_entry : loaded.inlined_deletes) {
		for (auto &entry : table_entry.second) {
			local_changes.AddNewInlinedDeletes(table_entry.first, entry.first, entry.second);
		}
	}
}

//===--------------------------------------------------------------------===//
// Commit
//===--------------------------------------------------------------------===//
static string DeleteFileValues(DuckLakeMetadataManager &metadata_manager, idx_t file_id, idx_t branch_id,
                               TableIndex table_id, idx_t seq, const string &target_data_file,
                               const string &target_branch_file, const DuckLakeDeleteFile &delete_file) {
	auto path = metadata_manager.GetRelativePath(delete_file.file_name);
	return StringUtil::Format("(%d, %d, %d, %d, NULL, %s, %s, %s, %s, %s, %d, %d, %d, %s, %s)", file_id, branch_id,
	                          table_id.index, seq, target_data_file, target_branch_file,
	                          DuckLakeUtil::SQLLiteralToString(path.path), path.path_is_relative ? "true" : "false",
	                          DuckLakeUtil::SQLLiteralToString(DeleteFileFormatToString(delete_file.format)),
	                          delete_file.delete_count, delete_file.file_size_bytes, delete_file.footer_size,
	                          OptionalToSQL(delete_file.row_group_count),
	                          DuckLakeUtil::EncryptionKeyLiteral(delete_file.encryption_key));
}

static void AppendValues(string &target, const string &values) {
	if (!target.empty()) {
		target += ", ";
	}
	target += values;
}

static void AppendChange(string &changes, const string &kind, const set<TableIndex> &tables) {
	for (auto &table_id : tables) {
		if (!changes.empty()) {
			changes += ",";
		}
		changes += kind + ":" + to_string(table_id.index);
	}
}

static bool IsLockedError(const string &message) {
	return StringUtil::Contains(StringUtil::Lower(message), "database is locked");
}

void DuckLakeBranchManager::CommitBranch(DuckLakeTransaction &transaction, DuckLakeLoadedBranch &loaded) {
	auto &state = *transaction.state;
	auto &metadata_manager = transaction.GetMetadataManager();
	auto branch_id = loaded.info.branch_id;
	auto new_seq = loaded.info.head_seq + 1;
	auto next_file_seq = loaded.info.next_file_seq;
	auto next_file_id = [&]() {
		return BRANCH_FILE_ID_BASE + (branch_id << 32) + next_file_seq++;
	};

	string data_rows, stats_rows, partition_rows, delete_rows, inlined_rows, dropped_rows;
	set<TableIndex> inserted_tables, deleted_tables, inlined_deleted_tables;
	unordered_set<string> current_delete_files;
	for (auto &entry : state.local_changes.Changes()) {
		auto table_id = entry.GetTableIndex();
		auto &changes = entry.GetTableChanges();
		if (changes.new_inlined_data || changes.new_inlined_file_deletes || !changes.compactions.empty()) {
			throw InternalException("Inlined data and compactions cannot be committed to a branch");
		}
		for (auto &file : changes.new_data_files) {
			idx_t file_id;
			auto loaded_entry = loaded.data_files.find(file.file_name);
			if (loaded_entry == loaded.data_files.end()) {
				file_id = next_file_id();
				inserted_tables.insert(table_id);
				auto path = metadata_manager.GetRelativePath(file.file_name);
				AppendValues(data_rows,
				             StringUtil::Format("(%d, %d, %d, %d, NULL, %s, %s, 'parquet', %d, %d, %s, %s, %s, %s)",
				                                file_id, branch_id, table_id.index, new_seq,
				                                DuckLakeUtil::SQLLiteralToString(path.path),
				                                path.path_is_relative ? "true" : "false", file.row_count,
				                                file.file_size_bytes, OptionalToSQL(file.footer_size),
				                                OptionalToSQL(file.row_group_count), OptionalToSQL(file.partition_id),
				                                DuckLakeUtil::EncryptionKeyLiteral(file.encryption_key)));
				for (auto &stats_entry : file.column_stats) {
					auto stats = DuckLakeColumnStatsInfo::FromColumnStats(stats_entry.first, stats_entry.second);
					AppendValues(stats_rows,
					             StringUtil::Format("(%d, %d, %d, %d, %s, %s, %s, %s, %s, %s, %s, %s, %s)", file_id,
					                                branch_id, table_id.index, stats_entry.first.index,
					                                stats.column_size_bytes, stats.value_count, stats.null_count,
					                                stats.min_val, stats.max_val, stats.contains_nan, stats.extra_stats,
					                                stats.min_is_exact, stats.max_is_exact));
				}
				for (auto &partition : file.partition_values) {
					auto value = partition.partition_value.IsNull()
					                 ? string("NULL")
					                 : DuckLakeUtil::SQLLiteralToString(partition.partition_value.ToString());
					AppendValues(partition_rows,
					             StringUtil::Format("(%d, %d, %d, %d, %s)", file_id, branch_id, table_id.index,
					                                partition.partition_column_idx, value));
				}
			} else {
				file_id = loaded_entry->second;
			}
			for (auto &delete_file : file.delete_files) {
				current_delete_files.insert(delete_file.file_name);
				if (loaded.delete_files.find(delete_file.file_name) == loaded.delete_files.end()) {
					deleted_tables.insert(table_id);
					AppendValues(delete_rows, DeleteFileValues(metadata_manager, next_file_id(), branch_id, table_id,
					                                           new_seq, "NULL", to_string(file_id), delete_file));
				}
			}
		}
		for (auto &delete_entry : changes.new_delete_files) {
			if (state.dropped_files.find(delete_entry.first) != state.dropped_files.end()) {
				// the main file was fully deleted on the branch - its delete files go with it
				continue;
			}
			for (auto &delete_file : delete_entry.second) {
				current_delete_files.insert(delete_file.file_name);
				if (loaded.delete_files.find(delete_file.file_name) != loaded.delete_files.end()) {
					continue;
				}
				if (!delete_file.data_file_id.IsValid()) {
					throw InternalException("Branch delete file on a main file without a data file id");
				}
				deleted_tables.insert(table_id);
				AppendValues(delete_rows,
				             DeleteFileValues(metadata_manager, next_file_id(), branch_id, table_id, new_seq,
				                              to_string(delete_file.data_file_id.index), "NULL", delete_file));
			}
		}
		for (auto &inlined_entry : changes.new_inlined_data_deletes) {
			auto &loaded_rows = loaded.inlined_deletes[table_id][inlined_entry.first];
			for (auto &row_id : inlined_entry.second->rows) {
				if (loaded_rows.find(row_id) != loaded_rows.end()) {
					continue;
				}
				inlined_deleted_tables.insert(table_id);
				AppendValues(inlined_rows,
				             StringUtil::Format("(%d, %d, %d, %s, %d)", branch_id, new_seq, table_id.index,
				                                DuckLakeUtil::SQLLiteralToString(inlined_entry.first), row_id));
			}
		}
	}
	vector<idx_t> ended_delete_files;
	for (auto &loaded_delete : loaded.delete_files) {
		if (current_delete_files.find(loaded_delete.first) == current_delete_files.end()) {
			ended_delete_files.push_back(loaded_delete.second);
		}
	}
	set<idx_t> new_dropped_files;
	for (auto &dropped : state.dropped_files) {
		auto data_file_id = dropped.second.index;
		if (loaded.dropped_files.find(data_file_id) == loaded.dropped_files.end()) {
			new_dropped_files.insert(data_file_id);
		}
	}
	auto dropped_main_files = ResolveMainFiles(transaction, new_dropped_files, transaction.GetSnapshot(), loaded.info);
	for (auto &data_file_id : new_dropped_files) {
		auto table_id = dropped_main_files[data_file_id].table_id;
		deleted_tables.insert(table_id);
		AppendValues(dropped_rows,
		             StringUtil::Format("(%d, %d, %d, %d)", branch_id, new_seq, table_id.index, data_file_id));
	}

	auto &connection = transaction.GetConnection();
	if (data_rows.empty() && delete_rows.empty() && inlined_rows.empty() && dropped_rows.empty() &&
	    ended_delete_files.empty()) {
		// nothing new on the branch
		connection.Commit();
		return;
	}
	state.EnsureCommitInfoProvided(state.commit_info);

	string changes_made;
	AppendChange(changes_made, "inserted_into_table", inserted_tables);
	AppendChange(changes_made, "deleted_from_table", deleted_tables);
	AppendChange(changes_made, "inlined_delete", inlined_deleted_tables);

	string batch;
	batch += StringUtil::Format(
	    "INSERT INTO {METADATA_CATALOG}.ducklake_branching_commit VALUES (%d, %d, NOW(), %s, %s, %s, %s);\n", branch_id,
	    new_seq, state.commit_info.author.ToSQLString(), state.commit_info.commit_message.ToSQLString(),
	    state.commit_info.commit_extra_info.ToSQLString(), DuckLakeUtil::SQLLiteralToString(changes_made));
	if (!data_rows.empty()) {
		batch += "INSERT INTO {METADATA_CATALOG}.ducklake_branching_data_file (branch_file_id, branch_id, table_id, "
		         "begin_seq, end_seq, path, path_is_relative, file_format, record_count, file_size_bytes, footer_size, "
		         "row_group_count, partition_id, encryption_key) VALUES " +
		         data_rows + ";\n";
	}
	if (!stats_rows.empty()) {
		batch += "INSERT INTO {METADATA_CATALOG}.ducklake_branching_file_column_stats VALUES " + stats_rows + ";\n";
	}
	if (!partition_rows.empty()) {
		batch +=
		    "INSERT INTO {METADATA_CATALOG}.ducklake_branching_file_partition_value VALUES " + partition_rows + ";\n";
	}
	if (!delete_rows.empty()) {
		batch += "INSERT INTO {METADATA_CATALOG}.ducklake_branching_delete_file (branch_file_id, branch_id, table_id, "
		         "begin_seq, end_seq, target_data_file_id, target_branch_file_id, path, path_is_relative, format, "
		         "delete_count, file_size_bytes, footer_size, row_group_count, encryption_key) VALUES " +
		         delete_rows + ";\n";
	}
	if (!ended_delete_files.empty()) {
		string id_list;
		for (auto &id : ended_delete_files) {
			if (!id_list.empty()) {
				id_list += ", ";
			}
			id_list += to_string(id);
		}
		batch += StringUtil::Format(
		    "INSERT INTO {METADATA_CATALOG}.ducklake_files_scheduled_for_deletion SELECT branch_file_id, path, "
		    "path_is_relative, NOW() FROM {METADATA_CATALOG}.ducklake_branching_delete_file WHERE branch_file_id IN "
		    "(%s);\nDELETE FROM {METADATA_CATALOG}.ducklake_branching_delete_file WHERE branch_file_id IN (%s);\n",
		    id_list, id_list);
	}
	if (!inlined_rows.empty()) {
		batch += "INSERT INTO {METADATA_CATALOG}.ducklake_branching_inlined_delete VALUES " + inlined_rows + ";\n";
	}
	if (!dropped_rows.empty()) {
		batch += "INSERT INTO {METADATA_CATALOG}.ducklake_branching_dropped_file VALUES " + dropped_rows + ";\n";
	}

	// status is written as well: DuckDB detects update conflicts per column, and DROP BRANCH changes status
	auto advance_head = StringUtil::Format("UPDATE {METADATA_CATALOG}.ducklake_branching_branch SET head_seq = %d, "
	                                       "next_file_seq = %d, status = 'active' WHERE branch_id = %d "
	                                       "AND head_seq = %d AND status = 'active'",
	                                       new_seq, next_file_seq, branch_id, loaded.info.head_seq);
	auto changed_error =
	    StringUtil::Format("Branch \"%s\" was changed or dropped by another transaction - retry", loaded.info.name);

	auto retry_config = DuckLakeRetryConfig();
	auto context_ref = transaction.context.lock();
	if (context_ref) {
		retry_config = DuckLakeRetryConfig::FromContext(*context_ref);
	}
	for (idx_t attempt = 0;; attempt++) {
		try {
			auto update_result = transaction.Query(advance_head);
			if (update_result->HasError()) {
				update_result->GetErrorObject().Throw(changed_error + ": ");
			}
			idx_t updated_rows = 0;
			for (auto &row : *update_result) {
				updated_rows = row.GetValue<idx_t>(0);
			}
			if (updated_rows != 1) {
				throw TransactionException(changed_error);
			}
			auto write_result = transaction.Query(batch);
			if (write_result->HasError()) {
				write_result->GetErrorObject().Throw(changed_error + ": ");
			}
			connection.Commit();
			return;
		} catch (std::exception &ex) {
			ErrorData error(ex);
			try {
				connection.Rollback();
			} catch (...) {
			}
			if (attempt < retry_config.max_retry_count && IsLockedError(error.RawMessage())) {
				auto wait_ms = static_cast<int64_t>(retry_config.retry_wait_ms *
				                                    std::pow(retry_config.retry_backoff, static_cast<double>(attempt)));
				std::this_thread::sleep_for(std::chrono::milliseconds(wait_ms));
				connection.BeginTransaction();
				continue;
			}
			// a failed commit never reaches Rollback - remove the files this transaction wrote
			state.local_changes.CleanupFiles(transaction.db);
			if (error.Type() == ExceptionType::TRANSACTION) {
				error.Throw();
			}
			throw TransactionException("%s (%s)", changed_error, error.RawMessage());
		}
	}
}

//===--------------------------------------------------------------------===//
// Main-side protection
//===--------------------------------------------------------------------===//
string DuckLakeBranchManager::ActiveForkSnapshotsQuery() {
	return "SELECT fork_snapshot_id FROM {METADATA_CATALOG}.ducklake_branching_branch WHERE status = 'active'";
}

string DuckLakeBranchManager::ActiveBranchFilesQuery() {
	return R"(
SELECT CASE WHEN f.path_is_relative THEN {DATA_PATH} || f.path ELSE f.path END
FROM {METADATA_CATALOG}.ducklake_branching_data_file f
JOIN {METADATA_CATALOG}.ducklake_branching_branch b ON f.branch_id = b.branch_id
WHERE b.status = 'active'
UNION ALL
SELECT CASE WHEN f.path_is_relative THEN {DATA_PATH} || f.path ELSE f.path END
FROM {METADATA_CATALOG}.ducklake_branching_delete_file f
JOIN {METADATA_CATALOG}.ducklake_branching_branch b ON f.branch_id = b.branch_id
WHERE b.status = 'active')";
}

vector<idx_t> DuckLakeBranchManager::GetActiveForkSnapshots(DuckLakeTransaction &transaction) {
	vector<idx_t> result;
	if (!HasBranchTables(transaction)) {
		return result;
	}
	auto query_result =
	    RunBranchQuery(transaction, ActiveForkSnapshotsQuery(), "Failed to read DuckLake branch fork snapshots: ");
	for (auto &row : *query_result) {
		result.push_back(row.GetValue<idx_t>(0));
	}
	return result;
}

} // namespace duckdb
