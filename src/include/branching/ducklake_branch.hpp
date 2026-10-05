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
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/common/unordered_set.hpp"
#include "duckdb/main/client_context_state.hpp"

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

	//! Loads the branch head into the transaction's local changes
	static void LoadBranch(DuckLakeTransaction &transaction, const DuckLakeBranchInfo &branch,
	                       DuckLakeSnapshot fork_snapshot, DuckLakeLoadedBranch &loaded);
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
	static void SetBranch(DuckLakeTransaction &transaction, idx_t branch_id, string branch_name);
	static void EnsureNotOnBranch(DuckLakeTransaction &transaction, const string &operation);
	//! The fork snapshot of the transaction's branch; loads the branch head on first use
	static DuckLakeSnapshot GetBranchSnapshot(DuckLakeTransaction &transaction);
	static void CommitToBranch(DuckLakeTransaction &transaction);
	//! Whether the transaction-local data file was loaded from an earlier branch commit
	static bool IsLoadedBranchFile(DuckLakeTransaction &transaction, TableIndex table_id, const string &path);
	//! Writes a delete on a main data file from a branch: main's deletes at the fork plus the branch's deletes
	static void FlushBranchDelete(const DuckLakeDelete &op, DuckLakeTransaction &transaction, ClientContext &context,
	                              unordered_map<string, DuckLakeDeleteFile> &written_files, const string &filename,
	                              const DuckLakeFileListExtendedEntry &data_file_info, set<idx_t> deletes,
	                              DuckLakeDeleteFile &delete_file);
};

} // namespace duckdb
