//===----------------------------------------------------------------------===//
//                         DuckDB
//
// branching/ducklake_branch_merge_rows.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/set.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/main/connection.hpp"

namespace duckdb {
class ClientContext;
class DuckLakeCatalog;

//! Runs a query on an internal connection and throws its error
unique_ptr<QueryResult> RunBranchSql(Connection &connection, const string &query);
//! A connection reading the branch: branch selection is per connection, so the branch gets its own
unique_ptr<Connection> BranchSqlConnection(ClientContext &context, const string &catalog_name,
                                           const string &branch_name);
//! "catalog"."schema"."table"
string BranchTableSql(const string &catalog_name, const string &schema_name, const string &table_name);

//! The rows of a query, one at a time
struct DuckLakeBranchRowCursor {
	unique_ptr<QueryResult> result;
	unique_ptr<DataChunk> chunk;
	idx_t row = 0;

	void Start(unique_ptr<QueryResult> result_p);
	bool Valid() const;
	void Next();
	//! The first column, a row id
	int64_t RowId() const;
	Value Get(idx_t column) const;
};

//! What the row-level dry run of a merge reads: one table on the branch, and the same table on main at the fork
struct DuckLakeMergeRowsBindData {
	string catalog_name;
	string branch_name;
	string schema_name;
	string table_name;
	idx_t fork_snapshot_id = 0;
	//! Main's head when the dry run started, which the row-by-row merge rules compare against
	idx_t head_snapshot_id = 0;
	//! The table conflicts as a whole (main dropped, altered or renamed it), or for the branch's deletes (main
	//! compacted it); the row-by-row rules do not apply then
	bool table_conflicts = false;
	bool deletes_conflict = false;
	//! The rows both sides changed: differently, or main's copy stays
	set<int64_t> conflict_rows;
	set<int64_t> same_as_main_rows;
	vector<string> column_names;
	vector<LogicalType> column_types;
};

//! The rows a merge would insert, delete and update in one table, in the terms of ducklake_table_changes: the table
//! as the branch has it against the table at the fork, matched by row id
class DuckLakeMergeRowsScan {
public:
	DuckLakeMergeRowsScan(ClientContext &context, const DuckLakeMergeRowsBindData &data);

	static DuckLakeMergeRowsBindData Bind(ClientContext &context, DuckLakeCatalog &catalog, const string &branch_name,
	                                      const string &schema_name, const string &table_name);
	//! Fills the output with the next changed rows; an empty output means the scan is done
	void Scan(DataChunk &output);

private:
	using Cursor = DuckLakeBranchRowCursor;

	void Emit(DataChunk &output, idx_t &count, const char *change_type, const Cursor &cursor);
	bool SameValues() const;
	Value MergeStatus(const char *change_type, int64_t row_id) const;

	const DuckLakeMergeRowsBindData &data;
	idx_t column_count;
	//! The connections run the two reads; they outlive the results
	unique_ptr<Connection> branch_connection;
	unique_ptr<Connection> main_connection;
	Cursor branch_rows;
	Cursor fork_rows;
};

} // namespace duckdb
