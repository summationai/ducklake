#include "branching/ducklake_branch.hpp"

#include "branching/ducklake_branch_util.hpp"
#include "common/ducklake_types.hpp"
#include "common/ducklake_util.hpp"
#include "duckdb/common/identifier.hpp"
#include "duckdb/parser/constraints/not_null_constraint.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/parsed_data/alter_table_info.hpp"
#include "duckdb/parser/parsed_data/comment_on_column_info.hpp"
#include "duckdb/parser/parsed_data/create_schema_info.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/parser/parsed_data/create_view_info.hpp"
#include "duckdb/parser/parser.hpp"
#include "storage/ducklake_catalog.hpp"
#include "storage/ducklake_catalog_set.hpp"
#include "storage/ducklake_field_data.hpp"
#include "storage/ducklake_metadata_info.hpp"
#include "storage/ducklake_metadata_manager.hpp"
#include "storage/ducklake_schema_entry.hpp"
#include "storage/ducklake_table_entry.hpp"
#include "storage/ducklake_transaction.hpp"
#include "storage/ducklake_transaction_changes.hpp"
#include "storage/ducklake_transaction_state.hpp"
#include "storage/ducklake_view_entry.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// Definition tables
//===--------------------------------------------------------------------===//
// A branch stores the current definition of every object it created, and what it did to main's objects. Column rows
// use ducklake_column's vocabulary and keep the column ids, so a merge writes the ids the branch's files use. A commit
// replaces the rows of the objects it changed; every row carries the sequence number of the commit that wrote it.
static constexpr const char *DEFINITION_TABLES_SQL = R"(
CREATE TABLE IF NOT EXISTS {METADATA_CATALOG}.ducklake_branching_object(branch_id BIGINT, begin_seq BIGINT, object_id BIGINT, object_type VARCHAR, parent_id BIGINT, object_name VARCHAR, uuid VARCHAR, path VARCHAR, path_is_relative BOOLEAN, view_sql VARCHAR, view_aliases VARCHAR, comment VARCHAR, next_column_id BIGINT);
CREATE TABLE IF NOT EXISTS {METADATA_CATALOG}.ducklake_branching_column(branch_id BIGINT, begin_seq BIGINT, object_id BIGINT, column_id BIGINT, parent_column BIGINT, column_order BIGINT, column_name VARCHAR, column_type VARCHAR, initial_default VARCHAR, default_value VARCHAR, default_value_type VARCHAR, nulls_allowed BOOLEAN, comment VARCHAR);
CREATE TABLE IF NOT EXISTS {METADATA_CATALOG}.ducklake_branching_main_change(branch_id BIGINT, begin_seq BIGINT, object_type VARCHAR, main_id BIGINT, change_type VARCHAR, new_name VARCHAR);
)";

static constexpr const char *SCHEMA_OBJECT = "schema";
static constexpr const char *TABLE_OBJECT = "table";
static constexpr const char *VIEW_OBJECT = "view";

const char *DuckLakeBranchManager::DefinitionTablesSql() {
	return DEFINITION_TABLES_SQL;
}

bool DuckLakeBranchManager::HasDefinitionTables(DuckLakeTransaction &transaction) {
	if (IsDefinitionTablesCached(transaction)) {
		return true;
	}
	auto probe = transaction.Query("SELECT NULL FROM {METADATA_CATALOG}.ducklake_branching_object LIMIT 1");
	if (probe->HasError()) {
		if (probe->GetErrorObject().Type() == ExceptionType::CATALOG) {
			return false;
		}
		probe->GetErrorObject().Throw("Failed to probe DuckLake branch tables: ");
	}
	SetHasDefinitionTables(transaction, true);
	return true;
}

