//===----------------------------------------------------------------------===//
//                         DuckDB
//
// branching/ducklake_branch_functions.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "functions/ducklake_table_functions.hpp"

namespace duckdb {

class DuckLakeBranchFunctions {
public:
	static TableFunction GetCreateBranchFunction();
	static TableFunction GetDropBranchFunction();
	static TableFunction GetSetBranchFunction();
	static TableFunction GetMergeBranchFunction();
};

class DuckLakeCurrentBranchFunction : public DuckLakeBaseMetadataFunction {
public:
	DuckLakeCurrentBranchFunction();
};

class DuckLakeBranchesFunction : public DuckLakeBaseMetadataFunction {
public:
	DuckLakeBranchesFunction();
};

} // namespace duckdb
