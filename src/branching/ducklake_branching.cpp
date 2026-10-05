#include "branching/ducklake_branching.hpp"

#include "branching/ducklake_branch.hpp"
#include "branching/ducklake_branch_functions.hpp"
#include "branching/ducklake_branch_parser.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/parser_extension.hpp"
#include "storage/ducklake_catalog.hpp"
#include "storage/ducklake_metadata_info.hpp"
#include "storage/ducklake_metadata_manager.hpp"
#include "storage/ducklake_transaction.hpp"
#include "storage/ducklake_transaction_state.hpp"

namespace duckdb {

void DuckLakeBranching::Register(ExtensionLoader &loader, DBConfig &config) {
	loader.RegisterFunction(DuckLakeBranchFunctions::GetCreateBranchFunction());
	loader.RegisterFunction(DuckLakeBranchFunctions::GetDropBranchFunction());
	loader.RegisterFunction(DuckLakeBranchFunctions::GetSetBranchFunction());
	loader.RegisterFunction(DuckLakeBranchFunctions::GetMergeBranchFunction());
	DuckLakeCurrentBranchFunction current_branch;
	loader.RegisterFunction(current_branch);
	DuckLakeBranchesFunction branches;
	loader.RegisterFunction(branches);
	ParserExtension::Register(config, DuckLakeBranchParserExtension());
}

//===--------------------------------------------------------------------===//
// Transaction lifecycle
//===--------------------------------------------------------------------===//
void DuckLakeBranching::OnTransactionStart(DuckLakeTransaction &transaction, ClientContext &context) {
	auto &catalog = transaction.GetCatalog();
	if (!catalog.IsInitialized() || catalog.CatalogSnapshot()) {
		return;
	}
	if (context.registered_state->Get<DuckLakeInternalConnectionState>(DuckLakeInternalConnectionState::KEY)) {
		return;
	}
	auto branch_state = context.registered_state->Get<DuckLakeBranchState>(DuckLakeBranchState::KEY);
	DuckLakeBranchState::Selection selection;
	if (branch_state && branch_state->TryGetSelection(catalog.GetOid(), selection)) {
		DuckLakeBranchManager::SetBranch(transaction, selection.branch_id, selection.name);
	}
}

bool DuckLakeBranching::IsOnBranch(DuckLakeTransaction &transaction) {
	return DuckLakeBranchManager::IsOnBranch(transaction);
}

void DuckLakeBranching::EnsureNotOnBranch(DuckLakeTransaction &transaction, const char *operation) {
	DuckLakeBranchManager::EnsureNotOnBranch(transaction, operation);
}

bool DuckLakeBranching::TryGetSnapshot(DuckLakeTransaction &transaction, DuckLakeSnapshot &result) {
	if (!IsOnBranch(transaction)) {
		return false;
	}
	result = DuckLakeBranchManager::GetBranchSnapshot(transaction);
	return true;
}

bool DuckLakeBranching::TryCommit(DuckLakeTransaction &transaction) {
	if (IsOnBranch(transaction)) {
		DuckLakeBranchManager::CommitToBranch(transaction);
		return true;
	}
	if (DuckLakeBranchManager::IsMergingBranch(transaction)) {
		DuckLakeBranchManager::CommitMerge(transaction);
		return true;
	}
	return false;
}

void DuckLakeBranching::PrepareCommitLoop(DuckLakeTransaction &transaction, DuckLakeCommitContext &context) {
	auto state = DuckLakeBranchManager::GetState(transaction);
	if (!state || !state->merge) {
		return;
	}
	// a merge starts at the fork snapshot: its checks run on every attempt, its bookkeeping commits with it
	auto &merge = *state->merge;
	context.pre_commit_check = [&transaction, &merge](const SnapshotChangeInformation &other_changes) {
		DuckLakeBranchManager::CheckMerge(transaction, merge, other_changes);
	};
	context.extra_commit_sql = DuckLakeBranchManager::MergeBookkeepingSql(transaction, merge, true);
}

void DuckLakeBranching::DeleteSnapshots(DuckLakeTransaction &transaction,
                                        const vector<DuckLakeSnapshotInfo> &snapshots) {
	auto &metadata_manager = transaction.GetMetadataManager();
	auto forks = DuckLakeBranchManager::GetActiveForkSnapshots(transaction);
	if (forks.empty()) {
		metadata_manager.DeleteSnapshots(snapshots);
		return;
	}
	// a branch may have forked since these snapshots were selected - it needs its fork and everything after it
	auto oldest_fork = *std::min_element(forks.begin(), forks.end());
	vector<DuckLakeSnapshotInfo> expirable;
	for (auto &snapshot : snapshots) {
		if (snapshot.id < oldest_fork) {
			expirable.push_back(snapshot);
		}
	}
	if (!expirable.empty()) {
		metadata_manager.DeleteSnapshots(expirable);
	}
}

//===--------------------------------------------------------------------===//
// Reading and writing on a branch
//===--------------------------------------------------------------------===//
idx_t DuckLakeBranching::InliningLimit(DuckLakeTransaction &transaction, idx_t limit) {
	return IsOnBranch(transaction) ? 0 : limit;
}

bool DuckLakeBranching::CanUseGlobalStats(DuckLakeTransaction &transaction) {
	return !IsOnBranch(transaction);
}

bool DuckLakeBranching::TryFlushDelete(const DuckLakeDelete &op, DuckLakeTransaction &transaction,
                                       ClientContext &context, unordered_map<string, DuckLakeDeleteFile> &written_files,
                                       const string &filename, const DuckLakeFileListExtendedEntry &data_file_info,
                                       set<idx_t> &deletes, DuckLakeDeleteFile &delete_file) {
	if (!IsOnBranch(transaction) || !data_file_info.file_id.IsValid()) {
		return false;
	}
	DuckLakeBranchManager::FlushBranchDelete(op, transaction, context, written_files, filename, data_file_info,
	                                         std::move(deletes), delete_file);
	return true;
}

bool DuckLakeBranching::KeepsLocalFile(DuckLakeTransaction &transaction, TableIndex table_id, const string &path) {
	return IsOnBranch(transaction) && DuckLakeBranchManager::IsLoadedBranchFile(transaction, table_id, path);
}

void DuckLakeBranching::PrepareLocalDelete(DuckLakeTransaction &transaction, TableIndex table_id,
                                           DuckLakeFileListEntry &file_entry) {
	if (IsOnBranch(transaction) && transaction.HasLocalDeleteForFile(table_id, file_entry.file.path)) {
		file_entry.inlined_file_deletions.clear();
	}
}

void DuckLakeBranching::RemoveDeleteFile(FileSystem &fs, const DuckLakeDeleteFile &file) {
	if (OwnsDeleteFile(file)) {
		fs.RemoveFile(file.file_name);
	}
}

void DuckLakeBranching::TryRemoveDeleteFile(FileSystem &fs, const DuckLakeDeleteFile &file) {
	if (OwnsDeleteFile(file)) {
		fs.TryRemoveFile(file.file_name);
	}
}

bool DuckLakeBranching::OwnsDeleteFile(const DuckLakeDeleteFile &file) {
	return file.created_by_ducklake;
}

//===--------------------------------------------------------------------===//
// Maintenance on main
//===--------------------------------------------------------------------===//
string DuckLakeBranching::ExpirableSnapshotFilter(DuckLakeTransaction &transaction) {
	if (!DuckLakeBranchManager::HasBranchTables(transaction)) {
		return string();
	}
	return DuckLakeBranchManager::ExpirableSnapshotFilter() + " AND ";
}

void DuckLakeBranching::FilterCompactionCandidates(DuckLakeTransaction &transaction, CompactionType type,
                                                   vector<DuckLakeCompactionFileEntry> &files) {
	if (type != CompactionType::MERGE_ADJACENT_TABLES) {
		return;
	}
	auto forks = DuckLakeBranchManager::GetActiveForkSnapshots(transaction);
	if (forks.empty()) {
		return;
	}
	// merging removes the source files - keep every file an active branch can still see at its fork
	auto newest_fork = *std::max_element(forks.begin(), forks.end());
	vector<DuckLakeCompactionFileEntry> candidates;
	for (auto &file : files) {
		if (file.file.begin_snapshot > newest_fork) {
			candidates.push_back(std::move(file));
		}
	}
	files = std::move(candidates);
}

bool DuckLakeBranching::IsInlinedTablePinned(DuckLakeTransaction &transaction,
                                             const DuckLakeInlinedTableInfo &inlined_table) {
	auto forks = DuckLakeBranchManager::GetActiveForkSnapshots(transaction);
	if (forks.empty()) {
		return false;
	}
	auto newest_fork = *std::max_element(forks.begin(), forks.end());
	auto &metadata_manager = transaction.GetMetadataManager();
	auto col_names = metadata_manager.InlinedColNames();
	auto result = metadata_manager.Query(
	    StringUtil::Format("SELECT 1 FROM {METADATA_CATALOG}.%s WHERE %s <= %d AND (%s IS NULL OR %s > %d) LIMIT 1",
	                       inlined_table.table_name, col_names.begin_snapshot, newest_fork, col_names.end_snapshot,
	                       col_names.end_snapshot, newest_fork));
	if (result->HasError()) {
		result->GetErrorObject().Throw("Failed to check inlined data against active branches: ");
	}
	for (auto &row : *result) {
		(void)row;
		return true;
	}
	return false;
}

void DuckLakeBranching::AddKnownFiles(DuckLakeTransaction &transaction, unordered_set<string> &known_files,
                                      const std::function<string(const string &)> &canonical_path) {
	if (!DuckLakeBranchManager::HasBranchTables(transaction)) {
		return;
	}
	// files written on active branches are not listed in the main file tables
	auto result = transaction.GetMetadataManager().Query(DuckLakeBranchManager::ActiveBranchFilesQuery());
	if (result->HasError()) {
		result->GetErrorObject().Throw("Failed to get branch files from DuckLake: ");
	}
	for (auto &row : *result) {
		known_files.insert(canonical_path(row.GetValue<string>(0)));
	}
}

} // namespace duckdb
