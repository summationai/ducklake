# Branching: where upstream DuckLake code calls in

All branching code lives in `src/branching/` and `src/include/branching/`. Upstream files contain only calls to
`DuckLakeBranching` (`src/include/branching/ducklake_branching.hpp`), plus the few declarations listed below. When a
rebase onto a new DuckLake release conflicts, these are the lines to re-apply. Each one sits at a function boundary or
replaces a single existing line, so the place to put it back is usually obvious from the function name.

Footprint against upstream: 18 files, +81 / -14 lines. Check it with:

```bash
git diff --stat <upstream release> -- src ':!src/branching' ':!src/include/branching'
```

## Calls

| Upstream file | Function | Call | What it does |
| --- | --- | --- | --- |
| `src/CMakeLists.txt` | | `add_subdirectory(branching)` | builds `src/branching/` |
| `src/ducklake_extension.cpp` | `LoadInternal`, at the end | `Register(loader, config)` | the branch functions and the `CREATE/SET/DROP BRANCH` parser extension |
| `src/storage/ducklake_transaction_manager.cpp` | `StartTransaction`, after `Start()` | `OnTransactionStart(*transaction, context)` | puts the transaction on the connection's selected branch |
| | same, the immediate-mode condition | `&& !IsOnBranch(*transaction)` | a branch is loaded on first use, not at transaction start |
| `src/storage/ducklake_transaction.cpp` | `GetSnapshot()`, at entry | `TryGetSnapshot(*this, branch_snapshot)` | a branch reads main at its fork, with the branch's changes loaded |
| | `Commit()`, first statement in the `try` | `TryCommit(*this)` | commits a branch transaction to its branch |
| | `DeleteSnapshots` | `DeleteSnapshots(*this, snapshots)` (replaces `metadata_manager.DeleteSnapshots`) | keeps the snapshots an open branch needs |
| | `CreateEntry`, `DropEntry`, `AlterEntry`, `GetSnapshot(at_clause)`, `SetConfigOption`, `ResetConfigOption`, `DeleteSnapshots`, `DeleteInlinedData`, `MarkInlinedDataForDeletion`, `AddCompaction`, `AddNameMap`, at entry | `EnsureNotOnBranch(*this, "...")` | operations a branch does not support yet |
| | `LocalTableChanges::CleanupFiles` (both), `DropTransactionLocalFile`, `AddDeletesToMap` | `RemoveDeleteFile` / `TryRemoveDeleteFile(fs, file)` (replace `fs.RemoveFile` / `fs.TryRemoveFile` of a delete file) | delete files of earlier branch commits are never removed by this transaction |
| | `TransactionLocalDelete` | `if (OwnsDeleteFile(old_file))` around `files_to_delete.push_back` | same, for the batched removal |
| `src/storage/ducklake_delete.cpp` | `FlushDelete`, after `delete_file.data_file_id` is set | `TryFlushDelete(...)` | a branch delete on a main file keeps main's deletes at the fork plus the branch's |
| | `TryDropFullyDeletedFile`, the transaction-local branch | `KeepsLocalFile(...)` | files of earlier branch commits stay, so later row ids do not shift |
| `src/storage/ducklake_catalog.cpp` | `GetInliningLimit`, the final return | `return InliningLimit(transaction, limit)` | branch writes always go to files |
| `src/storage/ducklake_scan.cpp` | `DuckLakeFunctionInfo::CanUseGlobalStats` | `&& CanUseGlobalStats(*active_transaction)` | global stats describe main's head, not a branch |
| `src/storage/ducklake_table_entry.cpp` | `DuckLakeTableEntry::CanUseGlobalStats` | `&& CanUseGlobalStats(transaction)` | same |
| `src/storage/ducklake_multi_file_list.cpp` | `GetFilesForTable`, the local-deletes loop | `PrepareLocalDelete(...)` | a branch delete file already holds main's inlined file deletions |
| `src/storage/ducklake_metadata_manager.cpp` | `GetOrphanFilesForCleanup`, after the known files are read | `AddKnownFiles(...)` | branch files are not orphans |
| `src/functions/ducklake_expire_snapshots.cpp` | bind, after the "never the latest snapshot" filter | `filter += ExpirableSnapshotFilter(...)` | open branches pin their fork snapshot |
| | bind, after `DuckLakeTransaction::Get` | `EnsureNotOnBranch` | |
| `src/functions/ducklake_flush_inlined_data.cpp` | bind, after `DuckLakeTransaction::Get` | `EnsureNotOnBranch` | |
| | bind, the per-inlined-table loop | `if (IsInlinedTablePinned(...)) continue;` | inlined rows an open branch sees at its fork stay inlined |
| `src/functions/ducklake_compaction_functions.cpp` | bind, after `DuckLakeTransaction::Get` | `EnsureNotOnBranch` | |
| | after `GetFilesForCompaction` | `FilterCompactionCandidates(transaction, type, files)` | merging files never removes a file an open branch sees at its fork |
| `src/functions/ducklake_cleanup_files.cpp` | bind, after `DuckLakeTransaction::Get` | `EnsureNotOnBranch` | |
| `src/functions/ducklake_add_data_files.cpp` | execute, after `DuckLakeTransaction::Get` | `EnsureNotOnBranch` | |

Every `.cpp` file above that calls `DuckLakeBranching` also includes `branching/ducklake_branching.hpp` right after its first `#include`.

## Declarations

| Upstream file | Declaration | Why it cannot be a call |
| --- | --- | --- |
| `include/storage/ducklake_transaction.hpp` | `shared_ptr<DuckLakeBranchTransactionState> branch_state;` next to `state` | the transaction's branch state; opaque, so the header needs only a forward declaration |
| | `friend class DuckLakeBranchManager;` in `DuckLakeTransaction` and `LocalTableChanges` | branching code reads the transaction's snapshot, connection and local changes |
| `include/storage/ducklake_delete.hpp` | `friend class DuckLakeBranchManager;` | the branch delete writer uses `TryDropFullyDeletedFile` |
| `include/common/ducklake_data_file.hpp` | `DuckLakeDeleteFile::created_by_ducklake` | marks delete files of earlier branch commits, as `DuckLakeDataFile::created_by_ducklake` already marks data files |

## Private members branching code depends on

These are reached through the `DuckLakeBranchManager` friend declarations. A rebase can apply cleanly and still fail to
compile in `src/branching/` when upstream changes them; that is where to fix it.

- `DuckLakeTransaction`: `state`, `snapshot`, `snapshot_lock`, `connection`, `branch_state`
- `LocalTableChanges`: `lock`, `changes`
- `DuckLakeDelete`: `TryDropFullyDeletedFile`
- `DuckLakeTransactionState` (public): `local_changes`, `dropped_files`, `dropped_file_stats`, `tables_deleted_from`

## After a rebase

1. `make release`. Compile errors in `src/branching/` mean upstream changed one of the members above.
2. `build/release/test/unittest "test/sql/branch/*"`, also with `--test-config test/configs/{no_inline,deletion_vectors,sqlite,ducklake_version}.json`.
3. The regression directories: `transaction delete update data_inlining deletion_inlining compaction cleanup remove_orphans time_travel table_changes stats snapshot_info checkpoint partitioning`.
4. `scripts/branching/coverage.sh` regenerates [COVERAGE.md](COVERAGE.md) and fails if a function in `src/branching/` is
   never executed or an uncovered line has no reason.
5. Check for a new upstream file-removal site in `LocalTableChanges` that is not routed through `RemoveDeleteFile` /
   `TryRemoveDeleteFile`: it would delete a branch's files on rollback.
