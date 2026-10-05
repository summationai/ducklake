//===----------------------------------------------------------------------===//
//                         DuckDB
//
// branching/ducklake_branch.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "common/ducklake_snapshot.hpp"
#include "common/index.hpp"
#include "duckdb/common/common.hpp"
#include "duckdb/common/enums/catalog_type.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/common/unordered_set.hpp"
#include "duckdb/main/client_context_state.hpp"

#include <functional>
#include <thread>

namespace duckdb {
class ClientContext;
class DuckLakeCatalog;
class DuckLakeTransaction;
class DuckLakeTransactionState;
class FileSystem;
struct DuckLakeCommitContext;
struct DuckLakeDeleteFile;
struct DuckLakeFileListExtendedEntry;
struct DuckLakeInlinedTableInfo;
struct DuckLakeSnapshotCommit;
struct SnapshotChangeInformation;
struct TransactionChangeInformation;
class DuckLakeDelete;

struct DuckLakeBranchInfo {
	idx_t branch_id = 0;
	string name;
	idx_t fork_snapshot_id = 0;
	idx_t head_seq = 0;
	idx_t next_file_seq = 0;
	string status;
	Value created_at;

	bool IsActive() const {
		return status == "active";
	}
};

//! The branch selected per attached DuckLake catalog (keyed by the catalog's oid) for one connection
class DuckLakeBranchState : public ClientContextState {
public:
	static constexpr const char *KEY = "ducklake_branch_state";

	struct Selection {
		idx_t branch_id;
		string name;
	};

	bool TryGetSelection(idx_t catalog_oid, Selection &result) const;
	void Select(idx_t catalog_oid, idx_t branch_id, string name);
	void Clear(idx_t catalog_oid);
	//! Clears the selection if it points at the given branch
	void ClearIfSelected(idx_t catalog_oid, idx_t branch_id);

private:
	mutable mutex lock;
	unordered_map<idx_t, Selection> selections;
};

//! The branch head as loaded into a transaction - used to find what a commit adds
struct DuckLakeLoadedBranch {
	DuckLakeBranchInfo info;
	//! Branch data files (full path -> branch file id)
	unordered_map<string, idx_t> data_files;
	//! Branch delete files (full path -> branch file id)
	unordered_map<string, idx_t> delete_files;
	//! Deletes of inlined rows on main (table -> inlined table name -> row ids)
	map<TableIndex, map<string, set<idx_t>>> inlined_deletes;
	//! Main data files dropped on the branch (data file id -> table)
	map<idx_t, TableIndex> dropped_files;
	//! A main data file the branch deleted from, with the branch's delete file for it
	struct MainDelete {
		TableIndex table_id;
		idx_t data_file_id;
		string data_file_path;
		string branch_delete_file;
	};
	vector<MainDelete> main_deletes;
};

//! A table both sides changed rows of since the fork, merged row by row: where to read it, and what the merge does to
//! the rows both sides touched
struct DuckLakeRowMergeTable {
	TableIndex table_id;
	string schema_name;
	//! Its name on main and on the branch
	string main_name;
	string branch_name;
	//! The rows both sides touched since the fork, rewrites with the old values included
	set<int64_t> overlap;
	//! The rows both sides changed differently
	vector<int64_t> conflicts;
	//! The rows both sides changed the same way
	set<int64_t> same_as_main;
	//! The branch's copies of rows main's copy stays for: branch data file -> positions
	map<string, set<idx_t>> branch_rows_to_drop;
	//! Main's copies of rows the branch's change wins for: data file or inlined table at main's head -> (position,
	//! row id)
	map<string, vector<pair<idx_t, int64_t>>> main_rows_to_drop;
};

//! A branch being merged into main by the current transaction
struct DuckLakeBranchMerge {
	DuckLakeLoadedBranch loaded;
	DuckLakeSnapshot fork_snapshot;
	//! Branch files main does not take over: (branch file id, full path), scheduled for deletion by the merge
	vector<pair<idx_t, string>> files_to_schedule;
	//! The transaction's changes once the merge was prepared - nothing may be added before it commits
	string changes_fingerprint;
	//! Main's head the merge was planned on
	DuckLakeSnapshot head_snapshot;
	//! The tables merged row by row
	map<TableIndex, DuckLakeRowMergeTable> row_merge;
};

//! Branch state of one DuckLake transaction, held by the transaction as an opaque pointer
struct DuckLakeBranchTransactionState {
	//! The branch the transaction reads and writes (if any)
	optional_idx branch_id;
	string branch_name;
	//! The branch head loaded into the local changes
	unique_ptr<DuckLakeLoadedBranch> loaded_branch;
	mutex load_lock;
	//! The fork snapshot while the branch is being loaded, for re-entrant snapshot lookups
	unique_ptr<DuckLakeSnapshot> loading_fork_snapshot;
	std::thread::id loading_thread;
	//! The branch the transaction merges into main on commit (if any)
	unique_ptr<DuckLakeBranchMerge> merge;
};

class DuckLakeBranchManager {
public:
	//! Branch file ids live above every id main hands out: BASE + branch_id * 2^32 + per-branch sequence
	static constexpr idx_t BRANCH_FILE_ID_BASE = idx_t(1) << 62;
	static constexpr const char *MAIN_BRANCH_NAME = "main";

