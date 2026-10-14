# Branching: where upstream DuckLake code calls in

All branching code lives in `src/branching/` and `src/include/branching/`. Upstream files contain only calls to
`DuckLakeBranching` (`src/include/branching/ducklake_branching.hpp`), plus the few declarations listed below. When a
rebase onto a new DuckLake release conflicts, these are the lines to re-apply. Each one sits at a function boundary or
replaces a single existing line, so the place to put it back is usually obvious from the function name.

Footprint against upstream: 20 files, about +100 / -20 lines. Check it with:

```bash
git diff --stat <upstream release> -- src ':!src/branching' ':!src/include/branching'
```

## Calls

| Upstream file | Function | Call | What it does |
| --- | --- | --- | --- |
| `src/CMakeLists.txt` | | `add_subdirectory(branching)` | builds `src/branching/` |
| `src/ducklake_extension.cpp` | `LoadInternal`, at the end | `Register(loader, config)` | the branch functions and the `CREATE/SET/DROP/MERGE BRANCH` parser extension |
| `src/storage/ducklake_transaction_manager.cpp` | `StartTransaction`, after `Start()` | `OnTransactionStart(*transaction, context)` | puts the transaction on the connection's selected branch |
| | same, the immediate-mode condition | `&& !IsOnBranch(*transaction)` | a branch is loaded on first use, not at transaction start |
| `src/storage/ducklake_transaction.cpp` | `GetSnapshot()`, at entry | `TryGetSnapshot(*this, branch_snapshot)` | a branch reads main at its fork, with the branch's changes loaded |
| | `Commit()`, first statement in the `try` | `TryCommit(*this)` | commits to the branch, merges a branch into main, or closes a stale branch |
| | `RunCommitLoop`, before `state->Commit` | `PrepareCommitLoop(*this, context)` | a merge, or the close of a stale branch, checks its branch on every attempt and records itself in the snapshot's batch |
| | `DeleteSnapshots` | `DeleteSnapshots(*this, snapshots)` (replaces `metadata_manager.DeleteSnapshots`) | keeps the snapshots an open branch needs |
| | `CreateEntry`, `DropEntry`, `AlterEntry`, at entry | `CheckCreate(*this, *entry)`, `CheckDrop(*this, entry)`, `CheckAlter(*this, entry, new_entry.get())` | refuse the catalog changes a branch does not support yet |
| | `GetTransactionLocalSchemas`, `GetTransactionLocalSchema`, `GetCatalogVersion`, at entry | `EnsureLoaded(*this)` | a branch's catalog changes are loaded before the transaction's own schemas or its catalog version are read |
| | `GetSnapshot(at_clause)`, `SetConfigOption`, `ResetConfigOption`, `DeleteSnapshots`, `DeleteInlinedData`, `MarkInlinedDataForDeletion`, `AddCompaction`, `AddNameMap`, at entry | `EnsureNotOnBranch(*this, "...")` | operations a branch does not support yet |
| | `LocalTableChanges::CleanupFiles` (both), `DropTransactionLocalFile`, `AddDeletesToMap` | `RemoveDeleteFile` / `TryRemoveDeleteFile(fs, file)` (replace `fs.RemoveFile` / `fs.TryRemoveFile` of a delete file) | delete files of earlier branch commits are never removed by this transaction |
| | `TransactionLocalDelete` | `if (OwnsDeleteFile(old_file))` around `files_to_delete.push_back` | same, for the batched removal |
| `src/storage/ducklake_transaction_state.cpp` | `CheckForConflicts`, after the conflict check | `pre_commit_check(other_changes)` | runs the merge's own checks |
| | `Commit`, the attempt loop | `if (i > 0 \|\| context.pre_commit_check)`, `context.pre_commit_check` passed on, `batch_queries += context.extra_commit_sql` | a merge checks conflicts from the first attempt and commits its bookkeeping atomically |
| `src/storage/ducklake_delete.cpp` | `FlushDelete`, after `delete_file.data_file_id` is set | `TryFlushDelete(...)` | a branch delete on a main file keeps main's deletes at the fork plus the branch's |
| | `TryDropFullyDeletedFile`, the transaction-local branch | `KeepsLocalFile(...)` | files of earlier branch commits stay, so later row ids do not shift |
| `src/storage/ducklake_catalog.cpp` | `GetInliningLimit`, the final return | `return InliningLimit(transaction, limit)` | branch writes always go to files |
| `src/storage/ducklake_scan.cpp` | `DuckLakeFunctionInfo::CanUseGlobalStats` | `&& CanUseGlobalStats(*active_transaction)` | global stats describe main's head, not a branch |
| `src/storage/ducklake_table_entry.cpp` | `DuckLakeTableEntry::CanUseGlobalStats` | `&& CanUseGlobalStats(transaction)` | same |
| `src/storage/ducklake_multi_file_list.cpp` | `GetFilesForTable`, the local-deletes loop | `PrepareLocalDelete(...)` | a branch delete file already holds main's inlined file deletions |
| `src/storage/ducklake_metadata_manager.cpp` | `GetOrphanFilesForCleanup`, after the known files are read | `AddKnownFiles(...)` | branch files are not orphans |
| `src/functions/ducklake_expire_snapshots.cpp` | bind, after the "never the latest snapshot" filter | `filter += ExpirableSnapshotFilter(...)` | open branches pin their fork and every later snapshot |
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
| `include/storage/ducklake_table_entry.hpp` | `friend class DuckLakeBranchManager;` | a column a branch adds keeps the id its files were written with: the replay sets the table's `next_column_id`, and a branch table stores the real counter |
| `include/common/ducklake_data_file.hpp` | `DuckLakeDeleteFile::created_by_ducklake` | marks delete files of earlier branch commits, as `DuckLakeDataFile::created_by_ducklake` already marks data files |
| `include/storage/ducklake_transaction_state.hpp` | `DuckLakeCommitContext::pre_commit_check`, `extra_commit_sql`; the defaulted `pre_commit_check` parameter of `CheckForConflicts` | generic commit-loop extension points the merge and the close of a stale branch use |