namespace {

string Literal(const string &value) {
	return DuckLakeUtil::SQLLiteralToString(value);
}

string ValueLiteral(const Value &value) {
	return value.IsNull() ? string("NULL") : Literal(value.ToString());
}

using ObjectIdAllocator = std::function<idx_t()>;

//! A table or view of main in the catalog of a snapshot. DuckLake's id lookup is meant for tables, so views are found
//! by scanning their schemas.
optional_ptr<CatalogEntry> MainEntryById(DuckLakeCatalogSet &snapshot_set, TableIndex id, CatalogType type) {
	if (type == CatalogType::TABLE_ENTRY) {
		auto entry = snapshot_set.GetEntryById(id);
		return entry && entry->type == CatalogType::TABLE_ENTRY ? entry : nullptr;
	}
	optional_ptr<CatalogEntry> result;
	for (auto &schema : snapshot_set.GetSchemaIdMap()) {
		schema.second.get().Scan(CatalogType::VIEW_ENTRY, [&](CatalogEntry &entry) {
			if (entry.type == CatalogType::VIEW_ENTRY && entry.Cast<DuckLakeViewEntry>().GetViewId() == id) {
				result = entry;
			}
		});
	}
	return result;
}

//! The branch object id of one of the transaction's schemas, tables or views; new objects get one when an allocator is
//! given
idx_t ObjectId(DuckLakeLoadedBranch &loaded, idx_t local_id, optional_ptr<const ObjectIdAllocator> allocate) {
	auto entry = loaded.object_ids.find(local_id);
	if (entry != loaded.object_ids.end()) {
		return entry->second;
	}
	if (!allocate) {
		throw InternalException("Branch object without a branch object id");
	}
	auto object_id = (*allocate)();
	loaded.object_ids[local_id] = object_id;
	loaded.local_ids[object_id] = local_id;
	return object_id;
}

idx_t StoredSchemaId(DuckLakeLoadedBranch &loaded, const DuckLakeSchemaEntry &schema,
                     optional_ptr<const ObjectIdAllocator> allocate) {
	auto schema_id = schema.GetSchemaId();
	return schema_id.IsTransactionLocal() ? ObjectId(loaded, schema_id.index, allocate) : schema_id.index;
}

//! One ducklake_branching_column row per field, children after their parent
void AppendColumnRows(idx_t object_id, const DuckLakeColumnInfo &column, optional_idx parent, idx_t order,
                      const Value &comment, vector<string> &rows, idx_t &max_column_id) {
	max_column_id = MaxValue(max_column_id, column.id.index);
	rows.push_back(StringUtil::Format("%d, %d, %s, %d, %s, %s, %s, %s, %s, %s, %s", object_id, column.id.index,
	                                  parent.IsValid() ? to_string(parent.GetIndex()) : string("NULL"), order,
	                                  Literal(column.name), Literal(column.type), ValueLiteral(column.initial_default),
	                                  ValueLiteral(column.default_value), Literal(column.default_value_type),
	                                  column.nulls_allowed ? "true" : "false", ValueLiteral(comment)));
	for (idx_t child_idx = 0; child_idx < column.children.size(); child_idx++) {
		AppendColumnRows(object_id, column.children[child_idx], column.id.index, child_idx, Value(), rows,
		                 max_column_id);
	}
}

DuckLakeBranchObjectRows EncodeTable(DuckLakeTransaction &transaction, DuckLakeLoadedBranch &loaded,
                                     DuckLakeTableEntry &table, idx_t object_id,
                                     optional_ptr<const ObjectIdAllocator> allocate) {
	auto &schema = table.ParentSchema().Cast<DuckLakeSchemaEntry>();
	auto path = transaction.GetMetadataManager().GetRelativePath(table.DataPath());
	DuckLakeBranchObjectRows rows;
	rows.object_type = TABLE_OBJECT;
	vector<Value> comments;
	for (auto &column : table.GetColumns().Logical()) {
		comments.push_back(column.Comment());
	}
	auto columns = table.GetTableColumns();
	idx_t max_column_id = 0;
	for (idx_t column_idx = 0; column_idx < columns.size(); column_idx++) {
		AppendColumnRows(object_id, columns[column_idx], optional_idx(), column_idx, comments[column_idx],
		                 rows.column_rows, max_column_id);
	}
	rows.object_row = StringUtil::Format(
	    "%d, 'table', %d, %s, %s, %s, %s, NULL, NULL, %s, %d", object_id, StoredSchemaId(loaded, schema, allocate),
	    Literal(table.name.GetIdentifierName()), Literal(table.GetTableUUID()), Literal(path.path),
	    path.path_is_relative ? "true" : "false", ValueLiteral(table.comment), max_column_id + 1);
	return rows;
}

DuckLakeBranchObjectRows EncodeView(DuckLakeLoadedBranch &loaded, DuckLakeViewEntry &view, idx_t object_id,
                                    optional_ptr<const ObjectIdAllocator> allocate) {
	auto &schema = view.ParentSchema().Cast<DuckLakeSchemaEntry>();
	DuckLakeBranchObjectRows rows;
	rows.object_type = VIEW_OBJECT;
	rows.object_row = StringUtil::Format(
	    "%d, 'view', %d, %s, %s, NULL, NULL, %s, %s, %s, NULL", object_id, StoredSchemaId(loaded, schema, allocate),
	    Literal(view.name.GetIdentifierName()), Literal(view.GetViewUUID()), Literal(view.GetQuerySQL()),
	    Literal(DuckLakeUtil::ToQuotedList(IdentifiersToStrings(view.aliases))), ValueLiteral(view.comment));
	return rows;
}

//! The rows of every schema, table and view the branch created, as the transaction has them now
map<idx_t, DuckLakeBranchObjectRows> EncodeObjects(DuckLakeTransaction &transaction, DuckLakeLoadedBranch &loaded,
                                                   optional_ptr<const ObjectIdAllocator> allocate) {
	auto &state = DuckLakeBranchManager::GetTransactionState(transaction);
	map<idx_t, DuckLakeBranchObjectRows> result;
	if (state.new_schemas) {
		vector<reference<DuckLakeSchemaEntry>> schemas;
		for (auto &entry : state.new_schemas->GetEntries()) {
			schemas.push_back(entry.second->Cast<DuckLakeSchemaEntry>());
		}
		// parents first, so that they get their ids first
		std::stable_sort(schemas.begin(), schemas.end(),
		                 [](const reference<DuckLakeSchemaEntry> &a, const reference<DuckLakeSchemaEntry> &b) {
			                 return a.get().SchemaDepth() < b.get().SchemaDepth();
		                 });
		for (auto &schema_ref : schemas) {
			auto &schema = schema_ref.get();
			auto object_id = ObjectId(loaded, schema.GetSchemaId().index, allocate);
			auto parent = schema.ParentDuckLakeSchema();
			auto path = transaction.GetMetadataManager().GetRelativePath(schema.DataPath());
			DuckLakeBranchObjectRows rows;
			rows.object_type = SCHEMA_OBJECT;
			rows.object_row =
			    StringUtil::Format("%d, 'schema', %s, %s, %s, %s, %s, NULL, NULL, NULL, NULL", object_id,
			                       parent ? to_string(StoredSchemaId(loaded, *parent, allocate)) : string("NULL"),
			                       Literal(schema.name.GetIdentifierName()), Literal(schema.GetSchemaUUID()),
			                       Literal(path.path), path.path_is_relative ? "true" : "false");
			result.emplace(object_id, std::move(rows));
		}
	}
	for (auto &schema_entry : state.new_tables) {
		for (auto &entry : schema_entry.second->GetEntries()) {
			auto &catalog_entry = *entry.second;
			if (catalog_entry.type == CatalogType::TABLE_ENTRY) {
				auto &table = catalog_entry.Cast<DuckLakeTableEntry>();
				if (!IsTransactionLocal(table.GetTableId())) {
					// a main table renamed on the branch
					continue;
				}
				auto object_id = ObjectId(loaded, table.GetTableId().index, allocate);
				result.emplace(object_id, EncodeTable(transaction, loaded, table, object_id, allocate));
			} else {
				auto &view = catalog_entry.Cast<DuckLakeViewEntry>();
				if (!IsTransactionLocal(view.GetViewId())) {
					continue;
				}
				auto object_id = ObjectId(loaded, view.GetViewId().index, allocate);
				result.emplace(object_id, EncodeView(loaded, view, object_id, allocate));
			}
		}
	}
	return result;
}

//! What the transaction does to main's objects: ducklake_branching_main_change rows -> their changes_made entry
map<string, string> EncodeMainChanges(DuckLakeTransaction &transaction, DuckLakeSnapshot fork_snapshot) {
	auto &state = DuckLakeBranchManager::GetTransactionState(transaction);
	map<string, string> result;
	auto add = [&](const char *object_type, idx_t id, const char *change, const string &new_name) {
		auto row = StringUtil::Format("'%s', %d, '%s', %s", object_type, id, change,
		                              new_name.empty() ? string("NULL") : Literal(new_name));
		result.emplace(std::move(row), StringUtil::Format("%s_%s:%d", change, object_type, id));
	};
	for (auto &table_id : state.dropped_tables) {
		add(TABLE_OBJECT, table_id.index, "dropped", string());
	}
	for (auto &view_id : state.dropped_views) {
		add(VIEW_OBJECT, view_id.index, "dropped", string());
	}
	for (auto &schema : state.dropped_schemas) {
		add(SCHEMA_OBJECT, schema.first.index, "dropped", string());
	}
	if (state.renamed_tables.empty() && state.renamed_views.empty()) {
		return result;
	}
	auto &fork_set = transaction.GetCatalog().GetSchemaForSnapshot(transaction, fork_snapshot);
	for (auto &schema_entry : state.new_tables) {
		for (auto &entry : schema_entry.second->GetEntries()) {
			auto &catalog_entry = *entry.second;
			auto is_table = catalog_entry.type == CatalogType::TABLE_ENTRY;
			auto id = is_table ? catalog_entry.Cast<DuckLakeTableEntry>().GetTableId()
			                   : catalog_entry.Cast<DuckLakeViewEntry>().GetViewId();
			if (IsTransactionLocal(id)) {
				continue;
			}
			// the only change a branch makes to a main table or view is a rename
			auto &new_name = catalog_entry.name.GetIdentifierName();
			auto fork_entry = MainEntryById(fork_set, id, catalog_entry.type);
			if (fork_entry && fork_entry->name.GetIdentifierName() == new_name) {
				// renamed back to its name at the fork
				continue;
			}
			add(is_table ? TABLE_OBJECT : VIEW_OBJECT, id.index, "renamed", new_name);
		}
	}
	return result;
}

//===--------------------------------------------------------------------===//
// Rebuilding a branch's catalog changes
//===--------------------------------------------------------------------===//
struct StoredColumn {
	idx_t column_id;
	optional_idx parent;
	idx_t order;
	DuckLakeColumnInfo info;
	Value comment;
};

struct StoredObject {
	idx_t object_id;
	string object_type;
	optional_idx parent_id;
	string name;
	string uuid;
	string path;
	bool path_is_relative = false;
	string view_sql;
	string view_aliases;
	Value comment;
	optional_idx next_column_id;
	vector<StoredColumn> columns;
};

struct StoredMainChange {
	string object_type;
	idx_t main_id;
	string change_type;
	string new_name;
};

//! The field id of a stored column with its children; mirrors how DuckLake's catalog loader reads ducklake_column
unique_ptr<DuckLakeFieldId> ColumnFieldId(const DuckLakeColumnInfo &col) {
	DuckLakeColumnData col_data;
	col_data.id = col.id;
	if (col.children.empty()) {
		auto col_type = DuckLakeTypes::FromString(col.type);
		col_data.initial_default = col.initial_default.DefaultCastAs(col_type);
		if (col.default_value.IsNull()) {
			col_data.default_value = ConstantExpression::Null();
		} else if (col.default_value_type == "expression") {
			auto expressions = Parser::ParseExpressionList(col.default_value.GetValue<string>());
			if (expressions.size() != 1) {
				throw InvalidInputException("Branch column \"%s\" has an invalid default", col.name);
			}
			col_data.default_value = std::move(expressions[0]);
		} else {
			col_data.default_value = ConstantExpression::FromValue(col.default_value);
		}
		return make_uniq<DuckLakeFieldId>(std::move(col_data), col.name, std::move(col_type));
	}
	vector<unique_ptr<DuckLakeFieldId>> child_fields;
	for (auto &child : col.children) {
		child_fields.push_back(ColumnFieldId(child));
	}
	LogicalType type;
	if (StringUtil::CIEquals(col.type, "struct")) {
		child_list_t<LogicalType> child_types;
		for (auto &child : child_fields) {
			child_types.emplace_back(child->Name(), child->Type());
		}
		type = LogicalType::STRUCT(std::move(child_types));
	} else if (StringUtil::CIEquals(col.type, "list") && child_fields.size() == 1) {
		type = LogicalType::LIST(child_fields[0]->Type());
	} else if (StringUtil::CIEquals(col.type, "map") && child_fields.size() == 2) {
		type = LogicalType::MAP(child_fields[0]->Type(), child_fields[1]->Type());
	} else {
		throw InvalidInputException("Branch column \"%s\" has an invalid nested type \"%s\"", col.name, col.type);
	}
	return make_uniq<DuckLakeFieldId>(std::move(col_data), col.name, std::move(type), std::move(child_fields));
}

//! The root columns of a stored table, with their children attached
vector<StoredColumn> BuildColumnTree(vector<StoredColumn> columns, const string &branch_name) {
	map<idx_t, vector<idx_t>> children;
	vector<idx_t> roots;
	for (idx_t column_idx = 0; column_idx < columns.size(); column_idx++) {
		auto &column = columns[column_idx];
		if (column.parent.IsValid()) {
			children[column.parent.GetIndex()].push_back(column_idx);
		} else {
			roots.push_back(column_idx);
		}
	}
	auto by_order = [&](idx_t a, idx_t b) {
		return columns[a].order < columns[b].order;
	};
	idx_t attached = roots.size();
	std::function<DuckLakeColumnInfo(idx_t)> build = [&](idx_t column_idx) {
		auto result = columns[column_idx].info;
		auto entry = children.find(columns[column_idx].column_id);
		if (entry != children.end()) {
			std::sort(entry->second.begin(), entry->second.end(), by_order);
			for (auto &child_idx : entry->second) {
				attached++;
				result.children.push_back(build(child_idx));
			}
		}
		return result;
	};
	std::sort(roots.begin(), roots.end(), by_order);
	vector<StoredColumn> result;
	for (auto &root_idx : roots) {
		auto root = columns[root_idx];
		root.info = build(root_idx);
		result.push_back(std::move(root));
	}
	if (attached != columns.size()) {
		throw InvalidInputException("Branch \"%s\" has a column whose parent column is missing", branch_name);
	}
	return result;
}

} // namespace

