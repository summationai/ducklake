#include "branching/ducklake_branch_functions.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/client_context.hpp"
#include "branching/ducklake_branch.hpp"
#include "storage/ducklake_catalog.hpp"
#include "storage/ducklake_transaction.hpp"

namespace duckdb {

static DuckLakeBranchState &GetBranchState(ClientContext &context) {
	return *context.registered_state->GetOrCreate<DuckLakeBranchState>(DuckLakeBranchState::KEY);
}

static DuckLakeCatalog &GetBranchCatalog(ClientContext &context, const Value &catalog_name) {
	auto &catalog = DuckLakeBaseMetadataFunction::GetCatalog(context, catalog_name).Cast<DuckLakeCatalog>();
	if (catalog.CatalogSnapshot()) {
		throw InvalidInputException("Branches cannot be used on a DuckLake attached at a fixed snapshot");
	}
	return catalog;
}

static void EnsureWritable(DuckLakeCatalog &catalog, const string &statement) {
	if (catalog.GetAttached().IsReadOnly()) {
		throw InvalidInputException("%s requires DuckLake \"%s\" to be attached read-write", statement,
		                            catalog.GetName().GetIdentifierName());
	}
}

struct DuckLakeBranchFunctionData : public TableFunctionData {
	DuckLakeBranchFunctionData(DuckLakeCatalog &catalog, string branch_name)
	    : catalog(catalog), branch_name(std::move(branch_name)) {
	}