## Private members branching code depends on

These are reached through the `DuckLakeBranchManager` friend declarations. A rebase can apply cleanly and still fail to
compile in `src/branching/` when upstream changes them; that is where to fix it.

- `DuckLakeTransaction`: `state`, `snapshot`, `snapshot_lock`, `connection`, `new_name_maps`, `branch_state`,
  `catalog_version`, `GetTransactionChanges()`
- `LocalTableChanges`: `lock`, `changes`
- `DuckLakeDelete`: `TryDropFullyDeletedFile`
- `DuckLakeTableEntry`: `next_column_id`
- `DuckLakeTransactionState` (public): `local_changes`, `dropped_files`, `dropped_file_stats`, `tables_deleted_from`,
  `tables_delete_attempted`, `flushed_inlined_tables`, `CheckForConflicts`, `CleanupFiles`, and the catalog changes
  `new_schemas`, `new_tables`, `dropped_tables`, `dropped_views`, `dropped_schemas`, `renamed_tables`, `renamed_views`,
  `new_scalar_macros`, `new_table_macros`, `dropped_scalar_macros`, `dropped_table_macros`

## Upstream behaviour the catalog changes rely on

`src/branching/ducklake_branch_ddl.cpp` rebuilds a branch's schemas, tables and views as transaction-local entries,
the way `DuckLakeCatalog::LoadSchemaForSnapshot` and `CreateSchema` / `CreateTableExtended` / `CreateView` build them,
and applies renames and comments through DuckLake's own `Alter` functions. A merge then commits them like any
transaction that created them. After a rebase, check:

- `ColumnFieldId` mirrors `TransformColumnType` in `ducklake_catalog.cpp`, which is file-static. Column rows are stored
  in `ducklake_column`'s vocabulary (`DuckLakeTableEntry::GetTableColumns`); a change to either side must reach both.
- The constructors of `DuckLakeSchemaEntry`, `DuckLakeTableEntry` and `DuckLakeViewEntry`, `DuckLakeTableEntry::Alter`,
  `DuckLakeViewEntry::AlterEntry`, `DuckLakeCatalog::GetSchemaForSnapshot` and `GetNewUncommittedCatalogVersion`.
- `DuckLakeCatalogSet` indexes transaction-local schemas by id but not tables; `GetTableEntry` searches for them.
- A rename on main writes a new `ducklake_table` / `ducklake_view` row and is published as a created table or view;
  the merge's rename checks (`RenamedOnMain`) depend on both.