void DuckLakeBranchManager::LoadDefinitions(DuckLakeTransaction &transaction, const DuckLakeBranchInfo &branch,
                                            DuckLakeSnapshot fork_snapshot, DuckLakeLoadedBranch &loaded) {
	if (!HasDefinitionTables(transaction)) {
		return;
	}
	auto filter = StringUtil::Format("branch_id = %d AND begin_seq <= %d", branch.branch_id, branch.head_seq);
	vector<StoredMainChange> main_changes;
	auto main_result = RunBranchQuery(transaction,
	                                  "SELECT object_type, main_id, change_type, new_name FROM "
	                                  "{METADATA_CATALOG}.ducklake_branching_main_change WHERE " +
	                                      filter + " ORDER BY main_id",
	                                  "Failed to read DuckLake branch catalog changes: ");
	for (auto &row : *main_result) {
		StoredMainChange change;
		change.object_type = row.GetValue<string>(0);
		change.main_id = row.GetValue<idx_t>(1);
		change.change_type = row.GetValue<string>(2);
		change.new_name = row.IsNull(3) ? string() : row.GetValue<string>(3);
		auto valid_type = change.object_type == TABLE_OBJECT || change.object_type == VIEW_OBJECT ||
		                  (change.object_type == SCHEMA_OBJECT && change.change_type == "dropped");
		if (!valid_type || (change.change_type != "dropped" && change.change_type != "renamed")) {
			throw InvalidInputException("Branch \"%s\" has an unknown catalog change \"%s\" of a %s", branch.name,
			                            change.change_type, change.object_type);
		}
		main_changes.push_back(std::move(change));
	}
	vector<StoredObject> objects;
	unordered_map<idx_t, idx_t> object_positions;
	auto object_result = RunBranchQuery(
	    transaction,
	    "SELECT object_id, object_type, parent_id, object_name, uuid, path, path_is_relative, view_sql, view_aliases, "
	    "comment, next_column_id FROM {METADATA_CATALOG}.ducklake_branching_object WHERE " +
	        filter + " ORDER BY object_id",
	    "Failed to read DuckLake branch catalog objects: ");
	for (auto &row : *object_result) {
		StoredObject object;
		object.object_id = row.GetValue<idx_t>(0);
		object.object_type = row.GetValue<string>(1);
		if (object.object_type != SCHEMA_OBJECT && object.object_type != TABLE_OBJECT &&
		    object.object_type != VIEW_OBJECT) {
			throw InvalidInputException("Branch \"%s\" has an object of unknown type \"%s\"", branch.name,
			                            object.object_type);
		}
		if (!row.IsNull(2)) {
			object.parent_id = row.GetValue<idx_t>(2);
		}
		object.name = row.GetValue<string>(3);
		object.uuid = row.GetValue<string>(4);
		object.path = row.IsNull(5) ? string() : row.GetValue<string>(5);
		object.path_is_relative = !row.IsNull(6) && row.GetValue<bool>(6);
		object.view_sql = row.IsNull(7) ? string() : row.GetValue<string>(7);
		object.view_aliases = row.IsNull(8) ? string() : row.GetValue<string>(8);
		object.comment = row.IsNull(9) ? Value() : Value(row.GetValue<string>(9));
		if (!row.IsNull(10)) {
			object.next_column_id = row.GetValue<idx_t>(10);
		}
		object_positions[object.object_id] = objects.size();
		objects.push_back(std::move(object));
	}
	auto column_result = RunBranchQuery(
	    transaction,
	    "SELECT object_id, column_id, parent_column, column_order, column_name, column_type, initial_default, "
	    "default_value, default_value_type, nulls_allowed, comment FROM "
	    "{METADATA_CATALOG}.ducklake_branching_column WHERE " +
	        filter,
	    "Failed to read DuckLake branch columns: ");
	for (auto &row : *column_result) {
		auto position = object_positions.find(row.GetValue<idx_t>(0));
		if (position == object_positions.end() || objects[position->second].object_type != TABLE_OBJECT) {
			throw InvalidInputException("Branch \"%s\" has a column of a table it does not have", branch.name);
		}
		StoredColumn column;
		column.column_id = row.GetValue<idx_t>(1);
		if (!row.IsNull(2)) {
			column.parent = row.GetValue<idx_t>(2);
		}
		column.order = row.GetValue<idx_t>(3);
		column.info.id = FieldIndex(column.column_id);
		column.info.name = row.GetValue<string>(4);
		column.info.type = row.GetValue<string>(5);
		column.info.initial_default = row.IsNull(6) ? Value() : Value(row.GetValue<string>(6));
		column.info.default_value = row.IsNull(7) ? Value() : Value(row.GetValue<string>(7));
		column.info.default_value_type = row.IsNull(8) ? string() : row.GetValue<string>(8);
		column.info.nulls_allowed = row.IsNull(9) || row.GetValue<bool>(9);
		column.comment = row.IsNull(10) ? Value() : Value(row.GetValue<string>(10));
		objects[position->second].columns.push_back(std::move(column));
	}
	if (main_changes.empty() && objects.empty()) {
		return;
	}

	auto context_ref = transaction.context.lock();
	auto &context = *context_ref;
	auto &catalog = transaction.GetCatalog();
	auto &state = *transaction.state;
	auto &fork_set = catalog.GetSchemaForSnapshot(transaction, fork_snapshot);
	auto main_entry = [&](const string &object_type, idx_t id) -> CatalogEntry & {
		optional_ptr<CatalogEntry> entry;
		auto expected = CatalogType::SCHEMA_ENTRY;
		if (object_type == SCHEMA_OBJECT) {
			entry = fork_set.GetEntryById(SchemaIndex(id));
		} else {
			expected = object_type == TABLE_OBJECT ? CatalogType::TABLE_ENTRY : CatalogType::VIEW_ENTRY;
			entry = MainEntryById(fork_set, TableIndex(id), expected);
		}
		if (!entry || entry->type != expected) {
			throw InvalidInputException("Branch \"%s\" changed %s %d, which does not exist at its fork", branch.name,
			                            object_type, id);
		}
		return *entry;
	};
	auto schema_for = [&](idx_t stored_id) -> optional_ptr<DuckLakeSchemaEntry> {
		if (stored_id < BRANCH_FILE_ID_BASE) {
			return &main_entry(SCHEMA_OBJECT, stored_id).Cast<DuckLakeSchemaEntry>();
		}
		auto local_id = loaded.local_ids.find(stored_id);
		if (local_id == loaded.local_ids.end()) {
			return nullptr;
		}
		return &transaction.GetLocalEntryById(SchemaIndex(local_id->second))->Cast<DuckLakeSchemaEntry>();
	};
	auto add_local_id = [&](idx_t object_id, idx_t local_id) {
		loaded.local_ids[object_id] = local_id;
		loaded.object_ids[local_id] = object_id;
	};

	// main's objects the branch dropped go first, so that the branch's own objects can reuse their names
	for (auto &change : main_changes) {
		if (change.change_type != "dropped") {
			continue;
		}
		auto &entry = main_entry(change.object_type, change.main_id);
		if (entry.type == CatalogType::SCHEMA_ENTRY) {
			state.dropped_schemas.emplace(SchemaIndex(change.main_id), entry.Cast<DuckLakeSchemaEntry>());
		} else if (entry.type == CatalogType::TABLE_ENTRY) {
			state.dropped_tables.insert(TableIndex(change.main_id));
			loaded.dropped_main_tables.insert(TableIndex(change.main_id));
		} else {
			state.dropped_views.insert(TableIndex(change.main_id));
		}
	}

	// the schemas the branch created, parents first
	vector<reference<StoredObject>> pending;
	for (auto &object : objects) {
		if (object.object_type == SCHEMA_OBJECT) {
			pending.push_back(object);
		}
	}
	while (!pending.empty()) {
		vector<reference<StoredObject>> waiting;
		for (auto &object_ref : pending) {
			auto &object = object_ref.get();
			optional_ptr<DuckLakeSchemaEntry> parent;
			if (object.parent_id.IsValid()) {
				parent = schema_for(object.parent_id.GetIndex());
				if (!parent) {
					waiting.push_back(object);
					continue;
				}
			}
			CreateSchemaInfo schema_info;
			schema_info.SetQualifiedName(QualifiedName(schema_info.GetQualifiedName().Catalog(),
			                                           Identifier(object.name), schema_info.GetQualifiedName().Name()));
			SchemaIndex local_id(transaction.GetLocalCatalogId());
			auto path = LoadBranchPath(catalog, catalog.DataPath(), object.path, object.path_is_relative);
			transaction.CreateEntry(
			    make_uniq<DuckLakeSchemaEntry>(catalog, schema_info, local_id, object.uuid, std::move(path), parent));
			add_local_id(object.object_id, local_id.index);
		}
		if (waiting.size() == pending.size()) {
			throw InvalidInputException("Branch \"%s\" has a schema whose parent schema is missing", branch.name);
		}
		pending = std::move(waiting);
	}

	// main's tables and views the branch renamed, through DuckLake's own ALTER
	for (auto &change : main_changes) {
		if (change.change_type != "renamed") {
			continue;
		}
		auto &entry = main_entry(change.object_type, change.main_id);
		auto &schema = entry.ParentSchema().Cast<DuckLakeSchemaEntry>();
		AlterEntryData alter_data(schema.GetQualifiedName(entry.name), OnEntryNotFound::THROW_EXCEPTION);
		if (entry.type == CatalogType::TABLE_ENTRY) {
			auto &table = entry.Cast<DuckLakeTableEntry>();
			RenameTableInfo info(alter_data, Identifier(change.new_name));
			transaction.AlterEntry(table, table.Alter(context, transaction, info));
		} else {
			auto &view = entry.Cast<DuckLakeViewEntry>();
			RenameViewInfo info(alter_data, Identifier(change.new_name));
			transaction.AlterEntry(view, view.AlterEntry(context, info));
		}
	}

	// the tables and views the branch created - tables first, since views read them
	Identifier catalog_name(catalog.GetName());
	for (auto object_type : {TABLE_OBJECT, VIEW_OBJECT}) {
		for (auto &object : objects) {
			if (object.object_type != object_type) {
				continue;
			}
			auto schema = object.parent_id.IsValid() ? schema_for(object.parent_id.GetIndex()) : nullptr;
			if (!schema) {
				throw InvalidInputException("Branch \"%s\" has %s \"%s\" without a schema", branch.name,
				                            object.object_type, object.name);
			}
			Identifier schema_name(schema->name.GetIdentifierName());
			TableIndex local_id(transaction.GetLocalCatalogId());
			Identifier object_name(object.name);
			if (object_type == VIEW_OBJECT) {
				auto view_info = make_uniq<CreateViewInfo>(*schema, object_name);
				view_info->aliases = StringsToIdentifiers(DuckLakeUtil::ParseQuotedList(object.view_aliases));
				auto view_entry = make_uniq<DuckLakeViewEntry>(catalog, *schema, *view_info, local_id, object.uuid,
				                                               object.view_sql, LocalChangeType::CREATED);
				auto &view = *view_entry;
				transaction.CreateEntry(std::move(view_entry));
				add_local_id(object.object_id, local_id.index);
				if (!object.comment.IsNull()) {
					SetCommentInfo info(CatalogType::VIEW_ENTRY, catalog_name, schema_name, object_name, object.comment,
					                    OnEntryNotFound::THROW_EXCEPTION);
					transaction.AlterEntry(view, view.AlterEntry(context, info));
				}
				continue;
			}
			if (!object.next_column_id.IsValid()) {
				throw InvalidInputException("Branch \"%s\" has table \"%s\" without a next column id", branch.name,
				                            object.name);
			}
			auto columns = BuildColumnTree(std::move(object.columns), branch.name);
			auto table_info = make_uniq<CreateTableInfo>(*schema, object_name);
			auto field_data = make_shared_ptr<DuckLakeFieldData>();
			for (auto &column : columns) {
				auto field_id = ColumnFieldId(column.info);
				ColumnDefinition definition(Identifier(column.info.name), field_id->Type());
				auto default_value = field_id->GetDefault();
				if (default_value) {
					definition.SetDefaultValue(std::move(default_value));
				}
				table_info->columns.AddColumn(std::move(definition));
				field_data->Add(std::move(field_id));
			}
			for (auto &column : columns) {
				if (!column.info.nulls_allowed) {
					auto &definition = table_info->columns.GetColumn(Identifier(column.info.name));
					table_info->constraints.push_back(make_uniq<NotNullConstraint>(definition.Logical()));
				}
			}
			auto path = LoadBranchPath(catalog, catalog.DataPath(), object.path, object.path_is_relative);
			auto table_entry = make_uniq<DuckLakeTableEntry>(
			    catalog, *schema, *table_info, local_id, object.uuid, std::move(path), std::move(field_data),
			    object.next_column_id, vector<DuckLakeInlinedTableInfo>(), LocalChangeType::CREATED);
			reference<DuckLakeTableEntry> latest = *table_entry;
			transaction.CreateEntry(std::move(table_entry));
			add_local_id(object.object_id, local_id.index);
			// comments go through DuckLake's own ALTER, so that a merge writes them as COMMENT ON does
			auto apply = [&](unique_ptr<CatalogEntry> new_entry) {
				auto &next = new_entry->Cast<DuckLakeTableEntry>();
				transaction.AlterEntry(latest.get(), std::move(new_entry));
				latest = next;
			};
			if (!object.comment.IsNull()) {
				SetCommentInfo info(CatalogType::TABLE_ENTRY, catalog_name, schema_name, object_name, object.comment,
				                    OnEntryNotFound::THROW_EXCEPTION);
				apply(latest.get().Alter(transaction, info));
			}
			for (auto &column : columns) {
				if (column.comment.IsNull()) {
					continue;
				}
				SetColumnCommentInfo info(catalog_name, schema_name, object_name, Identifier(column.info.name),
				                          column.comment, OnEntryNotFound::THROW_EXCEPTION);
				apply(latest.get().Alter(transaction, info));
			}
		}
	}
	// statements prepared before the branch's catalog changes were loaded are bound again
	transaction.catalog_version = catalog.GetNewUncommittedCatalogVersion();
	loaded.objects = EncodeObjects(transaction, loaded, nullptr);
	for (auto &change : EncodeMainChanges(transaction, fork_snapshot)) {
		loaded.main_changes.insert(change.first);
	}
}

