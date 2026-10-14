#include "branching/ducklake_branch.hpp"

#include "branching/ducklake_branch_merge_rows.hpp"
#include "branching/ducklake_branch_util.hpp"
#include "common/ducklake_util.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/connection.hpp"
#include "storage/ducklake_catalog.hpp"
#include "storage/ducklake_metadata_manager.hpp"
#include "storage/ducklake_transaction.hpp"
#include "storage/ducklake_transaction_changes.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// Finding stale branches
//===--------------------------------------------------------------------===//
vector<DuckLakeBranchInfo> DuckLakeBranchManager::GetBranchesWithActivity(DuckLakeTransaction &transaction) {
	auto branches = GetBranches(transaction);
	if (branches.empty()) {
		return branches;
	}
	vector<idx_t> ids;
	for (auto &branch : branches) {
		ids.push_back(branch.branch_id);
	}
	auto result =
	    RunBranchQuery(transaction,
	                   "SELECT branch_id, MAX(commit_time) FROM {METADATA_CATALOG}.ducklake_branching_commit WHERE "
	                   "branch_id IN (" +
	                       BranchIdList(ids) + ") GROUP BY branch_id",
	                   "Failed to read DuckLake branch commits: ");
	unordered_map<idx_t, Value> last_commits;
	for (auto &row : *result) {
		last_commits[row.GetValue<idx_t>(0)] = row.GetBaseValue(1);
	}
	for (auto &branch : branches) {
		auto last_commit = last_commits.find(branch.branch_id);
		auto &last_activity =
		    last_commit == last_commits.end() || last_commit->second.IsNull() ? branch.created_at : last_commit->second;
		branch.last_activity = last_activity.DefaultCastAs(LogicalType::TIMESTAMP_TZ);
	}
	return branches;
}

