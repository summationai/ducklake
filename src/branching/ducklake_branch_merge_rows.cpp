#include "branching/ducklake_branch_merge_rows.hpp"

#include "common/ducklake_util.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/query_result.hpp"

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

} // namespace duckdb