	//! Whether the branch metadata tables exist - probed again while they are absent
	static bool HasBranchTables(DuckLakeTransaction &transaction);
	static void SetHasBranchTables(DuckLakeTransaction &transaction, bool value);
	static bool IsBranchTablesCached(DuckLakeTransaction &transaction);
	//! Creates the branch metadata tables if needed; returns whether this call created them
	static bool CreateTables(DuckLakeTransaction &transaction);

	static unique_ptr<DuckLakeBranchInfo> GetBranch(DuckLakeTransaction &transaction, idx_t branch_id);
	static unique_ptr<DuckLakeBranchInfo> GetActiveBranch(DuckLakeTransaction &transaction, const string &name);
	static vector<DuckLakeBranchInfo> GetBranches(DuckLakeTransaction &transaction);
	static DuckLakeBranchInfo CreateBranch(DuckLakeTransaction &transaction, const string &name);
	static void DropBranch(DuckLakeTransaction &transaction, const DuckLakeBranchInfo &branch);

	//! Loads the branch head into the transaction's local changes; a merge also needs stats and partition values
	static void LoadBranch(DuckLakeTransaction &transaction, const DuckLakeBranchInfo &branch,
	                       DuckLakeSnapshot fork_snapshot, DuckLakeLoadedBranch &loaded, bool for_merge = false);
	//! Prepares the current main transaction to merge the branch when it commits
	static DuckLakeBranchInfo PrepareMerge(DuckLakeTransaction &transaction, const string &name,
	                                       optional_ptr<const DuckLakeSnapshotCommit> commit_info);
	//! Runs with every conflict check of a merge commit
	static void CheckMerge(DuckLakeTransaction &transaction, const DuckLakeBranchMerge &merge,
	                       const SnapshotChangeInformation &other_changes);
	//! The SQL that records the merge; part of the merge commit's batch
	static string MergeBookkeepingSql(DuckLakeTransaction &transaction, const DuckLakeBranchMerge &merge,
	                                  bool with_snapshot);
	//! Filter on ducklake_snapshot rows that no active branch needs (its fork and everything after it)
	static string ExpirableSnapshotFilter();
	//! Writes the transaction's new local changes as the next branch commit
	static void CommitBranch(DuckLakeTransaction &transaction, DuckLakeLoadedBranch &loaded);

	//! Subquery returning the fork snapshot ids of all active branches
	static string ActiveForkSnapshotsQuery();
	//! Query returning the full paths of all files owned by active branches
	static string ActiveBranchFilesQuery();
	//! The fork snapshot ids of all active branches
	static vector<idx_t> GetActiveForkSnapshots(DuckLakeTransaction &transaction);

	static void ValidateBranchName(const string &name);
	static void EnsureAutoCommit(ClientContext &context, const string &statement);

