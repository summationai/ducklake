#include "branching/ducklake_branch_parser.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database_manager.hpp"
#include "branching/ducklake_branch_functions.hpp"
#include "branching/ducklake_branch.hpp"

namespace duckdb {

enum class DuckLakeBranchStatementType : uint8_t { CREATE_BRANCH, DROP_BRANCH, SET_BRANCH, MERGE_BRANCH };

struct DuckLakeBranchParseData : public ParserExtensionParseData {
	DuckLakeBranchStatementType type = DuckLakeBranchStatementType::SET_BRANCH;
	string branch_name;
	string source_branch;
	bool if_exists = false;

	unique_ptr<ParserExtensionParseData> Copy() const override {
		auto result = make_uniq<DuckLakeBranchParseData>();
		result->type = type;
		result->branch_name = branch_name;
		result->source_branch = source_branch;
		result->if_exists = if_exists;
		return std::move(result);
	}

	static string Quote(const string &name) {
		return "\"" + StringUtil::Replace(name, "\"", "\"\"") + "\"";
	}

	string ToString() const override {
		auto name = Quote(branch_name);
		switch (type) {
		case DuckLakeBranchStatementType::CREATE_BRANCH:
			return "CREATE BRANCH " + name + (source_branch.empty() ? string() : " FROM " + Quote(source_branch));
		case DuckLakeBranchStatementType::DROP_BRANCH:
			return string("DROP BRANCH ") + (if_exists ? "IF EXISTS " : "") + name;
		case DuckLakeBranchStatementType::MERGE_BRANCH:
			return "MERGE BRANCH " + name;
		default:
			return "SET BRANCH " + name;
		}
	}
};

//! The tokens of one statement, without comments
class BranchTokenReader {
public:
	explicit BranchTokenReader(const vector<SimpleToken> &tokens) : tokens(tokens) {
		SkipComments();
	}

	bool AtEnd() const {
		if (position >= tokens.size()) {
			return true;
		}
		auto type = tokens[position].type;
		return type == TokenType::TERMINATOR || type == TokenType::END_OF_INPUT ||
		       type == TokenType::END_OF_INPUT_AUTOCOMPLETE;
	}

	bool TryConsumeWord(const char *word) {
		if (AtEnd() || !StringUtil::CIEquals(tokens[position].text, word)) {
			return false;
		}
		Advance();
		return true;
	}

	bool TryConsumeName(string &result) {
		if (AtEnd()) {
			return false;
		}
		auto &token = tokens[position];
		auto &text = token.text;
		if (token.type == TokenType::STRING_LITERAL && text.size() >= 2 && text.front() == '\'') {
			result = StringUtil::Replace(text.substr(1, text.size() - 2), "''", "'");
		} else if (text.size() >= 2 && text.front() == '"' && text.back() == '"') {
			result = StringUtil::Replace(text.substr(1, text.size() - 2), "\"\"", "\"");
		} else if (token.type == TokenType::OPERATOR || token.type == TokenType::NUMBER_LITERAL ||
		           token.type == TokenType::STRING_LITERAL) {
			return false;
		} else {
			result = text;
		}
		Advance();
		return true;
	}

	//! The number of tokens claimed, including the terminator
	int64_t ConsumedTokens() const {
		auto consumed = position;
		if (consumed < tokens.size()) {
			consumed++;
		}
		return NumericCast<int64_t>(consumed);
	}

private:
	void Advance() {
		position++;
		SkipComments();
	}
	void SkipComments() {
		while (position < tokens.size() && tokens[position].type == TokenType::COMMENT) {
			position++;
		}
	}

