//===----------------------------------------------------------------------===//
//                         DuckDB
//
// branching/ducklake_branch_merge_rows.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/main/connection.hpp"

namespace duckdb {
class ClientContext;

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

} // namespace duckdb
