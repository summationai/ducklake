//===----------------------------------------------------------------------===//
//                         DuckDB
//
// branching/ducklake_branching.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/set.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/common/unordered_set.hpp"

#include <functional>

namespace duckdb {
class ClientContext;
class DBConfig;
class DuckLakeDelete;
class DuckLakeTransaction;
class ExtensionLoader;
class FileSystem;
struct DuckLakeCommitContext;
struct DuckLakeCompactionFileEntry;
struct DuckLakeDeleteFile;
struct DuckLakeFileListEntry;
struct DuckLakeFileListExtendedEntry;
struct DuckLakeInlinedTableInfo;
struct DuckLakeSnapshot;
struct DuckLakeSnapshotInfo;
struct TableIndex;
enum class CompactionType;

//! Every call DuckLake's own code makes into branching. Upstream files contain only these calls (listed with their
//! call sites in docs/branching/TOUCHPOINTS.md); everything else about branches lives in src/branching.
class DuckLakeBranching {
public:
	//! ducklake_extension.cpp - the branch functions and the CREATE/SET/DROP/MERGE BRANCH statements
	static void Register(ExtensionLoader &loader, DBConfig &config);

	//===--------------------------------------------------------------------===//
	// Transaction lifecycle
	//===--------------------------------------------------------------------===//
	//! DuckLakeTransactionManager::StartTransaction - puts the transaction on the connection's selected branch
	static void OnTransactionStart(DuckLakeTransaction &transaction, ClientContext &context);
	static bool IsOnBranch(DuckLakeTransaction &transaction);
	//! Throws for an operation a branch does not support
	static void EnsureNotOnBranch(DuckLakeTransaction &transaction, const char *operation);
	//! DuckLakeTransaction::GetSnapshot - a branch transaction reads main at its fork, with the branch loaded
	static bool TryGetSnapshot(DuckLakeTransaction &transaction, DuckLakeSnapshot &result);
	//! DuckLakeTransaction::Commit - commits to a branch, or merges a branch into main
	static bool TryCommit(DuckLakeTransaction &transaction);
	//! DuckLakeTransaction::RunCommitLoop - a merge checks its branch and records itself in each attempt
	static void PrepareCommitLoop(DuckLakeTransaction &transaction, DuckLakeCommitContext &context);
	//! DuckLakeTransaction::DeleteSnapshots - keeps the snapshots an open branch needs
	static void DeleteSnapshots(DuckLakeTransaction &transaction, const vector<DuckLakeSnapshotInfo> &snapshots);

	//===--------------------------------------------------------------------===//
	// Reading and writing on a branch
	//===--------------------------------------------------------------------===//
	//! DuckLakeCatalog::GetInliningLimit - branch writes always go to files
	static idx_t InliningLimit(DuckLakeTransaction &transaction, idx_t limit);
	//! DuckLakeFunctionInfo / DuckLakeTableEntry::CanUseGlobalStats - global stats describe main's head
	static bool CanUseGlobalStats(DuckLakeTransaction &transaction);
	//! DuckLakeDelete::FlushDelete - writes a branch delete on a main file
	static bool TryFlushDelete(const DuckLakeDelete &op, DuckLakeTransaction &transaction, ClientContext &context,
	                           unordered_map<string, DuckLakeDeleteFile> &written_files, const string &filename,
	                           const DuckLakeFileListExtendedEntry &data_file_info, set<idx_t> &deletes,
	                           DuckLakeDeleteFile &delete_file);
	//! DuckLakeDelete::TryDropFullyDeletedFile - files of earlier branch commits stay, so later row ids do not shift
	static bool KeepsLocalFile(DuckLakeTransaction &transaction, TableIndex table_id, const string &path);
	//! DuckLakeMultiFileList::GetFilesForTable - a branch delete file already holds main's inlined file deletions
	static void PrepareLocalDelete(DuckLakeTransaction &transaction, TableIndex table_id,
	                               DuckLakeFileListEntry &file_entry);
	//! LocalTableChanges - delete files of earlier branch commits are never removed by this transaction
	static void RemoveDeleteFile(FileSystem &fs, const DuckLakeDeleteFile &file);
	static void TryRemoveDeleteFile(FileSystem &fs, const DuckLakeDeleteFile &file);
	static bool OwnsDeleteFile(const DuckLakeDeleteFile &file);

	//===--------------------------------------------------------------------===//
	// Maintenance on main keeps what open branches read
	//===--------------------------------------------------------------------===//
	//! ducklake_expire_snapshots - a filter prefix ("<condition> AND ") on expirable snapshots, or empty
	static string ExpirableSnapshotFilter(DuckLakeTransaction &transaction);
	//! ducklake_compaction_functions - merging files never removes a file an open branch sees at its fork
	static void FilterCompactionCandidates(DuckLakeTransaction &transaction, CompactionType type,
	                                       vector<DuckLakeCompactionFileEntry> &files);
	//! ducklake_flush_inlined_data - inlined rows an open branch sees at its fork stay inlined
	static bool IsInlinedTablePinned(DuckLakeTransaction &transaction, const DuckLakeInlinedTableInfo &inlined_table);
	//! DuckLakeMetadataManager::GetOrphanFilesForCleanup - branch files are not orphans
	static void AddKnownFiles(DuckLakeTransaction &transaction, unordered_set<string> &known_files,
	                          const std::function<string(const string &)> &canonical_path);
};

} // namespace duckdb