//===--------------------------------------------------------------------===//
// Archiving a branch into main
//===--------------------------------------------------------------------===//
namespace {

string SqlLiteral(const string &value) {
	return DuckLakeUtil::SQLLiteralToString(value);
}

string JsonString(const string &value) {
	return "\"" + StringUtil::Replace(StringUtil::Replace(value, "\\", "\\\\"), "\"", "\\\"") + "\"";
}

//! <branch>__<table>__<date>, with the schema before the table when it is not main
string ArchiveBaseName(const string &branch_name, const string &schema_name, const string &table_name,
                       date_t close_date) {
	int32_t year, month, day;
	Date::Convert(close_date, year, month, day);
	auto schema_part = schema_name == DEFAULT_SCHEMA ? string() : schema_name + "__";
	return StringUtil::Format("%s__%s%s__%04d%02d%02d", branch_name, schema_part, table_name, year, month, day);
}

//! The base name, or with _2, _3, ... appended while main or this call already has the name; runs in a transaction
string UniqueArchiveName(ClientContext &context, const string &catalog_name, const string &schema_name,
                         const string &base_name, case_insensitive_set_t &reserved_names) {
	for (idx_t attempt = 1;; attempt++) {
		auto name = attempt == 1 ? base_name : base_name + "_" + to_string(attempt);
		if (reserved_names.find(name) != reserved_names.end()) {
			continue;
		}
		QualifiedName qualified {Identifier(catalog_name), Identifier(schema_name), Identifier(name)};
		EntryLookupInfo lookup(CatalogType::TABLE_ENTRY, qualified);
		if (Catalog::GetEntry(context, lookup, OnEntryNotFound::RETURN_NULL)) {
			continue;
		}
		reserved_names.insert(name);
		return name;
	}
}

struct ArchiveSource {
	string schema_name;
	//! The table's name on the branch
	string table_name;
};

//! The tables the branch changed or created and still has, as a merge dry run lists them
vector<ArchiveSource> ArchiveSources(DuckLakeTransaction &transaction, const DuckLakeBranchInfo &info) {
	auto current = DuckLakeBranchManager::GetBranch(transaction, info.branch_id);
	if (!current || !current->IsActive() || current->head_seq != info.head_seq) {
		throw TransactionException("Branch \"%s\" was committed to, merged or dropped since it was found stale",
		                           info.name);
	}
	vector<ArchiveSource> sources;
	for (auto &entry : DuckLakeBranchManager::PreviewMerge(transaction, info.name)) {
		auto &changes = entry.branch_changes;
		if (entry.object_type != "table" || std::find(changes.begin(), changes.end(), "dropped") != changes.end()) {
			continue;
		}
		sources.push_back({entry.schema_name, entry.new_name.empty() ? entry.table_name : entry.new_name});
	}
	return sources;
}

void ArchiveAndClose(Connection &connection, DuckLakeCatalog &catalog, const string &archive_schema, date_t close_date,
                     bool dry_run, case_insensitive_set_t &reserved_names, DuckLakeStaleBranch &branch) {
	auto &context = *connection.context;
	auto catalog_name = catalog.GetName().GetIdentifierName();
	auto &info = branch.info;
	vector<ArchiveSource> sources;
	auto archive_name = [&](const ArchiveSource &source) {
		auto base_name = ArchiveBaseName(info.name, source.schema_name, source.table_name, close_date);
		return UniqueArchiveName(context, catalog_name, archive_schema, base_name, reserved_names);
	};
	context.RunFunctionInTransaction([&]() {
		auto &transaction = DuckLakeTransaction::Get(context, catalog);
		sources = ArchiveSources(transaction, info);
		if (dry_run) {
			for (auto &source : sources) {
				branch.archived_tables.push_back(archive_schema + "." + archive_name(source));
			}
		}
	});
	if (dry_run) {
		branch.status = "would close";
		return;
	}
	// the archive tables and the close commit together, or not at all
	RunBranchSql(connection, "BEGIN TRANSACTION");
	if (!sources.empty()) {
		RunBranchSql(connection, "CREATE SCHEMA IF NOT EXISTS " + DuckLakeUtil::SQLIdentifierToString(catalog_name) +
		                             "." + DuckLakeUtil::SQLIdentifierToString(archive_schema));
	}
	auto comment =
	    StringUtil::Format("Archived from branch \"%s\" (last activity %s, fork snapshot %d) on %s", info.name,
	                       info.last_activity.ToString(), info.fork_snapshot_id, Date::ToString(close_date));
	for (auto &source : sources) {
		string name;
		context.RunFunctionInTransaction([&]() { name = archive_name(source); });
		auto target = BranchTableSql(catalog_name, archive_schema, name);
		RunBranchSql(connection, StringUtil::Format(
		                             "CREATE TABLE %s AS SELECT * EXCLUDE (rowid) FROM ducklake_branch_table(%s, %s, "
		                             "%s, schema => %s)",
		                             target, SqlLiteral(catalog_name), SqlLiteral(info.name),
		                             SqlLiteral(source.table_name), SqlLiteral(source.schema_name)));
		RunBranchSql(connection, StringUtil::Format("COMMENT ON TABLE %s IS %s", target, SqlLiteral(comment)));
		branch.archived_tables.push_back(archive_schema + "." + name);
	}
	context.RunFunctionInTransaction([&]() {
		auto &transaction = DuckLakeTransaction::Get(context, catalog);
		auto close = make_uniq<DuckLakeBranchClose>();
		close->info = info;
		close->archived_tables = branch.archived_tables;
		close->has_column_change_table = DuckLakeBranchManager::HasColumnChangeTable(transaction);
		string archived;
		for (auto &table : branch.archived_tables) {
			archived += (archived.empty() ? "" : ", ") + JsonString(table);
		}
		auto &commit_info = transaction.GetCommitInfo();
		commit_info.commit_message = Value("Close branch " + info.name);
		commit_info.commit_extra_info = Value(StringUtil::Format(
		    R"({"branch": %s, "fork_snapshot_id": %d, "branch_commits": %d, "archived_tables": [%s]})",
		    JsonString(info.name), info.fork_snapshot_id, info.head_seq, archived));
		DuckLakeBranchManager::SetBranchClose(transaction, std::move(close));
	});
	RunBranchSql(connection, "COMMIT");
	branch.status = "closed";
}

} // namespace

vector<DuckLakeStaleBranch> DuckLakeBranchManager::CloseStaleBranches(ClientContext &context, DuckLakeCatalog &catalog,
                                                                      timestamp_tz_t older_than,
                                                                      const string &archive_schema, bool dry_run) {
	// each branch is archived and closed in a main transaction of its own, on a connection of its own
	Connection connection(*context.db);
	DuckLakeUtil::CopyExtensionSettings(context, *connection.context);
	vector<DuckLakeStaleBranch> result;
	connection.context->RunFunctionInTransaction([&]() {
		auto &transaction = DuckLakeTransaction::Get(*connection.context, catalog);
		for (auto &branch : GetBranchesWithActivity(transaction)) {
			if (branch.last_activity.IsNull() || branch.last_activity.GetValue<timestamp_tz_t>() >= older_than) {
				continue;
			}
			DuckLakeStaleBranch stale;
			stale.info = std::move(branch);
			result.push_back(std::move(stale));
		}
	});
	auto &caller = DuckLakeTransaction::Get(context, catalog);
	if (!dry_run && !caller.ChangesMade()) {
		// a SQLite metadata catalog takes no write while another transaction reads it: the caller's read ends here
		lock_guard<mutex> guard(caller.connection_lock);
		caller.connection.reset();
	}
	auto close_date = Timestamp::GetDate(Timestamp::GetCurrentTimestamp());
	case_insensitive_set_t reserved_names;
	for (auto &branch : result) {
		try {
			ArchiveAndClose(connection, catalog, archive_schema, close_date, dry_run, reserved_names, branch);
		} catch (std::exception &ex) {
			ErrorData error(ex);
			if (error.Type() == ExceptionType::INTERNAL || Exception::InvalidatesDatabase(error.Type())) {
				throw;
			}
			// the branch stays open, and nothing of its archive is kept
			if (connection.HasActiveTransaction()) {
				connection.Rollback();
			}
			branch.status = "skipped";
			branch.archived_tables.clear();
			branch.message = error.RawMessage();
		}
	}
	return result;
}

