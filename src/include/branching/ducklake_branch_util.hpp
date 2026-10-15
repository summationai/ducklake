//===----------------------------------------------------------------------===//
//                         DuckDB
//
// branching/ducklake_branch_util.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/string_util.hpp"
#include "duckdb/main/query_result.hpp"
#include "storage/ducklake_catalog.hpp"
#include "storage/ducklake_delete.hpp"
#include "storage/ducklake_transaction.hpp"

namespace duckdb {

//! Runs a query on the metadata catalog and throws with the given prefix when it fails
inline unique_ptr<QueryResult> RunBranchQuery(DuckLakeTransaction &transaction, string query, const string &error) {
	auto result = transaction.Query(std::move(query));
	if (result->HasError()) {
		result->GetErrorObject().Throw(error);
	}
	return result;
}

//! The full path of a path stored in a branch table
inline string LoadBranchPath(DuckLakeCatalog &catalog, const string &base_path, const string &path, bool relative) {
	auto result = relative ? base_path + path : path;
	auto &separator = catalog.Separator();
	if (separator != "/") {
		result = StringUtil::Replace(result, "/", separator);
	}
	return result;
}

//! Writes a delete file, as a deletion vector when the table uses them
inline DuckLakeDeleteFile WriteBranchDeleteFile(ClientContext &context, WriteDeleteFileInput &input,
                                                bool use_deletion_vectors) {
	return use_deletion_vectors ? DuckLakeDeleteFileWriter::WriteDeletionVectorFile(context, input)
	                            : DuckLakeDeleteFileWriter::WriteDeleteFile(context, input);
}

//! A deletion vector keeps no per-row snapshots, as with DuckLake's own deletes
inline DuckLakeDeleteFile WriteBranchDeleteFile(ClientContext &context, WriteDeleteFileWithSnapshotsInput &input,
                                                bool use_deletion_vectors) {
	if (!use_deletion_vectors) {
		return DuckLakeDeleteFileWriter::WriteDeleteFileWithSnapshots(context, input);
	}
	set<idx_t> positions;
	for (auto &position : input.positions) {
		positions.insert(NumericCast<idx_t>(position.position));
	}
	WriteDeleteFileInput plain {input.context,        input.transaction,    input.fs,
	                            input.data_path,      input.encryption_key, input.data_file_path,
	                            std::move(positions), input.source};
	return DuckLakeDeleteFileWriter::WriteDeletionVectorFile(context, plain);
}

//! Whether a table or view id was handed out by the current transaction
inline bool IsTransactionLocal(const TableIndex &id) {
	return id.IsTransactionLocal();
}

inline void AppendValues(string &target, const string &values) {
	if (!target.empty()) {
		target += ", ";
	}
	target += values;
}

//! "a, b, c" for the ids, for an IN list
template <class T>
string BranchIdList(const T &ids) {
	string result;
	for (auto &id : ids) {
		AppendValues(result, to_string(id));
	}
	return result;
}

} // namespace duckdb