	//===--------------------------------------------------------------------===//
	// Branch state of a transaction (ducklake_branch_transaction.cpp)
	//===--------------------------------------------------------------------===//
	static optional_ptr<DuckLakeBranchTransactionState> GetState(DuckLakeTransaction &transaction);
	static DuckLakeBranchTransactionState &GetOrCreateState(DuckLakeTransaction &transaction);
	static DuckLakeTransactionState &GetTransactionState(DuckLakeTransaction &transaction);
	static bool IsOnBranch(DuckLakeTransaction &transaction);
	static bool IsMergingBranch(DuckLakeTransaction &transaction);
	static void SetBranch(DuckLakeTransaction &transaction, idx_t branch_id, string branch_name);
	static void SetBranchMerge(DuckLakeTransaction &transaction, unique_ptr<DuckLakeBranchMerge> merge);
	static void EnsureNotOnBranch(DuckLakeTransaction &transaction, const string &operation);
	//! The fork snapshot of the transaction's branch; loads the branch head on first use
	static DuckLakeSnapshot GetBranchSnapshot(DuckLakeTransaction &transaction);
	static void CommitToBranch(DuckLakeTransaction &transaction);
	static void CommitMerge(DuckLakeTransaction &transaction);
	//! A summary of everything the transaction changed, to tell whether a statement added changes
	static string ChangesFingerprint(DuckLakeTransaction &transaction);
	//! Removes a loaded data file and its delete files from the change set without touching disk
	static void ForgetFile(DuckLakeTransaction &transaction, TableIndex table_id, const string &path);
	//! Removes the delete files of a data file from the change set without touching disk
	static void ForgetDeleteFiles(DuckLakeTransaction &transaction, TableIndex table_id, const string &data_file_path);
	//! Whether the transaction-local data file was loaded from an earlier branch commit
	static bool IsLoadedBranchFile(DuckLakeTransaction &transaction, TableIndex table_id, const string &path);
	//! Writes a delete on a main data file from a branch: main's deletes at the fork plus the branch's deletes
	static void FlushBranchDelete(const DuckLakeDelete &op, DuckLakeTransaction &transaction, ClientContext &context,
	                              unordered_map<string, DuckLakeDeleteFile> &written_files, const string &filename,
	                              const DuckLakeFileListExtendedEntry &data_file_info, set<idx_t> deletes,
	                              DuckLakeDeleteFile &delete_file);

	//===--------------------------------------------------------------------===//
	// Row-by-row merge (ducklake_branch_row_merge.cpp)
	//===--------------------------------------------------------------------===//
	//! Main's changes in the snapshots after the given one
	static SnapshotChangeInformation MainChangesSince(DuckLakeTransaction &transaction, DuckLakeSnapshot snapshot);
	//! The tables both sides changed rows of that merge row by row; the others keep DuckLake's table-level rules
	static map<TableIndex, DuckLakeRowMergeTable> FindRowMergeTables(DuckLakeTransaction &transaction,
	                                                                 const DuckLakeLoadedBranch &loaded,
	                                                                 DuckLakeSnapshot fork_snapshot,
	                                                                 const SnapshotChangeInformation &main_changes);
	//! The ids among the given tables (or views), at least one, that main renamed since the fork
	static set<TableIndex> RenamedOnMain(DuckLakeTransaction &transaction, bool views, const set<TableIndex> &ids,
	                                     idx_t fork_snapshot_id);
	//! Why main's changes to a table since the fork keep it out of the row-by-row merge, if they do
	static bool KeepsTableLevelRules(TableIndex table_id, bool branch_deleted, bool branch_deleted_inlined,
	                                 const SnapshotChangeInformation &main_changes);
	//! Reads both sides' changes to the table since the fork and decides each row both touched
	static void PlanRowMerge(ClientContext &context, const string &catalog_name, const string &branch_name,
	                         idx_t fork_snapshot_id, idx_t head_snapshot_id, DuckLakeRowMergeTable &table);
	static string RowConflictMessage(const string &branch_name, const DuckLakeRowMergeTable &table);
	//! Fails on rows the two sides changed differently; leaves out the branch's copies of rows main's copy stays for
	static void ApplyRowMerge(DuckLakeTransaction &transaction, DuckLakeBranchMerge &merge);
	//! Takes a table merged row by row out of DuckLake's insert and delete rules
	static void ExcludeFromInsertDeleteRules(TableIndex table_id, TransactionChangeInformation &changes);
	//! Fails the merge when main changed a table merged row by row after the merge was planned
	static void CheckRowMergeTablesUnchanged(DuckLakeTransaction &transaction, const DuckLakeBranchMerge &merge);

private:
	static void RebaseMainDeletes(DuckLakeTransaction &transaction, DuckLakeBranchMerge &merge);
	static void DropFullyDeletedBranchFiles(DuckLakeTransaction &transaction, DuckLakeBranchMerge &merge);
	static void ForgetFilesMainEmptied(DuckLakeTransaction &transaction, DuckLakeBranchMerge &merge);
};

} // namespace duckdb