//===--------------------------------------------------------------------===//
// Committing a close
//===--------------------------------------------------------------------===//
bool DuckLakeBranchManager::IsClosingBranch(DuckLakeTransaction &transaction) {
	auto state = GetState(transaction);
	return state && state->close;
}

void DuckLakeBranchManager::SetBranchClose(DuckLakeTransaction &transaction, unique_ptr<DuckLakeBranchClose> close) {
	GetOrCreateState(transaction).close = std::move(close);
}

void DuckLakeBranchManager::CheckClose(DuckLakeTransaction &transaction, const DuckLakeBranchClose &close) {
	auto &info = close.info;
	auto branch = GetBranch(transaction, info.branch_id);
	if (!branch || !branch->IsActive() || branch->head_seq != info.head_seq) {
		throw TransactionException("Branch \"%s\" was committed to, merged or dropped while it was being closed",
		                           info.name);
	}
}

string DuckLakeBranchManager::CloseBookkeepingSql(DuckLakeTransaction &transaction, const DuckLakeBranchClose &close,
                                                  bool with_snapshot) {
	auto &info = close.info;
	auto &commit_info = transaction.GetCommitInfo();
	auto id = info.branch_id;
	auto closed_seq = info.head_seq + 1;
	string sql;
	// as for a merge: the update only applies while the branch is where it was found, and the insert after it fails
	// the batch when it did not
	sql +=
	    StringUtil::Format("UPDATE {METADATA_CATALOG}.ducklake_branching_branch SET status = 'closed', head_seq = %d "
	                       "WHERE branch_id = %d AND head_seq = %d AND status = 'active';\n",
	                       closed_seq, id, info.head_seq);
	sql += StringUtil::Format(
	    "INSERT INTO {METADATA_CATALOG}.ducklake_branching_branch SELECT %d, %s, %d, %d, %d, 'close guard', NOW() "
	    "WHERE NOT EXISTS (SELECT 1 FROM {METADATA_CATALOG}.ducklake_branching_branch WHERE branch_id = %d AND "
	    "head_seq = %d AND status = 'closed');\n",
	    id, SqlLiteral(info.name), info.fork_snapshot_id, info.head_seq, info.next_file_seq, id, closed_seq);
	// the branch's history stays, ending with the close
	string changes_made = with_snapshot ? "closed:{SNAPSHOT_ID}" : "closed";
	sql +=
	    StringUtil::Format("INSERT INTO {METADATA_CATALOG}.ducklake_branching_commit VALUES (%d, %d, NOW(), %s, %s, "
	                       "%s, %s);\n",
	                       id, closed_seq, commit_info.author.ToSQLString(), commit_info.commit_message.ToSQLString(),
	                       commit_info.commit_extra_info.ToSQLString(), SqlLiteral(changes_made));
	sql += ScheduleBranchFilesSql(id);
	sql += DeleteBranchRowsSql(transaction, id, false, close.has_column_change_table);
	return sql;
}

void DuckLakeBranchManager::CommitClose(DuckLakeTransaction &transaction) {
	auto &close = *GetState(transaction)->close;
	if (transaction.ChangesMade()) {
		// the archive tables commit together with the close (PrepareCommitLoop)
		auto retry_config = DuckLakeRetryConfig::FromContext(*transaction.context.lock());
		transaction.RunCommitLoop(transaction.GetSnapshot(), transaction.GetTransactionChanges(), retry_config);
		return;
	}
	// nothing to archive - only record that the branch was closed
	auto &metadata_connection = transaction.GetConnection();
	try {
		CheckClose(transaction, close);
	} catch (...) {
		metadata_connection.Rollback();
		throw;
	}
	auto result = transaction.GetMetadataManager().Execute(CloseBookkeepingSql(transaction, close, false));
	if (result->HasError()) {
		metadata_connection.Rollback();
		result->GetErrorObject().Throw(StringUtil::Format("Failed to close branch \"%s\" - retry: ", close.info.name));
	}
	metadata_connection.Commit();
}

} // namespace duckdb
