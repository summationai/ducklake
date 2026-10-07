#include "branching/ducklake_branch.hpp"

#include "branching/ducklake_branch_util.hpp"
#include "common/ducklake_util.hpp"
#include "duckdb/common/types/blob.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/transaction/transaction_context.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/planner/tableref/bound_at_clause.hpp"
#include "storage/ducklake_catalog.hpp"
#include "storage/ducklake_delete.hpp"
#include "storage/ducklake_delete_filter.hpp"
#include "storage/ducklake_metadata_info.hpp"
#include "storage/ducklake_schema_entry.hpp"
#include "storage/ducklake_transaction_changes.hpp"
#include "storage/ducklake_metadata_manager.hpp"
#include "storage/ducklake_table_entry.hpp"
#include "storage/ducklake_transaction.hpp"
#include "storage/ducklake_transaction_state.hpp"
#include "storage/ducklake_view_entry.hpp"

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
static string OptionalToSQL(const optional_idx &value) {
	return value.IsValid() ? to_string(value.GetIndex()) : "NULL";
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
	RunBranchQuery(transaction, string(BRANCH_TABLES_SQL) + DefinitionTablesSql(),
	               "Failed to create DuckLake branch tables: ");
	SetHasBranchTables(transaction, true);
	SetHasDefinitionTables(transaction, true);
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
			SetHasDefinitionTables(transaction, false);
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
	if (HasDefinitionTables(transaction)) {
		branch_tables.insert(branch_tables.end(), {"ducklake_branching_object", "ducklake_branching_column",
		                                           "ducklake_branching_main_change"});
	}
	if (HasColumnChangeTable(transaction)) {
		branch_tables.push_back("ducklake_branching_column_change");
	}
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

//! Column stats and partition values of the branch's files - only a merge needs them
static void LoadFileDetails(DuckLakeTransaction &transaction, const DuckLakeBranchInfo &branch,
                            DuckLakeSnapshot fork_snapshot, map<TableIndex, vector<DuckLakeDataFile>> &files_per_table,
                            const unordered_map<idx_t, pair<TableIndex, idx_t>> &file_positions) {
	auto partition_result = RunBranchQuery(
	    transaction,
	    StringUtil::Format("SELECT branch_file_id, partition_key_index, partition_value FROM "
	                       "{METADATA_CATALOG}.ducklake_branching_file_partition_value WHERE branch_id = %d "
	                       "ORDER BY branch_file_id, partition_key_index",
	                       branch.branch_id),
	    "Failed to read DuckLake branch partition values: ");
	for (auto &row : *partition_result) {
		auto position = file_positions.find(row.GetValue<idx_t>(0));
		if (position == file_positions.end()) {
			continue;
		}
		DuckLakeFilePartition partition;
		partition.partition_column_idx = row.GetValue<idx_t>(1);
		partition.partition_value = row.IsNull(2) ? Value() : Value(row.GetValue<string>(2));
		files_per_table[position->second.first][position->second.second].partition_values.push_back(
		    std::move(partition));
	}

	map<TableIndex, optional_ptr<CatalogEntry>> table_entries;
	auto stats_result = RunBranchQuery(
	    transaction,
	    StringUtil::Format("SELECT branch_file_id, column_id, value_count, null_count, min_value, max_value, "
	                       "contains_nan, extra_stats, min_is_exact, max_is_exact, column_size_bytes FROM "
	                       "{METADATA_CATALOG}.ducklake_branching_file_column_stats WHERE branch_id = %d",
	                       branch.branch_id),
	    "Failed to read DuckLake branch file statistics: ");
	for (auto &row : *stats_result) {
		auto position = file_positions.find(row.GetValue<idx_t>(0));
		if (position == file_positions.end()) {
			continue;
		}
		auto table_id = position->second.first;
		auto table_entry = table_entries.find(table_id);
		if (table_entry == table_entries.end()) {
			table_entry =
			    table_entries
			        .emplace(table_id, DuckLakeBranchManager::CurrentTableEntry(transaction, fork_snapshot, table_id))
			        .first;
		}
		if (!table_entry->second) {
			continue;
		}
		auto &table = table_entry->second->Cast<DuckLakeTableEntry>();
		FieldIndex field_index(row.GetValue<idx_t>(1));
		auto field_id = table.GetFieldData().GetByFieldIndex(field_index);
		if (!field_id) {
			continue;
		}
		DuckLakeColumnStats col_stats(field_id->Type());
		if (!row.IsNull(2) && !row.IsNull(3)) {
			auto value_count = row.GetValue<idx_t>(2);
			auto null_count = row.GetValue<idx_t>(3);
			col_stats.has_num_values = true;
			col_stats.num_values = value_count + null_count;
			col_stats.has_null_count = true;
			col_stats.null_count = null_count;
		}
		if (!row.IsNull(4)) {
			col_stats.has_min = true;
			col_stats.min = row.GetValue<string>(4);
		}
		if (!row.IsNull(5)) {
			col_stats.has_max = true;
			col_stats.max = row.GetValue<string>(5);
		}
		if (!row.IsNull(6)) {
			col_stats.has_contains_nan = true;
			col_stats.contains_nan = row.GetValue<bool>(6);
		}
		if (!row.IsNull(7) && col_stats.extra_stats) {
			col_stats.extra_stats->Deserialize(row.GetValue<string>(7));
		}
		col_stats.min_is_exact = !row.IsNull(8) && row.GetValue<bool>(8);
		col_stats.max_is_exact = !row.IsNull(9) && row.GetValue<bool>(9);
		if (!row.IsNull(10)) {
			col_stats.column_size_bytes = row.GetValue<idx_t>(10);
		}
		auto &file = files_per_table[table_id][position->second.second];
		file.column_stats.emplace(field_index, std::move(col_stats));
	}
}

void DuckLakeBranchManager::LoadBranch(DuckLakeTransaction &transaction, const DuckLakeBranchInfo &branch,
                                       DuckLakeSnapshot fork_snapshot, DuckLakeLoadedBranch &loaded, bool for_merge) {
	auto &catalog = transaction.GetCatalog();
	auto &state = *transaction.state;
	auto &local_changes = state.local_changes;
	loaded.info = branch;
	// the branch's catalog changes come first: its files can belong to tables it created
	LoadDefinitions(transaction, branch, fork_snapshot, loaded);
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
		auto table_id = LocalTableId(loaded, row.GetValue<idx_t>(1), branch.name);
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
		auto table_id = LocalTableId(loaded, values[1].GetValue<idx_t>(), branch.name);
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
		loaded.main_deletes.push_back(DuckLakeLoadedBranch::MainDelete {main_delete.table_id, main_delete.data_file_id,
		                                                                reference.path, delete_file.file_name});
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
	if (for_merge) {
		LoadFileDetails(transaction, branch, fork_snapshot, files_per_table, file_positions);
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
	std::function<idx_t()> next_file_id = [&]() {
		return BRANCH_FILE_ID_BASE + (branch_id << 32) + next_file_seq++;
	};
	// catalog changes first: they give the tables the branch created their ids
	auto definitions = WriteDefinitions(transaction, loaded, new_seq, next_file_id);
	auto context_ref = transaction.context.lock();
	if (context_ref) {
		for (auto &table_id : state.dropped_tables) {
			// a main table dropped on the branch keeps none of the branch's files: this transaction's are removed,
			// the ones of earlier commits are ended below
			state.local_changes.CleanupFiles(*context_ref, table_id);
		}
	}

	string data_rows, stats_rows, partition_rows, delete_rows, inlined_rows, dropped_rows;
	set<TableIndex> inserted_tables, deleted_tables, inlined_deleted_tables;
	unordered_set<string> current_data_files, current_delete_files;
	for (auto &entry : state.local_changes.Changes()) {
		TableIndex table_id(StoredTableId(loaded, entry.GetTableIndex()));
		auto &changes = entry.GetTableChanges();
		if (changes.new_inlined_data || changes.new_inlined_file_deletes || !changes.compactions.empty()) {
			throw InternalException("Inlined data and compactions cannot be committed to a branch");
		}
		for (auto &file : changes.new_data_files) {
			idx_t file_id;
			current_data_files.insert(file.file_name);
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
	// files of earlier commits whose table was dropped
	vector<idx_t> ended_data_files;
	for (auto &loaded_file : loaded.data_files) {
		if (current_data_files.find(loaded_file.first) == current_data_files.end()) {
			ended_data_files.push_back(loaded_file.second);
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
		if (state.dropped_tables.find(table_id) != state.dropped_tables.end()) {
			continue;
		}
		deleted_tables.insert(table_id);
		AppendValues(dropped_rows,
		             StringUtil::Format("(%d, %d, %d, %d)", branch_id, new_seq, table_id.index, data_file_id));
	}

	auto &connection = transaction.GetConnection();
	if (data_rows.empty() && delete_rows.empty() && inlined_rows.empty() && dropped_rows.empty() &&
	    ended_data_files.empty() && ended_delete_files.empty() && !definitions.HasChanges()) {
		// nothing new on the branch
		connection.Commit();
		return;
	}
	state.EnsureCommitInfoProvided(state.commit_info);

	string changes_made;
	AppendChange(changes_made, "inserted_into_table", inserted_tables);
	AppendChange(changes_made, "deleted_from_table", deleted_tables);
	AppendChange(changes_made, "inlined_delete", inlined_deleted_tables);
	if (!definitions.changes_made.empty()) {
		changes_made += (changes_made.empty() ? "" : ",") + definitions.changes_made;
	}

	string batch;
	batch += StringUtil::Format(
	    "INSERT INTO {METADATA_CATALOG}.ducklake_branching_commit VALUES (%d, %d, NOW(), %s, %s, %s, %s);\n", branch_id,
	    new_seq, state.commit_info.author.ToSQLString(), state.commit_info.commit_message.ToSQLString(),
	    state.commit_info.commit_extra_info.ToSQLString(), DuckLakeUtil::SQLLiteralToString(changes_made));
	batch += definitions.batch;
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
	if (!ended_data_files.empty()) {
		auto id_list = BranchIdList(ended_data_files);
		batch += StringUtil::Format(
		    "INSERT INTO {METADATA_CATALOG}.ducklake_files_scheduled_for_deletion SELECT branch_file_id, path, "
		    "path_is_relative, NOW() FROM {METADATA_CATALOG}.ducklake_branching_data_file WHERE branch_file_id IN "
		    "(%s);\n",
		    id_list);
		for (auto table : {"ducklake_branching_data_file", "ducklake_branching_file_column_stats",
		                   "ducklake_branching_file_partition_value"}) {
			batch +=
			    StringUtil::Format("DELETE FROM {METADATA_CATALOG}.%s WHERE branch_file_id IN (%s);\n", table, id_list);
		}
	}
	if (!ended_delete_files.empty()) {
		auto id_list = BranchIdList(ended_delete_files);
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
			if (definitions.creates_tables) {
				SetHasDefinitionTables(transaction, true);
			}
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
// Merge
//===--------------------------------------------------------------------===//
static void EnsureSnapshotsSinceFork(DuckLakeTransaction &transaction, const DuckLakeBranchInfo &branch) {
	auto fork = branch.fork_snapshot_id;
	auto result = RunBranchQuery(transaction,
	                             StringUtil::Format("SELECT COUNT(*), COALESCE(MAX(snapshot_id), %d) FROM "
	                                                "{METADATA_CATALOG}.ducklake_snapshot WHERE snapshot_id > %d",
	                                                fork, fork),
	                             "Failed to read the main snapshots since the fork of a DuckLake branch: ");
	for (auto &row : *result) {
		auto count = row.GetValue<idx_t>(0);
		auto head = row.GetValue<idx_t>(1);
		if (count != head - fork) {
			throw InvalidInputException("Cannot merge branch \"%s\": main snapshots since its fork (%d) were expired, "
			                            "so conflicts cannot be checked",
			                            branch.name, fork);
		}
	}
}

//! Branch delete files on main files hold main's deletes at the fork plus the branch's own. Where main has its own
//! delete file or inlined deletions for the data file, rewrite them the way a DELETE on main would.
namespace {

//! Main's deletes on one of its data files at the fork, and what the branch deleted from the file on top of them
struct MainFileDeletes {
	bool has_main_delete = false;
	bool has_inlined = false;
	//! Main's positions, each with the snapshot that deleted it
	set<PositionWithSnapshot> main_positions;
	idx_t inlined_count = 0;
	//! The positions only the branch deleted
	set<idx_t> branch_only;

	idx_t DeletedCount() const {
		return main_positions.size() + inlined_count + branch_only.size();
	}
};

void LoadMainDeletes(ClientContext &context, const DuckLakeFileListExtendedEntry &main_file,
                     optional_ptr<const set<idx_t>> inlined_positions, DuckLakeSnapshot fork_snapshot,
                     MainFileDeletes &result) {
	result.has_main_delete = main_file.delete_file_id.IsValid();
	result.has_inlined = inlined_positions && !inlined_positions->empty();
	if (result.has_main_delete) {
		auto main_scan = DuckLakeDeleteFilter::ScanDeleteFile(context, main_file.delete_file);
		auto fallback_snapshot = main_file.delete_file_begin_snapshot.IsValid()
		                             ? main_file.delete_file_begin_snapshot.GetIndex()
		                             : fork_snapshot.snapshot_id;
		MergeDeletesWithSnapshots(main_scan, fallback_snapshot, result.main_positions);
	}
	if (result.has_inlined) {
		result.inlined_count = inlined_positions->size();
	}
}

//! Separates the branch's deletes on a main file from main's own deletes at the fork. When main had none, the
//! branch's delete file holds exactly the branch's deletes; it is read only when the caller needs the positions.
MainFileDeletes AnalyseMainDelete(ClientContext &context, LocalTableChanges &local_changes, TableIndex table_id,
                                  const DuckLakeLoadedBranch::MainDelete &main_delete,
                                  const DuckLakeFileListExtendedEntry &main_file,
                                  optional_ptr<const set<idx_t>> inlined_positions, DuckLakeSnapshot fork_snapshot,
                                  bool read_unchanged) {
	MainFileDeletes result;
	LoadMainDeletes(context, main_file, inlined_positions, fork_snapshot, result);
	if (!result.has_main_delete && !result.has_inlined && !read_unchanged) {
		return result;
	}
	DuckLakeFileData branch_file_data;
	local_changes.GetLocalDeleteForFile(table_id, main_delete.data_file_path, branch_file_data);
	auto branch_scan = DuckLakeDeleteFilter::ScanDeleteFile(context, branch_file_data);
	result.branch_only.insert(branch_scan.deleted_rows.begin(), branch_scan.deleted_rows.end());
	// main's cumulative delete files are back-dated, so this may also remove positions main deleted after the fork -
	// those rows are gone on main either way
	for (auto &position : result.main_positions) {
		result.branch_only.erase(static_cast<idx_t>(position.position));
	}
	if (result.has_inlined) {
		for (auto &position : *inlined_positions) {
			result.branch_only.erase(position);
		}
	}
	return result;
}

optional_ptr<const set<idx_t>> FindInlinedPositions(const map<idx_t, set<idx_t>> &inlined_deletions, idx_t file_id) {
	auto entry = inlined_deletions.find(file_id);
	if (entry == inlined_deletions.end()) {
		return nullptr;
	}
	return &entry->second;
}

DuckLakeTableEntry &GetTableAtFork(DuckLakeTransaction &transaction, DuckLakeSnapshot fork_snapshot,
                                   TableIndex table_id, const string &branch_name) {
	auto entry = DuckLakeBranchManager::GetTableEntry(transaction, fork_snapshot, table_id);
	if (!entry) {
		throw InvalidInputException("Branch \"%s\" changed table %d, which does not exist at its fork", branch_name,
		                            table_id.index);
	}
	return entry->Cast<DuckLakeTableEntry>();
}

unique_ptr<DuckLakeSnapshot> GetForkSnapshot(DuckLakeTransaction &transaction, const DuckLakeBranchInfo &branch) {
	BoundAtClause fork_clause(Identifier("version"), Value::UBIGINT(branch.fork_snapshot_id));
	try {
		return transaction.GetMetadataManager().GetSnapshot(fork_clause, SnapshotBound::UPPER_BOUND);
	} catch (InvalidInputException &) {
		throw InvalidInputException("Cannot merge branch \"%s\": its fork snapshot %d was expired", branch.name,
		                            branch.fork_snapshot_id);
	}
}

//! The conflict DuckLake's own rules miss: main deleting rows inline from a file the branch deleted from would leave
//! duplicate positions behind
void CheckMergeOnlyConflicts(const string &branch_name, const set<TableIndex> &deleted_from,
                             const SnapshotChangeInformation &other_changes) {
	for (auto &table_id : deleted_from) {
		if (other_changes.tables_deleted_inlined.find(table_id) != other_changes.tables_deleted_inlined.end()) {
			throw TransactionException("Transaction conflict - branch \"%s\" deleted from table %d, but main deleted "
			                           "inlined rows from it since the fork",
			                           branch_name, table_id.index);
		}
	}
}

} // namespace

void DuckLakeBranchManager::RebaseMainDeletes(DuckLakeTransaction &transaction, DuckLakeBranchMerge &merge) {
	auto &loaded = merge.loaded;
	// per table and main file: the branch's delete file on it, and main's copies of rows the branch's changes win for
	struct FileDeletes {
		optional_ptr<const DuckLakeLoadedBranch::MainDelete> branch_delete;
		vector<pair<idx_t, int64_t>> replaced_rows;
	};
	map<TableIndex, map<string, FileDeletes>> per_table;
	for (auto &main_delete : loaded.main_deletes) {
		per_table[main_delete.table_id][main_delete.data_file_path].branch_delete = main_delete;
	}
	for (auto &table : merge.row_merge) {
		for (auto &file : table.second.main_rows_to_drop) {
			auto &replaced = per_table[table.first][file.first].replaced_rows;
			replaced.insert(replaced.end(), file.second.begin(), file.second.end());
		}
	}
	ForgetFilesMainEmptied(transaction, merge);
	if (per_table.empty()) {
		return;
	}
	auto context_ref = transaction.context.lock();
	auto &context = *context_ref;
	auto &catalog = transaction.GetCatalog();
	auto &metadata_manager = transaction.GetMetadataManager();
	auto &local_changes = transaction.state->local_changes;
	auto &fs = FileSystem::GetFileSystem(context);
	// the branch's deletes become visible at the merge snapshot - the next one, as a DELETE on main assumes
	auto merge_snapshot = metadata_manager.GetSnapshot()->snapshot_id + 1;
	// main's deletes are taken as they are at main's head, so every main file gets one delete file holding both sides'
	auto head = merge.head_snapshot;

	for (auto &table_entry : per_table) {
		auto table_id = table_entry.first;
		auto entry = GetTableEntry(transaction, head, table_id);
		if (!entry) {
			// main dropped the table, which the merge's conflict check reports - unless it never existed
			GetTableAtFork(transaction, merge.fork_snapshot, table_id, loaded.info.name);
			continue;
		}
		auto &table = entry->Cast<DuckLakeTableEntry>();
		auto &schema = table.ParentSchema().Cast<DuckLakeSchemaEntry>();
		bool use_deletion_vectors =
		    catalog.WriteDeletionVectors(schema.GetSchemaId(), table_id, &table.GetTableOptions());
		auto row_merged = merge.row_merge.find(table_id) != merge.row_merge.end();
		auto main_files = metadata_manager.GetExtendedFilesForTable(table, head, nullptr);
		unordered_map<string, reference<DuckLakeFileListExtendedEntry>> files_by_path;
		for (auto &file : main_files) {
			if (file.file_id.IsValid()) {
				files_by_path.emplace(file.file.path, file);
			}
		}
		auto inlined_deletions = metadata_manager.ReadInlinedFileDeletions(table_id, head);
		for (auto &file_entry : table_entry.second) {
			auto &data_file_path = file_entry.first;
			auto &file_deletes = file_entry.second;
			auto branch_delete = file_deletes.branch_delete;
			auto main_file_entry = files_by_path.find(data_file_path);
			if (main_file_entry == files_by_path.end()) {
				if (!file_deletes.replaced_rows.empty()) {
					// main's copies held inlined: deleted by row id
					set<idx_t> row_ids;
					for (auto &row : file_deletes.replaced_rows) {
						row_ids.insert(static_cast<idx_t>(row.second));
					}
					local_changes.AddNewInlinedDeletes(table_id, data_file_path, std::move(row_ids));
				}
				if (branch_delete && row_merged) {
					// main deleted the whole file since the fork; the row-by-row merge found every row the branch
					// deleted from it deleted on main too
					merge.files_to_schedule.emplace_back(loaded.delete_files[branch_delete->branch_delete_file],
					                                     branch_delete->branch_delete_file);
					ForgetDeleteFiles(transaction, table_id, data_file_path);
				}
				// otherwise main compacted the file, which the merge's conflict check reports
				continue;
			}
			auto &main_file = main_file_entry->second.get();
			auto inlined_positions = FindInlinedPositions(inlined_deletions, main_file.file_id.index);
			MainFileDeletes deletes;
			if (branch_delete) {
				deletes = AnalyseMainDelete(context, local_changes, table_id, *branch_delete, main_file,
				                            inlined_positions, head, !file_deletes.replaced_rows.empty());
				if (!deletes.has_main_delete && !deletes.has_inlined && file_deletes.replaced_rows.empty()) {
					// the branch's delete file holds exactly the branch's deletes - main takes it over as is
					continue;
				}
				// the branch's own delete file is replaced below, or dropped with the data file
				merge.files_to_schedule.emplace_back(loaded.delete_files[branch_delete->branch_delete_file],
				                                     branch_delete->branch_delete_file);
			} else {
				LoadMainDeletes(context, main_file, inlined_positions, head, deletes);
			}
			for (auto &row : file_deletes.replaced_rows) {
				deletes.branch_only.insert(row.first);
			}
			if (deletes.DeletedCount() >= main_file.row_count) {
				if (branch_delete) {
					ForgetDeleteFiles(transaction, table_id, data_file_path);
				}
				transaction.DropFile(table_id, main_file.file_id, data_file_path, main_file.row_count,
				                     main_file.file.file_size_bytes);
				continue;
			}
			if (deletes.branch_only.empty()) {
				// every row the branch deleted from the file is deleted on main already
				ForgetDeleteFiles(transaction, table_id, data_file_path);
				continue;
			}
			auto encryption_key = catalog.GenerateEncryptionKey(context);
			DuckLakeDeleteFile written;
			if (deletes.has_main_delete) {
				// main's positions keep their snapshot ids - PositionWithSnapshot compares by position
				auto &positions = deletes.main_positions;
				for (auto &position : deletes.branch_only) {
					PositionWithSnapshot with_snapshot;
					with_snapshot.position = static_cast<int64_t>(position);
					with_snapshot.snapshot_id = static_cast<int64_t>(merge_snapshot);
					positions.insert(with_snapshot);
				}
				WriteDeleteFileWithSnapshotsInput input {
				    context,        transaction,    fs,        table.DataPath(),
				    encryption_key, data_file_path, positions, DeleteFileSource::REGULAR};
				written = DuckLakeDeleteFileWriter::Write(context, input, use_deletion_vectors);
				written.overwrites_existing_delete = true;
				written.overwritten_delete_file.delete_file_id = main_file.delete_file_id;
				written.overwritten_delete_file.path = main_file.delete_file.path;
				idx_t max_snapshot = 0;
				for (auto &position : positions) {
					max_snapshot = MaxValue(max_snapshot, static_cast<idx_t>(position.snapshot_id));
				}
				written.max_snapshot = max_snapshot;
			} else {
				WriteDeleteFileInput input {context,
				                            transaction,
				                            fs,
				                            table.DataPath(),
				                            encryption_key,
				                            data_file_path,
				                            deletes.branch_only,
				                            DeleteFileSource::REGULAR};
				written = DuckLakeDeleteFileWriter::Write(context, input, use_deletion_vectors);
			}
			written.data_file_id = main_file.file_id;
			vector<DuckLakeDeleteFile> written_files;
			written_files.push_back(std::move(written));
			transaction.AddDeletes(table_id, std::move(written_files));
		}
	}
}

void DuckLakeBranchManager::ForgetFilesMainEmptied(DuckLakeTransaction &transaction, DuckLakeBranchMerge &merge) {
	// a main file the branch emptied, which main has emptied since the fork as well, is gone at main's head
	auto &state = *transaction.state;
	for (auto &table : merge.row_merge) {
		set<idx_t> emptied;
		for (auto &dropped : merge.loaded.dropped_files) {
			if (dropped.second == table.first) {
				emptied.insert(dropped.first);
			}
		}
		if (emptied.empty()) {
			continue;
		}
		auto entry = GetTableEntry(transaction, merge.head_snapshot, table.first);
		auto main_files = transaction.GetMetadataManager().GetExtendedFilesForTable(entry->Cast<DuckLakeTableEntry>(),
		                                                                            merge.head_snapshot, nullptr);
		for (auto &file : main_files) {
			if (file.file_id.IsValid()) {
				emptied.erase(file.file_id.index);
			}
		}
		if (emptied.empty()) {
			continue;
		}
		auto references = ResolveMainFiles(transaction, emptied, merge.fork_snapshot, merge.loaded.info);
		auto &stats = state.dropped_file_stats[table.first];
		for (auto &reference : references) {
			state.dropped_files.erase(reference.second.path);
			stats.row_count -= reference.second.row_count;
			stats.file_size_bytes -= reference.second.file_size_bytes;
		}
	}
}

void DuckLakeBranchManager::DropFullyDeletedBranchFiles(DuckLakeTransaction &transaction, DuckLakeBranchMerge &merge) {
	auto &local_changes = transaction.state->local_changes;
	vector<pair<TableIndex, string>> fully_deleted;
	vector<string> written_by_merge;
	for (auto &entry : local_changes.Changes()) {
		for (auto &file : entry.GetTableChanges().new_data_files) {
			if (!file.delete_files.empty() && file.delete_files.back().delete_count >= file.row_count) {
				fully_deleted.emplace_back(entry.GetTableIndex(), file.file_name);
				merge.files_to_schedule.emplace_back(merge.loaded.data_files[file.file_name], file.file_name);
				for (auto &delete_file : file.delete_files) {
					auto branch_file = merge.loaded.delete_files.find(delete_file.file_name);
					if (branch_file != merge.loaded.delete_files.end()) {
						merge.files_to_schedule.emplace_back(branch_file->second, delete_file.file_name);
					} else {
						// written by this merge, for the rows main's copy stays for
						written_by_merge.push_back(delete_file.file_name);
					}
				}
			}
		}
	}
	for (auto &entry : fully_deleted) {
		ForgetFile(transaction, entry.first, entry.second);
	}
	if (!written_by_merge.empty()) {
		auto context_ref = transaction.context.lock();
		auto &fs = FileSystem::GetFileSystem(*context_ref);
		for (auto &path : written_by_merge) {
			fs.TryRemoveFile(path);
		}
	}
}

DuckLakeBranchInfo DuckLakeBranchManager::PrepareMerge(DuckLakeTransaction &transaction, const string &name,
                                                       optional_ptr<const DuckLakeSnapshotCommit> commit_info,
                                                       DuckLakeConflictResolution on_conflict) {
	if (IsOnBranch(transaction)) {
		throw InvalidInputException("Cannot merge while on a branch - run SET BRANCH main first");
	}
	if (IsMergingBranch(transaction)) {
		throw InvalidInputException("Only one branch can be merged per transaction");
	}
	if (transaction.ChangesMade()) {
		throw InvalidInputException("MERGE BRANCH must be the only change in its transaction");
	}
	auto branch = GetActiveBranch(transaction, name);
	if (!branch) {
		throw InvalidInputException("Branch \"%s\" does not exist", name);
	}
	auto fork_snapshot = GetForkSnapshot(transaction, *branch);
	EnsureSnapshotsSinceFork(transaction, *branch);

	auto merge = make_uniq<DuckLakeBranchMerge>();
	merge->fork_snapshot = *fork_snapshot;
	LoadBranch(transaction, *branch, *fork_snapshot, merge->loaded, true);
	merge->head_snapshot = transaction.GetSnapshot();
	auto main_changes = MainChangesSince(transaction, *fork_snapshot);
	// a column change main's own changes to the table rule out is reported before anything is written
	CheckColumnChanges(transaction, branch->name, *fork_snapshot, ColumnChangedMainTables(transaction), main_changes);
	// tables both sides changed rows of merge row by row
	merge->row_merge = FindRowMergeTables(transaction, merge->loaded, *fork_snapshot, main_changes);
	auto context_ref = transaction.context.lock();
	for (auto &table : merge->row_merge) {
		table.second.on_conflict = on_conflict;
		PlanRowMerge(*context_ref, transaction.GetCatalog().GetName().GetIdentifierName(), branch->name,
		             branch->fork_snapshot_id, merge->head_snapshot.snapshot_id, table.second);
	}
	ApplyRowMerge(transaction, *merge);
	RebaseMainDeletes(transaction, *merge);
	DropFullyDeletedBranchFiles(transaction, *merge);
	merge->changes_fingerprint = ChangesFingerprint(transaction);

	auto &info = transaction.GetCommitInfo();
	if (commit_info) {
		info = *commit_info;
	} else if (!info.is_commit_info_set) {
		info.commit_message = Value("MERGE BRANCH " + name);
		info.commit_extra_info =
		    Value(StringUtil::Format(R"({"branch": "%s", "fork_snapshot_id": %d, "branch_commits": %d})",
		                             StringUtil::Replace(StringUtil::Replace(name, "\\", "\\\\"), "\"", "\\\""),
		                             branch->fork_snapshot_id, branch->head_seq));
	}
	SetBranchMerge(transaction, std::move(merge));
	return *branch;
}

void DuckLakeBranchManager::CheckMerge(DuckLakeTransaction &transaction, const DuckLakeBranchMerge &merge,
                                       const SnapshotChangeInformation &other_changes) {
	auto &info = merge.loaded.info;
	auto branch = GetBranch(transaction, info.branch_id);
	if (branch && branch->status == "merged") {
		throw TransactionException("Branch \"%s\" was merged by another transaction", info.name);
	}
	if (!branch || !branch->IsActive() || branch->head_seq != info.head_seq) {
		throw TransactionException("Branch \"%s\" was changed or dropped during the merge - retry", info.name);
	}
	EnsureSnapshotsSinceFork(transaction, info);
	set<TableIndex> deleted_from = transaction.state->tables_deleted_from;
	for (auto &entry : transaction.state->local_changes.Changes()) {
		if (!entry.GetTableChanges().new_delete_files.empty()) {
			deleted_from.insert(entry.GetTableIndex());
		}
	}
	for (auto &table : merge.row_merge) {
		deleted_from.erase(table.first);
	}
	CheckMergeOnlyConflicts(info.name, deleted_from, other_changes);
	CheckMergeDefinitions(transaction, info.name, merge.fork_snapshot, ColumnChangedMainTables(transaction),
	                      other_changes);
	CheckRowMergeTablesUnchanged(transaction, merge);
}

//===--------------------------------------------------------------------===//
// Merge preview
//===--------------------------------------------------------------------===//
namespace {

void AddLabel(vector<string> &labels, const string &label) {
	if (std::find(labels.begin(), labels.end(), label) == labels.end()) {
		labels.push_back(label);
	}
}

void AddChange(vector<string> &result, const set<TableIndex> &tables, TableIndex table_id, const char *label) {
	if (tables.find(table_id) != tables.end()) {
		AddLabel(result, label);
	}
}

vector<string> DescribeChanges(const TransactionChangeInformation &changes, TableIndex table_id) {
	vector<string> result;
	AddChange(result, changes.tables_inserted_into, table_id, "inserted_into");
	AddChange(result, changes.tables_inserted_inlined, table_id, "inserted_inlined");
	AddChange(result, changes.tables_deleted_from, table_id, "deleted_from");
	AddChange(result, changes.tables_deleted_inlined, table_id, "deleted_inlined");
	AddChange(result, changes.tables_flushed_inlined, table_id, "flushed_inlined");
	AddChange(result, changes.altered_tables, table_id, "altered");
	AddChange(result, changes.dropped_tables, table_id, "dropped");
	AddChange(result, changes.tables_compacted, table_id, "compacted");
	AddChange(result, changes.tables_merge_adjacent, table_id, "compacted");
	AddChange(result, changes.tables_rewrite_delete, table_id, "compacted");
	return result;
}

vector<string> DescribeChanges(const SnapshotChangeInformation &changes, TableIndex table_id) {
	vector<string> result;
	AddChange(result, changes.inserted_tables, table_id, "inserted_into");
	AddChange(result, changes.tables_inserted_inlined, table_id, "inserted_inlined");
	AddChange(result, changes.tables_deleted_from, table_id, "deleted_from");
	AddChange(result, changes.tables_deleted_inlined, table_id, "deleted_inlined");
	AddChange(result, changes.tables_flushed_inlined, table_id, "flushed_inlined");
	AddChange(result, changes.altered_tables, table_id, "altered");
	AddChange(result, changes.dropped_tables, table_id, "dropped");
	AddChange(result, changes.tables_compacted, table_id, "compacted");
	AddChange(result, changes.tables_merge_adjacent, table_id, "compacted");
	AddChange(result, changes.tables_rewrite_delete, table_id, "compacted");
	return result;
}

//! The transaction's changes limited to one table, so the conflict rules can be run for that table alone
TransactionChangeInformation RestrictToTable(const TransactionChangeInformation &changes, TableIndex table_id) {
	TransactionChangeInformation result;
	auto keep = [&](const set<TableIndex> &from, set<TableIndex> &to) {
		if (from.find(table_id) != from.end()) {
			to.insert(table_id);
		}
	};
	keep(changes.altered_tables, result.altered_tables);
	keep(changes.altered_tables_with_schema_version_changes, result.altered_tables_with_schema_version_changes);
	keep(changes.altered_views, result.altered_views);
	keep(changes.dropped_tables, result.dropped_tables);
	keep(changes.dropped_views, result.dropped_views);
	keep(changes.tables_inserted_into, result.tables_inserted_into);
	keep(changes.tables_deleted_from, result.tables_deleted_from);
	keep(changes.tables_delete_attempted, result.tables_delete_attempted);
	keep(changes.tables_inserted_inlined, result.tables_inserted_inlined);
	keep(changes.tables_deleted_inlined, result.tables_deleted_inlined);
	keep(changes.tables_flushed_inlined, result.tables_flushed_inlined);
	keep(changes.tables_compacted, result.tables_compacted);
	keep(changes.tables_merge_adjacent, result.tables_merge_adjacent);
	keep(changes.tables_rewrite_delete, result.tables_rewrite_delete);
	for (auto &schema_entry : changes.created_tables) {
		for (auto &created : schema_entry.second) {
			auto &object = created.get();
			auto id = object.type == CatalogType::TABLE_ENTRY ? object.Cast<DuckLakeTableEntry>().GetTableId()
			                                                  : object.Cast<DuckLakeViewEntry>().GetViewId();
			if (id == table_id) {
				result.created_tables[schema_entry.first].insert(object);
			}
		}
	}
	return result;
}

//! The transaction's changes limited to one schema
TransactionChangeInformation RestrictToSchema(const TransactionChangeInformation &changes, SchemaIndex schema_id) {
	TransactionChangeInformation result;
	for (auto &entry : changes.created_schemas) {
		if (entry.second.get().GetSchemaId() == schema_id) {
			result.created_schemas.insert(entry);
		}
	}
	for (auto &entry : changes.dropped_schemas) {
		if (entry.first == schema_id) {
			result.dropped_schemas.insert(entry);
		}
	}
	return result;
}

void KeepOne(const set<TableIndex> &all, TableIndex id, set<TableIndex> &target) {
	target.clear();
	if (all.find(id) != all.end()) {
		target.insert(id);
	}
}

//! Takes the loaded branch out of the transaction again when the preview returns or fails
struct LoadedBranchDiscard {
	explicit LoadedBranchDiscard(DuckLakeTransaction &transaction) : transaction(transaction) {
	}
	~LoadedBranchDiscard() {
		DuckLakeBranchManager::DiscardLoadedBranch(transaction);
	}

	DuckLakeTransaction &transaction;
};

} // namespace

void DuckLakeBranchManager::DiscardLoadedBranch(DuckLakeTransaction &transaction) {
	// everything LoadBranch touches; the transaction had no changes before, so clearing restores it
	auto &state = *transaction.state;
	state.local_changes.Clear();
	state.dropped_files.clear();
	state.dropped_file_stats.clear();
	state.tables_deleted_from.clear();
	state.tables_delete_attempted.clear();
	ClearDefinitions(transaction);
}

vector<DuckLakeMergePreviewEntry> DuckLakeBranchManager::PreviewMerge(DuckLakeTransaction &transaction,
                                                                      const string &name,
                                                                      DuckLakeConflictResolution on_conflict) {
	// a dry run loads the branch into its transaction, which the scans of one query share and may run in parallel
	// (a UNION ALL of dry runs under an ORDER BY): dry runs take turns
	static mutex preview_lock;
	lock_guard<mutex> preview_guard(preview_lock);
	if (IsOnBranch(transaction)) {
		throw InvalidInputException("Cannot preview a merge while on a branch - run SET BRANCH main first");
	}
	if (IsMergingBranch(transaction) || transaction.ChangesMade()) {
		throw InvalidInputException("ducklake_merge_branch(dry_run => true) needs a transaction without other changes");
	}
	auto branch = GetActiveBranch(transaction, name);
	if (!branch) {
		throw InvalidInputException("Branch \"%s\" does not exist", name);
	}
	auto fork_snapshot = GetForkSnapshot(transaction, *branch);
	EnsureSnapshotsSinceFork(transaction, *branch);

	auto context_ref = transaction.context.lock();
	auto &context = *context_ref;
	auto &catalog = transaction.GetCatalog();
	auto &metadata_manager = transaction.GetMetadataManager();
	auto &state = *transaction.state;
	auto &local_changes = state.local_changes;

	// the branch is loaded the way a merge loads it, and removed from the transaction again before returning
	LoadedBranchDiscard discard(transaction);
	DuckLakeLoadedBranch loaded;
	LoadBranch(transaction, *branch, *fork_snapshot, loaded, false);

	// tables and views by id - they share one id space - and schemas
	map<TableIndex, DuckLakeMergePreviewEntry> entries;
	map<SchemaIndex, DuckLakeMergePreviewEntry> schema_entries;
	auto add_entry = [&](TableIndex id, CatalogEntry &object) -> DuckLakeMergePreviewEntry & {
		auto existing = entries.find(id);
		if (existing != entries.end()) {
			return existing->second;
		}
		auto &entry = entries[id];
		entry.table_id = id;
		entry.object_type = object.type == CatalogType::VIEW_ENTRY ? "view" : "table";
		entry.schema_name = object.ParentSchema().name.GetIdentifierName();
		entry.table_name = object.name.GetIdentifierName();
		return entry;
	};
	auto get_entry = [&](TableIndex table_id) -> DuckLakeMergePreviewEntry & {
		auto existing = entries.find(table_id);
		if (existing != entries.end()) {
			return existing->second;
		}
		return add_entry(table_id, GetTableAtFork(transaction, *fork_snapshot, table_id, name));
	};
	auto main_view = [&](TableIndex view_id) -> CatalogEntry & {
		auto view = GetMainEntry(transaction, *fork_snapshot, view_id, CatalogType::VIEW_ENTRY);
		if (!view) {
			throw InternalException("A view the branch changed does not exist at its fork");
		}
		return *view;
	};

	// rows the branch adds: its data files, minus the rows it deleted from them again
	for (auto &change : local_changes.Changes()) {
		for (auto &file : change.GetTableChanges().new_data_files) {
			idx_t deleted = file.delete_files.empty() ? 0 : file.delete_files.back().delete_count;
			if (deleted >= file.row_count) {
				// left out of main, as the merge does
				continue;
			}
			auto &entry = get_entry(change.GetTableIndex());
			entry.rows_inserted += file.row_count - deleted;
			entry.files_added++;
		}
	}

	// rows the branch removes from main: deletes on main files, main files it emptied, and inlined rows
	map<TableIndex, vector<reference<const DuckLakeLoadedBranch::MainDelete>>> deletes_per_table;
	for (auto &main_delete : loaded.main_deletes) {
		deletes_per_table[main_delete.table_id].push_back(main_delete);
	}
	map<TableIndex, vector<idx_t>> dropped_per_table;
	for (auto &dropped : loaded.dropped_files) {
		dropped_per_table[dropped.second].push_back(dropped.first);
	}
	set<TableIndex> deleting_tables;
	for (auto &entry : deletes_per_table) {
		deleting_tables.insert(entry.first);
	}
	for (auto &entry : dropped_per_table) {
		deleting_tables.insert(entry.first);
	}
	for (auto &table_id : deleting_tables) {
		auto &entry = get_entry(table_id);
		auto &table = GetTableAtFork(transaction, *fork_snapshot, table_id, name);
		auto main_files = metadata_manager.GetExtendedFilesForTable(table, *fork_snapshot, nullptr);
		unordered_map<idx_t, reference<DuckLakeFileListExtendedEntry>> files_by_id;
		for (auto &file : main_files) {
			if (file.file_id.IsValid()) {
				files_by_id.emplace(file.file_id.index, file);
			}
		}
		auto inlined_deletions = metadata_manager.ReadInlinedFileDeletions(table_id, *fork_snapshot);
		auto find_main_file = [&](idx_t file_id) -> DuckLakeFileListExtendedEntry & {
			auto file_entry = files_by_id.find(file_id);
			if (file_entry == files_by_id.end()) {
				throw InvalidInputException("Branch \"%s\" deleted from data file %d, which is not visible at its fork",
				                            name, file_id);
			}
			return file_entry->second.get();
		};
		for (auto &main_delete_ref : deletes_per_table[table_id]) {
			auto &main_delete = main_delete_ref.get();
			auto &main_file = find_main_file(main_delete.data_file_id);
			auto deletes = AnalyseMainDelete(context, local_changes, table_id, main_delete, main_file,
			                                 FindInlinedPositions(inlined_deletions, main_delete.data_file_id),
			                                 *fork_snapshot, true);
			entry.rows_deleted += deletes.branch_only.size();
		}
		for (auto &file_id : dropped_per_table[table_id]) {
			// the branch deleted every row main had not deleted already
			auto &main_file = find_main_file(file_id);
			MainFileDeletes deletes;
			LoadMainDeletes(context, main_file, FindInlinedPositions(inlined_deletions, file_id), *fork_snapshot,
			                deletes);
			auto already_deleted = deletes.main_positions.size() + deletes.inlined_count;
			entry.rows_deleted += main_file.row_count > already_deleted ? main_file.row_count - already_deleted : 0;
		}
	}
	for (auto &table_entry : loaded.inlined_deletes) {
		auto &entry = get_entry(table_entry.first);
		for (auto &inlined : table_entry.second) {
			entry.rows_deleted += inlined.second.size();
		}
	}

	// what the branch did to the catalog
	for (auto &schema_set : state.new_tables) {
		for (auto &catalog_entry : schema_set.second->GetEntries()) {
			auto &object = *catalog_entry.second;
			auto is_table = object.type == CatalogType::TABLE_ENTRY;
			auto id = is_table ? object.Cast<DuckLakeTableEntry>().GetTableId()
			                   : object.Cast<DuckLakeViewEntry>().GetViewId();
			if (IsTransactionLocal(id)) {
				AddLabel(add_entry(id, object).branch_changes, "created");
				continue;
			}
			// a table or view of main the branch renamed, or changed the columns of (labelled with its changes below)
			auto &entry = is_table ? get_entry(id) : add_entry(id, main_view(id));
			if (object.name.GetIdentifierName() != entry.table_name) {
				entry.new_name = object.name.GetIdentifierName();
				AddLabel(entry.branch_changes, "renamed");
			}
		}
	}
	for (auto &table_id : state.dropped_tables) {
		AddLabel(get_entry(table_id).branch_changes, "dropped");
	}
	for (auto &view_id : state.dropped_views) {
		AddLabel(add_entry(view_id, main_view(view_id)).branch_changes, "dropped");
	}
	auto add_schema_entry = [&](DuckLakeSchemaEntry &schema, const char *label) {
		auto &entry = schema_entries[schema.GetSchemaId()];
		entry.object_type = "schema";
		entry.schema_name = schema.name.GetIdentifierName();
		entry.branch_changes.emplace_back(label);
	};
	if (state.new_schemas) {
		for (auto &schema_entry : state.new_schemas->GetEntries()) {
			add_schema_entry(schema_entry.second->Cast<DuckLakeSchemaEntry>(), "created");
		}
	}
	for (auto &schema_entry : state.dropped_schemas) {
		add_schema_entry(schema_entry.second.get(), "dropped");
	}

	// the checks the merge commit runs, table by table so that every conflict is reported
	auto changes = transaction.GetTransactionChanges();
	auto executor = [&](string query) -> unique_ptr<QueryResult> {
		auto result = metadata_manager.Query(*fork_snapshot, query);
		if (result->HasError()) {
			result->GetErrorObject().Throw("Failed to preview the merge of a DuckLake branch: ");
		}
		return result;
	};
	auto other_changes = MainChangesSince(transaction, *fork_snapshot);
	// the tables both sides changed rows of merge row by row, as the merge decides them
	auto row_merge = FindRowMergeTables(transaction, loaded, *fork_snapshot, other_changes);
	auto head_snapshot = transaction.GetSnapshot();
	for (auto &table : row_merge) {
		table.second.on_conflict = on_conflict;
		PlanRowMerge(context, catalog.GetName().GetIdentifierName(), name, branch->fork_snapshot_id,
		             head_snapshot.snapshot_id, table.second);
	}
	set<TableIndex> main_tables, main_views;
	for (auto &entry : entries) {
		if (!IsTransactionLocal(entry.first)) {
			(entry.second.object_type == "view" ? main_views : main_tables).insert(entry.first);
		}
	}
	auto renamed_on_main = RenamedOnMain(transaction, false, main_tables, fork_snapshot->snapshot_id);
	auto views_renamed_on_main = RenamedOnMain(transaction, true, main_views, fork_snapshot->snapshot_id);
	// the rules compare the transaction's files and catalog changes with main's, so they see one object at a time
	auto all_local_changes = std::move(local_changes.changes);
	auto all_dropped_files = std::move(state.dropped_files);
	auto all_dropped_tables = std::move(state.dropped_tables);
	auto all_dropped_views = std::move(state.dropped_views);
	auto all_renamed_tables = std::move(state.renamed_tables);
	auto all_renamed_views = std::move(state.renamed_views);
	auto check = [&](DuckLakeMergePreviewEntry &entry, const TransactionChangeInformation &object_changes) {
		try {
			// the branch's own rules first: they name the object, DuckLake's give its index
			CheckMergeDefinitions(transaction, name, *fork_snapshot, object_changes.altered_tables, other_changes);
			state.CheckForConflicts(object_changes, other_changes, *fork_snapshot, executor);
			CheckMergeOnlyConflicts(name, object_changes.tables_deleted_from, other_changes);
		} catch (std::exception &ex) {
			ErrorData error(ex);
			if (error.Type() != ExceptionType::TRANSACTION) {
				throw;
			}
			entry.conflict = error.RawMessage();
		}
	};
	vector<DuckLakeMergePreviewEntry> result;
	for (auto &entry_pair : entries) {
		auto table_id = entry_pair.first;
		auto &entry = entry_pair.second;
		auto is_view = entry.object_type == "view";
		local_changes.changes.clear();
		auto table_local_changes = all_local_changes.find(table_id);
		if (table_local_changes != all_local_changes.end()) {
			local_changes.changes.emplace(table_id, std::move(table_local_changes->second));
		}
		state.dropped_files.clear();
		for (auto &dropped : all_dropped_files) {
			auto dropped_table = loaded.dropped_files.find(dropped.second.index);
			if (dropped_table != loaded.dropped_files.end() && dropped_table->second == table_id) {
				state.dropped_files.insert(dropped);
			}
		}
		KeepOne(all_dropped_tables, table_id, state.dropped_tables);
		KeepOne(all_dropped_views, table_id, state.dropped_views);
		KeepOne(all_renamed_tables, table_id, state.renamed_tables);
		KeepOne(all_renamed_views, table_id, state.renamed_views);
		auto object_changes = RestrictToTable(changes, table_id);
		for (auto &label : DescribeChanges(object_changes, table_id)) {
			AddLabel(entry.branch_changes, label);
		}
		if (IsTransactionLocal(table_id) && entry.files_added > 0) {
			// DuckLake reports the inserts into a table it creates with the table
			AddLabel(entry.branch_changes, "inserted_into");
		}
		if (is_view) {
			AddChange(entry.main_changes, other_changes.altered_views, table_id, "altered");
			AddChange(entry.main_changes, other_changes.dropped_views, table_id, "dropped");
			AddChange(entry.main_changes, views_renamed_on_main, table_id, "renamed");
		} else {
			entry.main_changes = DescribeChanges(other_changes, table_id);
			AddChange(entry.main_changes, renamed_on_main, table_id, "renamed");
		}
		auto row_merged = row_merge.find(table_id);
		if (row_merged != row_merge.end()) {
			ExcludeFromInsertDeleteRules(table_id, object_changes);
		}
		check(entry, object_changes);
		if (row_merged != row_merge.end() && entry.conflict.empty() && !row_merged->second.conflicts.empty()) {
			entry.conflict =
			    RowConflictMessage(transaction.GetCatalog().GetName().GetIdentifierName(), name, row_merged->second);
		}
		if (table_local_changes != all_local_changes.end()) {
			table_local_changes->second = std::move(local_changes.changes[table_id]);
		}
		result.push_back(std::move(entry));
	}
	local_changes.changes.clear();
	state.dropped_files.clear();
	state.dropped_tables.clear();
	state.dropped_views.clear();
	state.renamed_tables.clear();
	state.renamed_views.clear();
	for (auto &entry_pair : schema_entries) {
		auto &entry = entry_pair.second;
		if (other_changes.dropped_schemas.find(entry_pair.first) != other_changes.dropped_schemas.end()) {
			entry.main_changes.emplace_back("dropped");
		}
		check(entry, RestrictToSchema(changes, entry_pair.first));
		result.push_back(std::move(entry));
	}
	local_changes.changes = std::move(all_local_changes);
	state.dropped_files = std::move(all_dropped_files);
	state.dropped_tables = std::move(all_dropped_tables);
	state.dropped_views = std::move(all_dropped_views);
	state.renamed_tables = std::move(all_renamed_tables);
	state.renamed_views = std::move(all_renamed_views);
	std::sort(result.begin(), result.end(), [](const DuckLakeMergePreviewEntry &a, const DuckLakeMergePreviewEntry &b) {
		return std::tie(a.schema_name, a.table_name) < std::tie(b.schema_name, b.table_name);
	});
	return result;
}

string DuckLakeBranchManager::MergeBookkeepingSql(DuckLakeTransaction &transaction, const DuckLakeBranchMerge &merge,
                                                  bool with_snapshot) {
	auto &info = merge.loaded.info;
	auto &commit_info = transaction.GetCommitInfo();
	auto &metadata_manager = transaction.GetMetadataManager();
	auto id = info.branch_id;
	auto merged_seq = info.head_seq + 1;
	string sql;
	// the update only applies while the branch is where it was loaded, and takes the row lock on backends that have
	// one; the insert after it duplicates the branch id and fails the batch when the update did not apply
	sql +=
	    StringUtil::Format("UPDATE {METADATA_CATALOG}.ducklake_branching_branch SET status = 'merged', head_seq = %d "
	                       "WHERE branch_id = %d AND head_seq = %d AND status = 'active';\n",
	                       merged_seq, id, info.head_seq);
	sql += StringUtil::Format(
	    "INSERT INTO {METADATA_CATALOG}.ducklake_branching_branch SELECT %d, %s, %d, %d, %d, 'merge guard', NOW() "
	    "WHERE "
	    "NOT EXISTS (SELECT 1 FROM {METADATA_CATALOG}.ducklake_branching_branch WHERE branch_id = %d AND head_seq = %d "
	    "AND status = 'merged');\n",
	    id, DuckLakeUtil::SQLLiteralToString(info.name), info.fork_snapshot_id, info.head_seq, info.next_file_seq, id,
	    merged_seq);
	string changes_made = with_snapshot ? "merged:{SNAPSHOT_ID}" : "merged";
	sql +=
	    StringUtil::Format("INSERT INTO {METADATA_CATALOG}.ducklake_branching_commit VALUES (%d, %d, NOW(), %s, %s, "
	                       "%s, %s);\n",
	                       id, merged_seq, commit_info.author.ToSQLString(), commit_info.commit_message.ToSQLString(),
	                       commit_info.commit_extra_info.ToSQLString(), DuckLakeUtil::SQLLiteralToString(changes_made));
	string scheduled;
	for (auto &file : merge.files_to_schedule) {
		auto path = metadata_manager.GetRelativePath(file.second);
		AppendValues(scheduled,
		             StringUtil::Format("(%d, %s, %s, NOW())", file.first, DuckLakeUtil::SQLLiteralToString(path.path),
		                                path.path_is_relative ? "true" : "false"));
	}
	if (!scheduled.empty()) {
		sql += "INSERT INTO {METADATA_CATALOG}.ducklake_files_scheduled_for_deletion VALUES " + scheduled + ";\n";
	}
	// main owns the branch's data files now - only the bookkeeping rows go
	vector<string> branch_tables {"ducklake_branching_data_file",
	                              "ducklake_branching_delete_file",
	                              "ducklake_branching_file_column_stats",
	                              "ducklake_branching_file_partition_value",
	                              "ducklake_branching_inlined_delete",
	                              "ducklake_branching_dropped_file",
	                              "ducklake_branching_name"};
	if (HasDefinitionTables(transaction)) {
		branch_tables.insert(branch_tables.end(), {"ducklake_branching_object", "ducklake_branching_column",
		                                           "ducklake_branching_main_change"});
	}
	if (merge.loaded.has_column_change_table) {
		branch_tables.push_back("ducklake_branching_column_change");
	}
	for (auto &table : branch_tables) {
		sql += StringUtil::Format("DELETE FROM {METADATA_CATALOG}.%s WHERE branch_id = %d;\n", table, id);
	}
	return sql;
}

//===--------------------------------------------------------------------===//
// Main-side protection
//===--------------------------------------------------------------------===//
string DuckLakeBranchManager::ActiveForkSnapshotsQuery() {
	return "SELECT fork_snapshot_id FROM {METADATA_CATALOG}.ducklake_branching_branch WHERE status = 'active'";
}

string DuckLakeBranchManager::ExpirableSnapshotFilter() {
	return "NOT EXISTS (SELECT 1 FROM {METADATA_CATALOG}.ducklake_branching_branch b WHERE b.status = 'active' AND "
	       "b.fork_snapshot_id <= snapshot_id)";
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