## Upstream behaviour column changes rely on

A branch's ADD COLUMN and DROP COLUMN are stored in `ducklake_branching_column_change` and replayed through DuckLake's own
`Alter`, so a merge commits them as a direct ALTER would. After a rebase, check:

- `DuckLakeTableEntry::AlterTable(AddColumnInfo)` takes the new column's id from `next_column_id` when it is set
  (`RequireNextColumnId`), and `DuckLakeFieldData::AddColumn` numbers nested fields depth first after it.
  `AddColumnWithIds` sets the id on a copy of the table and checks the ids it gets.
- Main hands out `MAX(column_id) + 1` over all of a table's `ducklake_column` rows (`GetNextColumnId`), so a dropped id
  is never reused once it has a row. A table created and changed in one transaction writes its created columns and then
  ends a dropped one, which is how a branch table keeps its dropped ids at merge. A column added and dropped again in
  one transaction gets no row.
- Scans match file columns by field id (`DuckLakeMultiFileReader`): a column missing in a file reads as its
  `initial_default`, a dropped one is ignored.
- DuckLake's conflict check fails an alter against another alter or drop of the table, and lets it pass after
  concurrent inserts and deletes. `CheckColumnChanges` reports the first case by the table's name, before the commit.

## Upstream behaviour the row-by-row merge relies on

`src/branching/ducklake_branch_row_merge.cpp` compares the rows of a table both sides changed since the fork, by row id,
reading them through SQL on internal connections (`ducklake_branch_merge_rows.cpp`). It adds no call site. After a
rebase, check:

- DuckLake tables expose the virtual columns `rowid`, `filename`, `file_row_number` and `snapshot_id`
  (`DuckLakeTableEntry::GetVirtualColumns`). A branch's rows have no `snapshot_id`; an inlined row's `filename` is its
  inlined data table; an UPDATE keeps the row id.
- `ducklake_table_deletions(catalog, schema, table, start, end)` lists every row main deleted, the old copies of updated
  rows included.
- `DuckLakeTransactionState::Commit` uses the `TransactionChangeInformation` it is given for the conflict checks and the
  catalog writes only; `WriteSnapshotChanges` rebuilds the insert and delete sets from the state. A merge takes the
  tables it merges row by row out of those sets (`ExcludeFromInsertDeleteRules`).
- Expiry, compaction and flushing never touch what an open branch reads at its fork, so main's compaction or flush
  cannot meet a branch's deletes on those rows; `ducklake_rewrite_data_files` can, and keeps the table-level rule.

## Upstream behaviour closing stale branches relies on

`src/branching/ducklake_branch_close.cpp` archives a stale branch with `CREATE TABLE ... AS SELECT` from
`ducklake_branch_table` on an internal connection, and closes the branch in the commit that creates the archive tables.
It adds no call site. After a rebase, check:

- A main transaction with catalog changes commits through `RunCommitLoop`, which `CommitClose` calls directly, as
  `CommitMerge` does, so the close never takes `FlushChangesServerSide`.
- `DuckLakeTransaction::GetConnection` creates the metadata connection on first use. Before closing, the caller's
  connection is dropped when the caller made no changes: a SQLite metadata catalog takes no write while another
  transaction reads it.
- `ducklake_files_scheduled_for_deletion` accepts branch file ids, and `ducklake_cleanup_old_files` removes those files,
  as for `DROP BRANCH`.

## After a rebase

1. `make release`. Compile errors in `src/branching/` mean upstream changed one of the members above.
2. `build/release/test/unittest "test/sql/branch/*"`, also with `--test-config test/configs/{no_inline,deletion_vectors,sqlite,ducklake_version}.json`.
3. The regression directories: `transaction delete update data_inlining deletion_inlining compaction cleanup remove_orphans time_travel table_changes stats snapshot_info checkpoint partitioning catalog alter comments schema_evolution`.
4. `scripts/branching/coverage.sh` regenerates [COVERAGE.md](COVERAGE.md) and fails if a function in `src/branching/` is
   never executed or an uncovered line has no reason.
5. Check for a new upstream file-removal site in `LocalTableChanges` that is not routed through `RemoveDeleteFile` /
   `TryRemoveDeleteFile`: it would delete a branch's files on rollback.