	const vector<SimpleToken> &tokens;
	idx_t position = 0;
};

static ParserExtensionParseResult BranchSyntaxError(const string &message) {
	ParserExtensionParseResult result(message);
	result.consumed_tokens = -1;
	return result;
}

static ParserExtensionParseResult DuckLakeBranchParse(ParserExtensionInfo *info, const vector<SimpleToken> &tokens) {
	BranchTokenReader reader(tokens);
	auto data = make_uniq<DuckLakeBranchParseData>();
	string usage;
	if (reader.TryConsumeWord("CREATE")) {
		if (!reader.TryConsumeWord("BRANCH")) {
			return ParserExtensionParseResult();
		}
		data->type = DuckLakeBranchStatementType::CREATE_BRANCH;
		usage = "CREATE BRANCH <name> [FROM main]";
		if (!reader.TryConsumeName(data->branch_name)) {
			return BranchSyntaxError("Expected a branch name - usage: " + usage);
		}
		if (reader.TryConsumeWord("FROM") && !reader.TryConsumeName(data->source_branch)) {
			return BranchSyntaxError("Expected a branch name after FROM - usage: " + usage);
		}
	} else if (reader.TryConsumeWord("DROP")) {
		if (!reader.TryConsumeWord("BRANCH")) {
			return ParserExtensionParseResult();
		}
		data->type = DuckLakeBranchStatementType::DROP_BRANCH;
		usage = "DROP BRANCH [IF EXISTS] <name>";
		if (reader.TryConsumeWord("IF")) {
			if (!reader.TryConsumeWord("EXISTS")) {
				return BranchSyntaxError("Expected EXISTS after IF - usage: " + usage);
			}
			data->if_exists = true;
		}
		if (!reader.TryConsumeName(data->branch_name)) {
			return BranchSyntaxError("Expected a branch name - usage: " + usage);
		}
	} else if (reader.TryConsumeWord("SET")) {
		if (!reader.TryConsumeWord("BRANCH")) {
			return ParserExtensionParseResult();
		}
		data->type = DuckLakeBranchStatementType::SET_BRANCH;
		usage = "SET BRANCH <name>";
		if (!reader.TryConsumeName(data->branch_name)) {
			return BranchSyntaxError("Expected a branch name - usage: " + usage);
		}
	} else if (reader.TryConsumeWord("MERGE")) {
		if (!reader.TryConsumeWord("BRANCH")) {
			return ParserExtensionParseResult();
		}
		data->type = DuckLakeBranchStatementType::MERGE_BRANCH;
		usage = "MERGE BRANCH <name>";
		if (!reader.TryConsumeName(data->branch_name)) {
			return BranchSyntaxError("Expected a branch name - usage: " + usage);
		}
	} else {
		return ParserExtensionParseResult();
	}
	if (!reader.AtEnd()) {
		return BranchSyntaxError("Unexpected input after the branch statement - usage: " + usage);
	}
	ParserExtensionParseResult result(std::move(data));
	result.consumed_tokens = reader.ConsumedTokens();
	return result;
}

static ParserExtensionPlanResult DuckLakeBranchPlan(ParserExtensionInfo *info, ClientContext &context,
                                                    unique_ptr<ParserExtensionParseData> parse_data) {
	auto &data = parse_data->Cast<DuckLakeBranchParseData>();
	auto catalog_name = DatabaseManager::GetDefaultDatabase(context).GetIdentifierName();

	ParserExtensionPlanResult result;
	result.parameters.push_back(Value(catalog_name));
	result.parameters.push_back(Value(data.branch_name));
	switch (data.type) {
	case DuckLakeBranchStatementType::CREATE_BRANCH:
		if (!data.source_branch.empty() &&
		    !StringUtil::CIEquals(data.source_branch, DuckLakeBranchManager::MAIN_BRANCH_NAME)) {
			throw NotImplementedException("Creating a branch from another branch is not supported yet");
		}
		result.function = DuckLakeBranchFunctions::GetCreateBranchFunction();
		break;
	case DuckLakeBranchStatementType::MERGE_BRANCH:
		result.function = DuckLakeBranchFunctions::GetMergeBranchFunction();
		break;
	case DuckLakeBranchStatementType::DROP_BRANCH:
		result.function = DuckLakeBranchFunctions::GetDropBranchFunction();
		result.function.arguments.push_back(LogicalType::BOOLEAN);
		result.parameters.push_back(Value::BOOLEAN(data.if_exists));
		break;
	default:
		result.function = DuckLakeBranchFunctions::GetSetBranchFunction();
		break;
	}
	result.requires_valid_transaction = true;
	result.return_type = StatementReturnType::QUERY_RESULT;
	return result;
}

DuckLakeBranchParserExtension::DuckLakeBranchParserExtension() {
	parse_function = DuckLakeBranchParse;
	plan_function = DuckLakeBranchPlan;
}

} // namespace duckdb