optional_ptr<CatalogEntry> DuckLakeBranchManager::GetTableEntry(DuckLakeTransaction &transaction,
                                                                DuckLakeSnapshot snapshot, TableIndex table_id) {
	if (!IsTransactionLocal(table_id)) {
		return transaction.GetCatalog().GetEntryById(transaction, snapshot, table_id);
	}
	// DuckLake does not index the transaction's own tables by id
	for (auto &schema_entry : transaction.state->new_tables) {
		for (auto &entry : schema_entry.second->GetEntries()) {
			if (entry.second->type == CatalogType::TABLE_ENTRY &&
			    entry.second->Cast<DuckLakeTableEntry>().GetTableId() == table_id) {
				return entry.second.get();
			}
		}
	}
	return nullptr;
}

TableIndex DuckLakeBranchManager::LocalTableId(const DuckLakeLoadedBranch &loaded, idx_t stored_id,
                                               const string &branch_name) {
	if (stored_id < BRANCH_FILE_ID_BASE) {
		return TableIndex(stored_id);
	}
	auto entry = loaded.local_ids.find(stored_id);
	if (entry == loaded.local_ids.end()) {
		throw InvalidInputException("Branch \"%s\" has files of table %d, which it does not have", branch_name,
		                            stored_id);
	}
	return TableIndex(entry->second);
}