	DuckLakeCatalog &catalog;
	string branch_name;
	string source_branch;
	bool if_exists = false;
	//! Author and message given to MERGE BRANCH
	unique_ptr<DuckLakeSnapshotCommit> commit_info;
	//! Report what the merge would do instead of merging
	bool dry_run = false;
};

struct DuckLakeBranchFunctionState : public GlobalTableFunctionState {
	bool finished = false;
};

static unique_ptr<GlobalTableFunctionState> DuckLakeBranchFunctionInit(ClientContext &context,
                                                                       TableFunctionInitInput &input) {
	return make_uniq<DuckLakeBranchFunctionState>();
}

static unique_ptr<DuckLakeBranchFunctionData> BindBranchFunction(ClientContext &context,
                                                                 TableFunctionBindInput &input) {
	auto &catalog = GetBranchCatalog(context, input.inputs[0]);
	if (input.inputs[1].IsNull()) {
		throw InvalidInputException("Branch name cannot be NULL");
	}
	return make_uniq<DuckLakeBranchFunctionData>(catalog, input.inputs[1].GetValue<string>());
}

//===--------------------------------------------------------------------===//
// ducklake_create_branch
//===--------------------------------------------------------------------===//
static unique_ptr<FunctionData> CreateBranchBind(ClientContext &context, TableFunctionBindInput &input,
                                                 vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto result = BindBranchFunction(context, input);
	auto source = input.named_parameters.find("from_branch");
	if (source != input.named_parameters.end() && !source->second.IsNull()) {
		result->source_branch = source->second.GetValue<string>();
	}
	names.emplace_back("branch_name");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("fork_snapshot_id");
	return_types.emplace_back(LogicalType::UBIGINT);
	return std::move(result);
}

static void CreateBranchExecute(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &state = data_p.global_state->Cast<DuckLakeBranchFunctionState>();
	auto &data = data_p.bind_data->Cast<DuckLakeBranchFunctionData>();
	if (state.finished) {
		return;
	}
	DuckLakeBranchManager::EnsureAutoCommit(context, "CREATE BRANCH");
	EnsureWritable(data.catalog, "CREATE BRANCH");
	if (!data.source_branch.empty() &&
	    !StringUtil::CIEquals(data.source_branch, DuckLakeBranchManager::MAIN_BRANCH_NAME)) {
		throw NotImplementedException("Creating a branch from another branch is not supported yet");
	}
	auto &transaction = DuckLakeTransaction::Get(context, data.catalog);
	auto branch = DuckLakeBranchManager::CreateBranch(transaction, data.branch_name);
	output.data[0].Append(Value(branch.name));
	output.data[1].Append(Value::UBIGINT(branch.fork_snapshot_id));
	output.SetChildCardinality(1);
	state.finished = true;
}

TableFunction DuckLakeBranchFunctions::GetCreateBranchFunction() {
	TableFunction function("ducklake_create_branch", {LogicalType::VARCHAR, LogicalType::VARCHAR}, CreateBranchExecute,
	                       CreateBranchBind, DuckLakeBranchFunctionInit);
	function.named_parameters["from_branch"] = LogicalType::VARCHAR;
	return function;
}

//===--------------------------------------------------------------------===//
// ducklake_drop_branch
//===--------------------------------------------------------------------===//
static unique_ptr<FunctionData> DropBranchBind(ClientContext &context, TableFunctionBindInput &input,
                                               vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto result = BindBranchFunction(context, input);
	auto if_exists = input.named_parameters.find("if_exists");
	if (if_exists != input.named_parameters.end() && !if_exists->second.IsNull()) {
		result->if_exists = if_exists->second.GetValue<bool>();
	}
	if (input.inputs.size() > 2 && !input.inputs[2].IsNull()) {
		// DROP BRANCH IF EXISTS passes the flag positionally
		result->if_exists = input.inputs[2].GetValue<bool>();
	}
	names.emplace_back("branch_name");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("dropped");
	return_types.emplace_back(LogicalType::BOOLEAN);
	return std::move(result);
}

static void DropBranchExecute(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &state = data_p.global_state->Cast<DuckLakeBranchFunctionState>();
	auto &data = data_p.bind_data->Cast<DuckLakeBranchFunctionData>();
	if (state.finished) {
		return;
	}
	DuckLakeBranchManager::EnsureAutoCommit(context, "DROP BRANCH");
	EnsureWritable(data.catalog, "DROP BRANCH");
	DuckLakeBranchManager::ValidateBranchName(data.branch_name);
	auto &transaction = DuckLakeTransaction::Get(context, data.catalog);
	auto branch = DuckLakeBranchManager::GetActiveBranch(transaction, data.branch_name);
	if (!branch && !data.if_exists) {
		throw InvalidInputException("Branch \"%s\" does not exist", data.branch_name);
	}
	if (branch) {
		DuckLakeBranchManager::DropBranch(transaction, *branch);
		GetBranchState(context).ClearIfSelected(data.catalog.GetOid(), branch->branch_id);
	}
	output.data[0].Append(Value(data.branch_name));
	output.data[1].Append(Value::BOOLEAN(branch != nullptr));
	output.SetChildCardinality(1);
	state.finished = true;
}

TableFunction DuckLakeBranchFunctions::GetDropBranchFunction() {
	TableFunction function("ducklake_drop_branch", {LogicalType::VARCHAR, LogicalType::VARCHAR}, DropBranchExecute,
	                       DropBranchBind, DuckLakeBranchFunctionInit);
	function.named_parameters["if_exists"] = LogicalType::BOOLEAN;
	return function;
}

//===--------------------------------------------------------------------===//
// ducklake_set_branch
//===--------------------------------------------------------------------===//
static unique_ptr<FunctionData> SetBranchBind(ClientContext &context, TableFunctionBindInput &input,
                                              vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto result = BindBranchFunction(context, input);
	names.emplace_back("branch_name");
	return_types.emplace_back(LogicalType::VARCHAR);
	return std::move(result);
}

static void SetBranchExecute(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &state = data_p.global_state->Cast<DuckLakeBranchFunctionState>();
	auto &data = data_p.bind_data->Cast<DuckLakeBranchFunctionData>();
	if (state.finished) {
		return;
	}
	DuckLakeBranchManager::EnsureAutoCommit(context, "SET BRANCH");
	auto &branch_state = GetBranchState(context);
	string branch_name = DuckLakeBranchManager::MAIN_BRANCH_NAME;
	if (StringUtil::CIEquals(data.branch_name, DuckLakeBranchManager::MAIN_BRANCH_NAME)) {
		branch_state.Clear(data.catalog.GetOid());
	} else {
		auto &transaction = DuckLakeTransaction::Get(context, data.catalog);
		auto branch = DuckLakeBranchManager::GetActiveBranch(transaction, data.branch_name);
		if (!branch) {
			throw InvalidInputException("Branch \"%s\" does not exist", data.branch_name);
		}
		branch_state.Select(data.catalog.GetOid(), branch->branch_id, branch->name);
		branch_name = branch->name;
	}
	output.data[0].Append(Value(branch_name));
	output.SetChildCardinality(1);
	state.finished = true;
}

TableFunction DuckLakeBranchFunctions::GetSetBranchFunction() {
	return TableFunction("ducklake_set_branch", {LogicalType::VARCHAR, LogicalType::VARCHAR}, SetBranchExecute,
	                     SetBranchBind, DuckLakeBranchFunctionInit);
}

//===--------------------------------------------------------------------===//
// ducklake_merge_branch
//===--------------------------------------------------------------------===//
struct DuckLakeMergeBranchState : public GlobalTableFunctionState {
	bool started = false;
	//! dry run: the tables still to report
	vector<DuckLakeMergePreviewEntry> entries;
	idx_t offset = 0;
};

static unique_ptr<GlobalTableFunctionState> MergeBranchInit(ClientContext &context, TableFunctionInitInput &input) {
	return make_uniq<DuckLakeMergeBranchState>();
}

static unique_ptr<FunctionData> MergeBranchBind(ClientContext &context, TableFunctionBindInput &input,
                                                vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto result = BindBranchFunction(context, input);
	auto dry_run = input.named_parameters.find("dry_run");
	if (dry_run != input.named_parameters.end() && !dry_run->second.IsNull()) {
		result->dry_run = dry_run->second.GetValue<bool>();
	}
	auto author = input.named_parameters.find("author");
	auto message = input.named_parameters.find("message");
	if (author != input.named_parameters.end() || message != input.named_parameters.end()) {
		result->commit_info = make_uniq<DuckLakeSnapshotCommit>();
		if (author != input.named_parameters.end()) {
			result->commit_info->author = author->second;
		}
		if (message != input.named_parameters.end()) {
			result->commit_info->commit_message = message->second;
			result->commit_info->is_commit_info_set = true;
		}
	}
	if (result->dry_run) {
		// what the merge would do, table by table
		names.emplace_back("schema_name");
		return_types.emplace_back(LogicalType::VARCHAR);
		names.emplace_back("table_name");
		return_types.emplace_back(LogicalType::VARCHAR);
		names.emplace_back("status");
		return_types.emplace_back(LogicalType::VARCHAR);
		names.emplace_back("conflict");
		return_types.emplace_back(LogicalType::VARCHAR);
		names.emplace_back("rows_inserted");
		return_types.emplace_back(LogicalType::BIGINT);
		names.emplace_back("rows_deleted");
		return_types.emplace_back(LogicalType::BIGINT);
		names.emplace_back("files_added");
		return_types.emplace_back(LogicalType::BIGINT);
		names.emplace_back("branch_changes");
		return_types.emplace_back(LogicalType::LIST(LogicalType::VARCHAR));
		names.emplace_back("main_changes_since_fork");
		return_types.emplace_back(LogicalType::LIST(LogicalType::VARCHAR));
		return std::move(result);
	}
	names.emplace_back("branch_name");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("fork_snapshot_id");
	return_types.emplace_back(LogicalType::UBIGINT);
	names.emplace_back("branch_commits");
	return_types.emplace_back(LogicalType::UBIGINT);
	return std::move(result);
}

static Value ChangeList(const vector<string> &changes) {
	vector<Value> values;
	for (auto &change : changes) {
		values.emplace_back(change);
	}
	return Value::LIST(LogicalType::VARCHAR, std::move(values));
}

static void MergeBranchDryRun(ClientContext &context, DuckLakeMergeBranchState &state,
                              const DuckLakeBranchFunctionData &data, DataChunk &output) {
	if (!state.started) {
		auto &transaction = DuckLakeTransaction::Get(context, data.catalog);
		state.entries = DuckLakeBranchManager::PreviewMerge(transaction, data.branch_name);
		state.started = true;
	}
	idx_t count = 0;
	while (state.offset < state.entries.size() && count < STANDARD_VECTOR_SIZE) {
		auto &entry = state.entries[state.offset++];
		output.data[0].SetValue(count, Value(entry.schema_name));
		output.data[1].SetValue(count, Value(entry.table_name));
		output.data[2].SetValue(count, Value(entry.conflict.empty() ? "ok" : "conflict"));
		output.data[3].SetValue(count, entry.conflict.empty() ? Value(LogicalType::VARCHAR) : Value(entry.conflict));
		output.data[4].SetValue(count, Value::BIGINT(NumericCast<int64_t>(entry.rows_inserted)));
		output.data[5].SetValue(count, Value::BIGINT(NumericCast<int64_t>(entry.rows_deleted)));
		output.data[6].SetValue(count, Value::BIGINT(NumericCast<int64_t>(entry.files_added)));
		output.data[7].SetValue(count, ChangeList(entry.branch_changes));
		output.data[8].SetValue(count, ChangeList(entry.main_changes));
		count++;
	}
	output.SetChildCardinality(count);
}

static void MergeBranchExecute(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &state = data_p.global_state->Cast<DuckLakeMergeBranchState>();
	auto &data = data_p.bind_data->Cast<DuckLakeBranchFunctionData>();
	if (data.dry_run) {
		MergeBranchDryRun(context, state, data, output);
		return;
	}
	if (state.started) {
		return;
	}
	DuckLakeBranchManager::EnsureAutoCommit(context, "MERGE BRANCH");
	EnsureWritable(data.catalog, "MERGE BRANCH");
	auto &transaction = DuckLakeTransaction::Get(context, data.catalog);
	// the statement's own commit performs the merge
	auto branch = DuckLakeBranchManager::PrepareMerge(transaction, data.branch_name, data.commit_info.get());
	output.data[0].Append(Value(branch.name));
	output.data[1].Append(Value::UBIGINT(branch.fork_snapshot_id));
	output.data[2].Append(Value::UBIGINT(branch.head_seq));
	output.SetChildCardinality(1);
	state.started = true;
}

TableFunction DuckLakeBranchFunctions::GetMergeBranchFunction() {
	TableFunction function("ducklake_merge_branch", {LogicalType::VARCHAR, LogicalType::VARCHAR}, MergeBranchExecute,
	                       MergeBranchBind, MergeBranchInit);
	function.named_parameters["author"] = LogicalType::VARCHAR;
	function.named_parameters["message"] = LogicalType::VARCHAR;
	function.named_parameters["dry_run"] = LogicalType::BOOLEAN;
	return function;
}

//===--------------------------------------------------------------------===//
// ducklake_current_branch / ducklake_branches
//===--------------------------------------------------------------------===//
static unique_ptr<FunctionData> CurrentBranchBind(ClientContext &context, TableFunctionBindInput &input,
                                                  vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto &catalog = GetBranchCatalog(context, input.inputs[0]);
	names.emplace_back("branch_name");
	return_types.emplace_back(LogicalType::VARCHAR);
	string branch_name = DuckLakeBranchManager::MAIN_BRANCH_NAME;
	DuckLakeBranchState::Selection selection;
	if (GetBranchState(context).TryGetSelection(catalog.GetOid(), selection)) {
		branch_name = selection.name;
	}
	auto result = make_uniq<MetadataBindData>();
	result->rows.push_back({Value(branch_name)});
	return std::move(result);
}

static unique_ptr<FunctionData> BranchesBind(ClientContext &context, TableFunctionBindInput &input,
                                             vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto &catalog = GetBranchCatalog(context, input.inputs[0]);
	names.emplace_back("branch_id");
	return_types.emplace_back(LogicalType::UBIGINT);
	names.emplace_back("branch_name");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("fork_snapshot_id");
	return_types.emplace_back(LogicalType::UBIGINT);
	names.emplace_back("commit_count");
	return_types.emplace_back(LogicalType::UBIGINT);
	names.emplace_back("created_at");
	return_types.emplace_back(LogicalType::TIMESTAMP_TZ);

	auto result = make_uniq<MetadataBindData>();
	auto &transaction = DuckLakeTransaction::Get(context, catalog);
	for (auto &branch : DuckLakeBranchManager::GetBranches(transaction)) {
		vector<Value> row;
		row.push_back(Value::UBIGINT(branch.branch_id));
		row.push_back(Value(branch.name));
		row.push_back(Value::UBIGINT(branch.fork_snapshot_id));
		row.push_back(Value::UBIGINT(branch.head_seq));
		row.push_back(branch.created_at.DefaultCastAs(LogicalType::TIMESTAMP_TZ));
		result->rows.push_back(std::move(row));
	}
	return std::move(result);
}

DuckLakeCurrentBranchFunction::DuckLakeCurrentBranchFunction()
    : DuckLakeBaseMetadataFunction("ducklake_current_branch", CurrentBranchBind) {
}

DuckLakeBranchesFunction::DuckLakeBranchesFunction() : DuckLakeBaseMetadataFunction("ducklake_branches", BranchesBind) {
}

} // namespace duckdb
