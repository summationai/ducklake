//===----------------------------------------------------------------------===//
//                         DuckDB
//
// branching/ducklake_branch_parser.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/parser/parser_extension.hpp"

namespace duckdb {

//! Parses CREATE BRANCH, DROP BRANCH and SET BRANCH for the current DuckLake database
class DuckLakeBranchParserExtension : public ParserExtension {
public:
	DuckLakeBranchParserExtension();
};

} // namespace duckdb