idx_t DuckLakeBranchManager::StoredTableId(const DuckLakeLoadedBranch &loaded, TableIndex table_id) {
	if (!IsTransactionLocal(table_id)) {
		return table_id.index;
	}
	auto entry = loaded.object_ids.find(table_id.index);
	if (entry == loaded.object_ids.end()) {
		throw InternalException("Branch table without a branch object id");
	}
	return entry->second;
}

//===--------------------------------------------------------------------===//
// Committing a branch's catalog changes
//===--------------------------------------------------------------------===//
DuckLakeBranchDefinitionChanges DuckLakeBranchManager::WriteDefinitions(DuckLakeTransaction &transaction,
                                                                        DuckLakeLoadedBranch &loaded, idx_t new_seq,
                                                                        const std::function<idx_t()> &next_object_id) {
	DuckLakeBranchDefinitionChanges result;
	auto &state = *transaction.state;
	auto branch_id = loaded.info.branch_id;
	auto objects = EncodeObjects(transaction, loaded, &next_object_id);
	auto main_changes = EncodeMainChanges(transaction, transaction.GetSnapshot());
	auto add_change = [&](const string &change) {
		if (!result.changes_made.empty()) {
			result.changes_made += ",";
		}
		result.changes_made += change;
	};

	string batch, object_rows, column_rows;
	set<idx_t> replaced;
	for (auto &entry : objects) {
		auto &rows = entry.second;
		auto loaded_entry = loaded.objects.find(entry.first);
		if (loaded_entry == loaded.objects.end()) {
			add_change(StringUtil::Format("created_%s:%d", rows.object_type, entry.first));
		} else if (loaded_entry->second == rows) {
			continue;
		} else {
			replaced.insert(entry.first);
			add_change(StringUtil::Format("altered_%s:%d", rows.object_type, entry.first));
		}
		AppendValues(object_rows, StringUtil::Format("(%d, %d, %s)", branch_id, new_seq, rows.object_row));
		for (auto &column_row : rows.column_rows) {
			AppendValues(column_rows, StringUtil::Format("(%d, %d, %s)", branch_id, new_seq, column_row));
		}
	}
	for (auto &entry : loaded.objects) {
		if (objects.find(entry.first) == objects.end()) {
			replaced.insert(entry.first);
			add_change(StringUtil::Format("dropped_%s:%d", entry.second.object_type, entry.first));
		}
	}
	if (!replaced.empty()) {
		auto ids = BranchIdList(replaced);
		batch +=
		    StringUtil::Format("DELETE FROM {METADATA_CATALOG}.ducklake_branching_object WHERE branch_id = %d AND "
		                       "object_id IN (%s);\nDELETE FROM {METADATA_CATALOG}.ducklake_branching_column WHERE "
		                       "branch_id = %d AND object_id IN (%s);\n",
		                       branch_id, ids, branch_id, ids);
	}
	if (!object_rows.empty()) {
		batch += "INSERT INTO {METADATA_CATALOG}.ducklake_branching_object VALUES " + object_rows + ";\n";
	}
	if (!column_rows.empty()) {
		batch += "INSERT INTO {METADATA_CATALOG}.ducklake_branching_column VALUES " + column_rows + ";\n";
	}

	set<string> main_change_rows;
	for (auto &change : main_changes) {
		main_change_rows.insert(change.first);
	}
	if (main_change_rows != loaded.main_changes) {
		batch += StringUtil::Format(
		    "DELETE FROM {METADATA_CATALOG}.ducklake_branching_main_change WHERE branch_id = %d;\n", branch_id);
		string rows;
		for (auto &change : main_changes) {
			AppendValues(rows, StringUtil::Format("(%d, %d, %s)", branch_id, new_seq, change.first));
			if (loaded.main_changes.find(change.first) == loaded.main_changes.end()) {
				add_change(change.second);
			}
		}
		if (!rows.empty()) {
			batch += "INSERT INTO {METADATA_CATALOG}.ducklake_branching_main_change VALUES " + rows + ";\n";
		}
	}

	// a main table dropped on the branch keeps none of the branch's changes to it
	set<idx_t> newly_dropped;
	for (auto &table_id : state.dropped_tables) {
		if (loaded.dropped_main_tables.find(table_id) == loaded.dropped_main_tables.end()) {
			newly_dropped.insert(table_id.index);
		}
	}
	if (!newly_dropped.empty()) {
		auto ids = BranchIdList(newly_dropped);
		batch += StringUtil::Format(
		    "DELETE FROM {METADATA_CATALOG}.ducklake_branching_inlined_delete WHERE branch_id = %d AND table_id IN "
		    "(%s);\nDELETE FROM {METADATA_CATALOG}.ducklake_branching_dropped_file WHERE branch_id = %d AND table_id "
		    "IN (%s);\n",
		    branch_id, ids, branch_id, ids);
	}
	if (!batch.empty() && !HasDefinitionTables(transaction)) {
		batch = string(DEFINITION_TABLES_SQL) + batch;
		result.creates_tables = true;
	}
	result.batch = std::move(batch);
	return result;
}

