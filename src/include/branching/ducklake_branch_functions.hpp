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
	//! if_exists_positional: DROP BRANCH IF EXISTS passes the flag as a third positional argument
	static TableFunction GetDropBranchFunction(bool if_exists_positional = false);
	static TableFunction GetSetBranchFunction();
	//! on_conflict_positional: MERGE BRANCH ... ON CONFLICT KEEP passes it as a third positional argument
	static TableFunction GetMergeBranchFunction(bool on_conflict_positional = false);
	static TableFunction GetBranchTableFunction();
	static TableFunction GetCloseStaleBranchesFunction();
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
