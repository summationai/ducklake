#include "branching/ducklake_branch_merge_rows.hpp"

#include "branching/ducklake_branch.hpp"
#include "common/ducklake_util.hpp"
#include "common/index.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/query_result.hpp"
#include "duckdb/planner/tableref/bound_at_clause.hpp"
#include "storage/ducklake_metadata_manager.hpp"
#include "storage/ducklake_transaction_changes.hpp"
#include "storage/ducklake_catalog.hpp"
#include "storage/ducklake_table_entry.hpp"
#include "storage/ducklake_transaction.hpp"

namespace duckdb {

unique_ptr<QueryResult> RunBranchSql(Connection &connection, const string &query) {
	auto result = connection.Query(query);
	if (result->HasError()) {
		result->GetErrorObject().Throw();
	}
	return result;
}

unique_ptr<Connection> BranchSqlConnection(ClientContext &context, const string &catalog_name,
                                           const string &branch_name) {
	auto connection = make_uniq<Connection>(*context.db);
	RunBranchSql(*connection,
	             StringUtil::Format("CALL ducklake_set_branch(%s, %s)", DuckLakeUtil::SQLLiteralToString(catalog_name),
	                                DuckLakeUtil::SQLLiteralToString(branch_name)));
	return connection;
}

string BranchTableSql(const string &catalog_name, const string &schema_name, const string &table_name) {
	return DuckLakeUtil::SQLIdentifierToString(catalog_name) + "." + DuckLakeUtil::SQLIdentifierToString(schema_name) +
	       "." + DuckLakeUtil::SQLIdentifierToString(table_name);
}

DuckLakeMergeRowsBindData DuckLakeMergeRowsScan::Bind(ClientContext &context, DuckLakeCatalog &catalog,
                                                      const string &branch_name, const string &schema_name,
                                                      const string &table_name, DuckLakeConflictResolution on_conflict,
                                                      bool conflicts_only) {
	auto &transaction = DuckLakeTransaction::Get(context, catalog);
	if (DuckLakeBranchManager::IsOnBranch(transaction)) {
		throw InvalidInputException("Cannot preview a merge while on a branch - run SET BRANCH main first");
	}
	if (!DuckLakeBranchManager::GetActiveBranch(transaction, branch_name)) {
		throw InvalidInputException("Branch \"%s\" does not exist", branch_name);
	}
	DuckLakeMergeRowsBindData result;
	result.catalog_name = catalog.GetName().GetIdentifierName();
	result.branch_name = branch_name;
	result.schema_name = schema_name;
	result.table_name = table_name;
	result.on_conflict = on_conflict;
	result.conflicts_only = conflicts_only;
	// the table as the branch has it: its columns, and whether it existed at the fork under which name
	auto connection = BranchSqlConnection(context, result.catalog_name, branch_name);
	auto &branch_context = *connection->context;
	TableIndex main_table_id;
	branch_context.RunFunctionInTransaction([&]() {
		QualifiedName name(Identifier(result.catalog_name), Identifier(schema_name), Identifier(table_name));
		EntryLookupInfo lookup(CatalogType::TABLE_ENTRY, name);
		auto entry = Catalog::GetEntry(branch_context, lookup, OnEntryNotFound::RETURN_NULL);
		if (!entry) {
			throw InvalidInputException("Table \"%s.%s\" does not exist on branch \"%s\"", schema_name, table_name,
			                            branch_name);
		}
		if (entry->type != CatalogType::TABLE_ENTRY) {
			throw InvalidInputException("\"%s\" is a %s, not a table - the dry run shows the rows of tables",
			                            table_name, StringUtil::Lower(CatalogTypeToString(entry->type)));
		}
		auto &table = entry->Cast<DuckLakeTableEntry>();
		for (auto &column : table.GetColumns().Logical()) {
			result.column_names.push_back(column.Name().GetIdentifierName());
			result.column_types.push_back(column.Type());
		}
		auto &branch_transaction = DuckLakeTransaction::Get(branch_context, catalog);
		auto fork_snapshot = branch_transaction.GetSnapshot();
		result.fork_snapshot_id = fork_snapshot.snapshot_id;
		if (IsTransactionLocal(table.GetTableId())) {
			// created on the branch: every row is an insert
			return;
		}
		auto fork_entry = DuckLakeBranchManager::GetMainEntry(branch_transaction, fork_snapshot, table.GetTableId(),
		                                                      CatalogType::TABLE_ENTRY);
		if (!fork_entry) {
			throw InternalException("A table of main the branch reads does not exist at its fork");
		}
		result.fork_table_name = fork_entry->name.GetIdentifierName();
		main_table_id = table.GetTableId();
		DuckLakeBranchManager::ProjectColumns(table, fork_entry->Cast<DuckLakeTableEntry>(), result.branch_columns,
		                                      result.fork_columns);
	});
	// how the merge treats rows main changed too since the fork
	auto head = transaction.GetSnapshot();
	result.head_snapshot_id = head.snapshot_id;
	if (result.fork_table_name.empty() || head.snapshot_id <= result.fork_snapshot_id) {
		return result;
	}
	BoundAtClause fork_clause(Identifier("version"), Value::UBIGINT(result.fork_snapshot_id));
	auto fork_snapshot = transaction.GetMetadataManager().GetSnapshot(fork_clause, SnapshotBound::UPPER_BOUND);
	auto main_changes = DuckLakeBranchManager::MainChangesSince(transaction, *fork_snapshot);
	set<TableIndex> table_ids {main_table_id};
	if (DuckLakeBranchManager::KeepsTableLevelRules(main_table_id, false, false, main_changes) ||
	    !DuckLakeBranchManager::RenamedOnMain(transaction, false, table_ids, result.fork_snapshot_id).empty()) {
		result.table_conflicts = true;
	} else if (DuckLakeBranchManager::KeepsTableLevelRules(main_table_id, true, true, main_changes)) {
		result.deletes_conflict = true;
	}
	if (result.table_conflicts || result.deletes_conflict) {
		if (conflicts_only) {
			throw InvalidInputException("Table \"%s\" conflicts as a whole: main altered, dropped, renamed or "
			                            "compacted it since the fork, so its rows are not merged one by one - "
			                            "ducklake_merge_branch with dry_run => true shows the conflict",
			                            table_name);
		}
		return result;
	}
	DuckLakeRowMergeTable table;
	table.table_id = main_table_id;
	table.schema_name = schema_name;
	table.main_name = result.fork_table_name;
	table.branch_name = table_name;
	table.on_conflict = on_conflict;
	table.keep_conflict_values = conflicts_only;
	table.branch_columns = result.branch_columns;
	table.main_columns = result.fork_columns;
	DuckLakeBranchManager::PlanRowMerge(context, result.catalog_name, branch_name, result.fork_snapshot_id,
	                                    head.snapshot_id, table);
	result.conflict_rows.insert(table.conflicts.begin(), table.conflicts.end());
	result.same_as_main_rows = table.same_as_main;
	result.resolved_rows = table.resolved;
	if (conflicts_only) {
		for (auto &conflict : table.conflict_rows) {
			result.conflict_details.emplace_back(conflict.first, std::move(conflict.second));
		}
	}
	return result;
}

LogicalType DuckLakeMergeRowsBindData::RowType() const {
	child_list_t<LogicalType> children;
	for (idx_t column = 0; column < column_names.size(); column++) {
		children.emplace_back(column_names[column], column_types[column]);
	}
	return LogicalType::STRUCT(std::move(children));
}

DuckLakeMergeRowsScan::DuckLakeMergeRowsScan(ClientContext &context, const DuckLakeMergeRowsBindData &data)
    : data(data), column_count(data.column_names.size()) {
	if (data.conflicts_only) {
		// the bind read everything it reports
		return;
	}
	branch_connection = BranchSqlConnection(context, data.catalog_name, data.branch_name);
	auto select_list = [](const vector<string> &columns) {
		return columns.empty() ? string("*") : StringUtil::Join(columns, ", ");
	};
	branch_rows.Start(
	    RunBranchSql(*branch_connection, "SELECT rowid, " + select_list(data.branch_columns) + " FROM " +
	                                         BranchTableSql(data.catalog_name, data.schema_name, data.table_name) +
	                                         " ORDER BY rowid"));
	if (data.fork_table_name.empty()) {
		return;
	}
	main_connection = make_uniq<Connection>(*context.db);
	fork_rows.Start(RunBranchSql(
	    *main_connection,
	    StringUtil::Format("SELECT rowid, %s FROM %s AT (VERSION => %d) ORDER BY rowid", select_list(data.fork_columns),
	                       BranchTableSql(data.catalog_name, data.schema_name, data.fork_table_name),
	                       data.fork_snapshot_id)));
	if (fork_rows.result->ColumnCount() != branch_rows.result->ColumnCount()) {
		// both reads are projected onto the branch's columns
		throw InternalException("A table of main has other columns on the branch than at its fork");
	}
}

void DuckLakeBranchRowCursor::Start(unique_ptr<QueryResult> result_p) {
	result = std::move(result_p);
	// a materialized result ends with no chunk, never with an empty one
	chunk = result->Fetch();
	row = 0;
}

bool DuckLakeBranchRowCursor::Valid() const {
	return chunk != nullptr;
}

void DuckLakeBranchRowCursor::Next() {
	row++;
	if (row < chunk->size()) {
		return;
	}
	chunk = result->Fetch();
	row = 0;
}

int64_t DuckLakeBranchRowCursor::RowId() const {
	return chunk->GetValue(0, row).GetValue<int64_t>();
}

Value DuckLakeBranchRowCursor::Get(idx_t column) const {
	return chunk->GetValue(column, row);
}

bool DuckLakeMergeRowsScan::SameValues() const {
	for (idx_t column = 1; column <= column_count; column++) {
		if (!Value::NotDistinctFrom(branch_rows.Get(column), fork_rows.Get(column))) {
			return false;
		}
	}
	return true;
}

Value DuckLakeMergeRowsScan::MergeStatus(const char *change_type, int64_t row_id) const {
	auto insert = StringUtil::Equals(change_type, "insert");
	if (data.table_conflicts || (data.deletes_conflict && !insert)) {
		return Value("table conflict");
	}
	if (data.conflict_rows.find(row_id) != data.conflict_rows.end()) {
		return Value("conflict");
	}
	if (data.resolved_rows.find(row_id) != data.resolved_rows.end()) {
		return Value(data.on_conflict == DuckLakeConflictResolution::KEEP_MAIN ? "main wins" : "branch wins");
	}
	if (data.same_as_main_rows.find(row_id) != data.same_as_main_rows.end()) {
		return Value("same as main");
	}
	return Value("ok");
}

Value DuckLakeMergeRowsScan::RowValue(const vector<Value> &values) const {
	vector<Value> fields;
	for (idx_t column = 0; column < column_count; column++) {
		fields.push_back(values[column].DefaultCastAs(data.column_types[column]));
	}
	return Value::STRUCT(data.RowType(), std::move(fields));
}

void DuckLakeMergeRowsScan::ScanConflicts(DataChunk &output) {
	idx_t count = 0;
	auto resolution = data.on_conflict == DuckLakeConflictResolution::KEEP_MAIN     ? "main wins"
	                  : data.on_conflict == DuckLakeConflictResolution::KEEP_BRANCH ? "branch wins"
	                                                                                : "conflict";
	auto row_type = data.RowType();
	for (; conflict_offset < data.conflict_details.size() && count < STANDARD_VECTOR_SIZE; conflict_offset++) {
		auto &entry = data.conflict_details[conflict_offset];
		auto &conflict = entry.second;
		output.SetValue(0, count, Value::BIGINT(entry.first));
		output.SetValue(1, count, Value(conflict.on_branch ? "update" : "delete"));
		output.SetValue(2, count, Value(conflict.on_main ? "update" : "delete"));
		output.SetValue(3, count, Value(resolution));
		output.SetValue(4, count, RowValue(conflict.fork));
		output.SetValue(5, count, conflict.on_branch ? RowValue(conflict.branch) : Value(row_type));
		output.SetValue(6, count, conflict.on_main ? RowValue(conflict.main) : Value(row_type));
		count++;
	}
	output.SetCardinality(count);
}

void DuckLakeMergeRowsScan::Emit(DataChunk &output, idx_t &count, const char *change_type, const Cursor &cursor) {
	output.SetValue(0, count, Value(change_type));
	auto row_id = cursor.RowId();
	// a row the branch inserted gets its row id from the merge
	output.SetValue(1, count,
	                DuckLakeConstants::IsTransactionLocalRowId(row_id) ? Value(LogicalType::BIGINT)
	                                                                   : Value::BIGINT(row_id));
	output.SetValue(2, count, MergeStatus(change_type, row_id));
	for (idx_t column = 1; column <= column_count; column++) {
		output.SetValue(column + 2, count, cursor.Get(column));
	}
	count++;
}

void DuckLakeMergeRowsScan::Scan(DataChunk &output) {
	if (data.conflicts_only) {
		ScanConflicts(output);
		return;
	}
	idx_t count = 0;
	// an update takes two rows of output
	while (count + 2 <= STANDARD_VECTOR_SIZE && (branch_rows.Valid() || fork_rows.Valid())) {
		if (!fork_rows.Valid() || (branch_rows.Valid() && branch_rows.RowId() < fork_rows.RowId())) {
			Emit(output, count, "insert", branch_rows);
			branch_rows.Next();
			continue;
		}
		if (!branch_rows.Valid() || fork_rows.RowId() < branch_rows.RowId()) {
			Emit(output, count, "delete", fork_rows);
			fork_rows.Next();
			continue;
		}
		if (!SameValues()) {
			Emit(output, count, "update_preimage", fork_rows);
			Emit(output, count, "update_postimage", branch_rows);
		}
		branch_rows.Next();
		fork_rows.Next();
	}
	output.SetCardinality(count);
}

} // namespace duckdb