void DuckLakeBranchManager::ClearDefinitions(DuckLakeTransaction &transaction) {
	auto &state = *transaction.state;
	// tables refer to their schemas, so they go first
	state.new_tables.clear();
	state.new_schemas.reset();
	state.dropped_tables.clear();
	state.dropped_views.clear();
	state.dropped_schemas.clear();
	state.renamed_tables.clear();
	state.renamed_views.clear();
}

string DuckLakeBranchManager::CatalogChangesFingerprint(DuckLakeTransaction &transaction) {
	auto &state = *transaction.state;
	idx_t entries = 0;
	auto count = [&](const DuckLakeCatalogSet &set) {
		for (auto &entry : set.GetEntries()) {
			for (reference<CatalogEntry> version = *entry.second;; version = version.get().Child()) {
				entries++;
				if (!version.get().HasChild()) {
					break;
				}
			}
		}
	};
	if (state.new_schemas) {
		count(*state.new_schemas);
	}
	for (auto &sets : {&state.new_tables, &state.new_scalar_macros, &state.new_table_macros}) {
		for (auto &entry : *sets) {
			count(*entry.second);
		}
	}
	return StringUtil::Format("%d/%d/%d/%d/%d/%d/%d/%d", entries, state.dropped_tables.size(),
	                          state.dropped_views.size(), state.dropped_schemas.size(), state.renamed_tables.size(),
	                          state.renamed_views.size(), state.dropped_scalar_macros.size(),
	                          state.dropped_table_macros.size());
}

//===--------------------------------------------------------------------===//
// What a branch can change
//===--------------------------------------------------------------------===//
void DuckLakeBranchManager::CheckCreate(DuckLakeTransaction &transaction, CatalogEntry &entry) {
	if (!IsOnBranch(transaction)) {
		return;
	}
	switch (entry.type) {
	case CatalogType::SCHEMA_ENTRY:
	case CatalogType::VIEW_ENTRY:
		return;
	case CatalogType::TABLE_ENTRY: {
		auto &table = entry.Cast<DuckLakeTableEntry>();
		if (table.GetPartitionData()) {
			EnsureNotOnBranch(transaction, "CREATE TABLE ... PARTITIONED BY");
		}
		if (table.GetSortData()) {
			EnsureNotOnBranch(transaction, "CREATE TABLE ... SORTED BY");
		}
		if (!table.GetTableOptions().empty()) {
			EnsureNotOnBranch(transaction, "CREATE TABLE ... WITH options");
		}
		return;
	}
	default:
		EnsureNotOnBranch(transaction, "CREATE MACRO");
	}
}

void DuckLakeBranchManager::CheckDrop(DuckLakeTransaction &transaction, CatalogEntry &entry) {
	if (!IsOnBranch(transaction)) {
		return;
	}
	switch (entry.type) {
	case CatalogType::SCHEMA_ENTRY:
	case CatalogType::TABLE_ENTRY:
	case CatalogType::VIEW_ENTRY:
		return;
	default:
		EnsureNotOnBranch(transaction, "DROP MACRO");
	}
}

void DuckLakeBranchManager::CheckAlter(DuckLakeTransaction &transaction, CatalogEntry &entry,
                                       optional_ptr<CatalogEntry> new_entry) {
	if (!IsOnBranch(transaction) || !new_entry) {
		return;
	}
	auto is_table = entry.type == CatalogType::TABLE_ENTRY;
	if (!is_table && entry.type != CatalogType::VIEW_ENTRY) {
		throw InternalException("DuckLake alters only tables and views");
	}
	auto change = is_table ? new_entry->Cast<DuckLakeTableEntry>().GetLocalChange().type
	                       : new_entry->Cast<DuckLakeViewEntry>().GetLocalChange().type;
	auto id = is_table ? entry.Cast<DuckLakeTableEntry>().GetTableId() : entry.Cast<DuckLakeViewEntry>().GetViewId();
	if (change == LocalChangeType::RENAMED) {
		return;
	}
	if (!IsTransactionLocal(id)) {
		EnsureNotOnBranch(transaction, is_table ? "Changing a main table other than renaming it"
		                                        : "Changing a main view other than renaming it");
	}
	switch (change) {
	case LocalChangeType::ADD_COLUMN:
	case LocalChangeType::RENAME_COLUMN:
	case LocalChangeType::SET_DEFAULT:
	case LocalChangeType::SET_NULL:
	case LocalChangeType::DROP_NULL:
	case LocalChangeType::SET_COMMENT:
		return;
	case LocalChangeType::SET_COLUMN_COMMENT:
		if (!is_table) {
			EnsureNotOnBranch(transaction, "COMMENT ON COLUMN of a view");
		}
		return;
	case LocalChangeType::REMOVE_COLUMN:
		EnsureNotOnBranch(transaction, "DROP COLUMN");
		return;
	case LocalChangeType::CHANGE_COLUMN_TYPE:
		EnsureNotOnBranch(transaction, "Changing a column type or its struct fields");
		return;
	case LocalChangeType::SET_PARTITION_KEY:
		EnsureNotOnBranch(transaction, "SET PARTITIONED BY");
		return;
	case LocalChangeType::SET_SORT_KEY:
		EnsureNotOnBranch(transaction, "SET SORTED BY");
		return;
	default:
		throw InternalException("Unknown ALTER on a branch");
	}
}

void DuckLakeBranchManager::EnsureLoaded(DuckLakeTransaction &transaction) {
	if (IsOnBranch(transaction)) {
		// loads the branch on first use
		transaction.GetSnapshot();
	}
}

//===--------------------------------------------------------------------===//
// Merging a branch's catalog changes
//===--------------------------------------------------------------------===//
optional_ptr<CatalogEntry> DuckLakeBranchManager::GetMainEntry(DuckLakeTransaction &transaction,
                                                               DuckLakeSnapshot snapshot, TableIndex id,
                                                               CatalogType type) {
	return MainEntryById(transaction.GetCatalog().GetSchemaForSnapshot(transaction, snapshot), id, type);
}

void DuckLakeBranchManager::CheckMergeDefinitions(DuckLakeTransaction &transaction, const string &branch_name,
                                                  DuckLakeSnapshot fork_snapshot,
                                                  const SnapshotChangeInformation &other_changes) {
	auto &state = *transaction.state;
	set<TableIndex> tables = state.dropped_tables;
	tables.insert(state.renamed_tables.begin(), state.renamed_tables.end());
	set<TableIndex> views = state.dropped_views;
	views.insert(state.renamed_views.begin(), state.renamed_views.end());
	auto renamed_tables = RenamedOnMain(transaction, false, tables, fork_snapshot.snapshot_id);
	auto renamed_views = RenamedOnMain(transaction, true, views, fork_snapshot.snapshot_id);
	auto &fork_set = transaction.GetCatalog().GetSchemaForSnapshot(transaction, fork_snapshot);
	auto conflict = [&](const char *branch_action, const char *object_type, TableIndex id, const char *main_action) {
		auto entry = MainEntryById(fork_set, id,
		                           StringUtil::Equals(object_type, "table") ? CatalogType::TABLE_ENTRY
		                                                                    : CatalogType::VIEW_ENTRY);
		throw TransactionException("Transaction conflict - branch \"%s\" %s %s \"%s\", but main %s it since the fork",
		                           branch_name, branch_action, object_type,
		                           entry ? entry->name.GetIdentifierName() : to_string(id.index), main_action);
	};
	auto contains = [](const set<TableIndex> &tables, TableIndex id) {
		return tables.find(id) != tables.end();
	};
	// DuckLake lets a drop win over changes made since; a merge would lose main's changes
	for (auto &id : state.dropped_tables) {
		if (contains(other_changes.inserted_tables, id) || contains(other_changes.tables_inserted_inlined, id)) {
			conflict("dropped", "table", id, "inserted into");
		}
		if (contains(other_changes.tables_deleted_from, id) || contains(other_changes.tables_deleted_inlined, id)) {
			conflict("dropped", "table", id, "deleted from");
		}
		if (contains(other_changes.altered_tables, id)) {
			conflict("dropped", "table", id, "altered");
		}
		if (contains(renamed_tables, id)) {
			conflict("dropped", "table", id, "renamed");
		}
	}
	for (auto &id : state.dropped_views) {
		if (contains(other_changes.altered_views, id)) {
			conflict("dropped", "view", id, "altered");
		}
		if (contains(renamed_views, id)) {
			conflict("dropped", "view", id, "renamed");
		}
	}
	// DuckLake publishes a rename as a created table, which its rename check does not look for
	for (auto &id : state.renamed_tables) {
		if (contains(renamed_tables, id)) {
			conflict("renamed", "table", id, "renamed");
		}
	}
	for (auto &id : state.renamed_views) {
		if (contains(renamed_views, id)) {
			conflict("renamed", "view", id, "renamed");
		}
	}
}

} // namespace duckdb
