#define DUCKDB_EXTENSION_MAIN

#include "dryrun_extension.hpp"

#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/materialized_query_result.hpp"
#include "duckdb/parser/expression/cast_expression.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/comparison_expression.hpp"
#include "duckdb/parser/expression/conjunction_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/star_expression.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/query_node/set_operation_node.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"
#include "duckdb/parser/tableref/joinref.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"

#include <algorithm>
#include <cstdlib>
#include <regex>
#include <unordered_map>
#include <unordered_set>

namespace duckdb {

namespace {

struct DryrunEstimate {
	int64_t estimated_compute_bytes = 0;
	int64_t estimated_compressed_bytes = 0;
	int64_t estimated_uncompressed_bytes = 0;
	int64_t estimated_metadata_bytes = 0;
	int64_t estimated_files = 0;
	int64_t estimated_row_groups = 0;
	int64_t total_row_groups = 0;
	string confidence = "high";
	vector<string> notes;
};

struct DryrunBindData : public TableFunctionData {
	DryrunEstimate estimate;

	unique_ptr<FunctionData> Copy() const override {
		auto result = make_uniq<DryrunBindData>();
		result->column_ids = column_ids;
		result->estimate = estimate;
		return std::move(result);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<DryrunBindData>();
		return estimate.estimated_compute_bytes == other.estimate.estimated_compute_bytes &&
		       estimate.estimated_compressed_bytes == other.estimate.estimated_compressed_bytes &&
		       estimate.estimated_uncompressed_bytes == other.estimate.estimated_uncompressed_bytes &&
		       estimate.estimated_metadata_bytes == other.estimate.estimated_metadata_bytes &&
		       estimate.estimated_files == other.estimate.estimated_files &&
		       estimate.estimated_row_groups == other.estimate.estimated_row_groups &&
		       estimate.total_row_groups == other.estimate.total_row_groups &&
		       estimate.confidence == other.estimate.confidence && estimate.notes == other.estimate.notes;
	}
};

struct ProjectionInfo {
	bool known = false;
	bool all_columns = true;
	unordered_set<string> columns;
};

struct Predicate {
	string column;
	string op;
	string value;
};

struct ParsedQueryInfo {
	vector<string> paths;
	ProjectionInfo projection;
	vector<Predicate> predicates;
	vector<string> notes;
	bool complex_filter = false;
};

struct RowGroupKey {
	string file_name;
	int64_t row_group_id;

	bool operator==(const RowGroupKey &other) const {
		return file_name == other.file_name && row_group_id == other.row_group_id;
	}
};

struct RowGroupKeyHash {
	size_t operator()(const RowGroupKey &key) const {
		return std::hash<string> {}(key.file_name) ^ (std::hash<int64_t> {}(key.row_group_id) << 1);
	}
};

struct ColumnStats {
	string min_value;
	string max_value;
	bool has_min = false;
	bool has_max = false;
};

struct RowGroupEstimate {
	string file_name;
	int64_t row_group_id = 0;
	int64_t compressed_bytes = 0;
	int64_t uncompressed_bytes = 0;
	unordered_map<string, ColumnStats> stats;
};

static string TrimCopy(string value) {
	StringUtil::Trim(value);
	return value;
}

static string UnquoteIdentifier(string value) {
	value = TrimCopy(std::move(value));
	if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
		value = value.substr(1, value.size() - 2);
		string result;
		for (idx_t i = 0; i < value.size(); i++) {
			if (value[i] == '"' && i + 1 < value.size() && value[i + 1] == '"') {
				result += '"';
				i++;
			} else {
				result += value[i];
			}
		}
		return result;
	}
	return value;
}

static string NormalizeColumn(string value) {
	value = UnquoteIdentifier(std::move(value));
	auto dot = value.rfind('.');
	if (dot != string::npos) {
		value = value.substr(dot + 1);
		value = UnquoteIdentifier(std::move(value));
	}
	return StringUtil::Lower(value);
}

static string EscapeSQLString(const string &value) {
	string result;
	result.reserve(value.size() + 2);
	for (auto c : value) {
		result += c;
		if (c == '\'') {
			result += '\'';
		}
	}
	return result;
}

static bool IsHttpGlob(const string &path) {
	auto lower = StringUtil::Lower(path);
	return (StringUtil::StartsWith(lower, "http://") || StringUtil::StartsWith(lower, "https://")) &&
	       StringUtil::Contains(path, "*");
}

static void RejectHttpGlob(const string &path) {
	if (IsHttpGlob(path)) {
		throw BinderException(
		    "dryrun does not support HTTP/HTTPS globbing; use explicit file URLs or read_parquet([...])");
	}
}

static void AddUniquePath(vector<string> &paths, const string &path) {
	if (std::find(paths.begin(), paths.end(), path) == paths.end()) {
		paths.push_back(path);
	}
}

static void CollectPathValue(const Value &value, ParsedQueryInfo &result) {
	if (value.IsNull()) {
		return;
	}
	auto type_id = value.type().id();
	if (type_id == LogicalTypeId::VARCHAR) {
		auto path = value.GetValue<string>();
		auto lower = StringUtil::Lower(path);
		if (StringUtil::Contains(lower, ".parquet")) {
			RejectHttpGlob(path);
			AddUniquePath(result.paths, path);
		} else if (StringUtil::Contains(lower, ".csv") || StringUtil::Contains(lower, ".json")) {
			throw BinderException("dryrun only supports Parquet scans in v1");
		}
		return;
	}
	if (type_id == LogicalTypeId::LIST) {
		for (auto &child : ListValue::GetChildren(value)) {
			CollectPathValue(child, result);
		}
		return;
	}
	if (type_id == LogicalTypeId::ARRAY) {
		for (auto &child : ArrayValue::GetChildren(value)) {
			CollectPathValue(child, result);
		}
	}
}

static void CollectPathConstants(const ParsedExpression &expr, ParsedQueryInfo &result) {
	ParsedExpressionIterator::VisitExpression<ConstantExpression>(
	    expr, [&](const ConstantExpression &constant) { CollectPathValue(constant.value, result); });
}

static void CollectColumnRefs(const ParsedExpression &expr, unordered_set<string> &columns) {
	ParsedExpressionIterator::VisitExpression<ColumnRefExpression>(expr, [&](const ColumnRefExpression &column_ref) {
		columns.insert(NormalizeColumn(column_ref.GetColumnName()));
	});
}

static bool IsTopLevelStar(const ParsedExpression &expr) {
	return expr.GetExpressionClass() == ExpressionClass::STAR;
}

static void CollectTableFunctionPaths(const ParsedExpression &function_expr, ParsedQueryInfo &result) {
	if (function_expr.GetExpressionClass() != ExpressionClass::FUNCTION) {
		result.notes.emplace_back("table function not analyzable");
		return;
	}
	auto &function = function_expr.Cast<FunctionExpression>();
	auto function_name = StringUtil::Lower(function.function_name);
	if (function_name == "read_csv" || function_name == "read_json" || function_name == "read_csv_auto" ||
	    function_name == "read_json_auto") {
		throw BinderException("dryrun only supports Parquet scans in v1");
	}
	if (function_name != "read_parquet" && function_name != "parquet_scan") {
		result.notes.emplace_back("table function not analyzable");
		return;
	}
	for (auto &child : function.children) {
		if (child) {
			CollectPathConstants(*child, result);
		}
	}
}

static void ExtractScanSources(const TableRef &ref, ParsedQueryInfo &result) {
	switch (ref.type) {
	case TableReferenceType::BASE_TABLE: {
		auto &base = ref.Cast<BaseTableRef>();
		auto lower_name = StringUtil::Lower(base.table_name);
		if (StringUtil::Contains(lower_name, ".parquet")) {
			RejectHttpGlob(base.table_name);
			AddUniquePath(result.paths, base.table_name);
		} else if (StringUtil::Contains(lower_name, ".csv") || StringUtil::Contains(lower_name, ".json")) {
			throw BinderException("dryrun only supports Parquet scans in v1");
		} else {
			result.notes.emplace_back("catalog table scan not analyzable in v1");
		}
		break;
	}
	case TableReferenceType::TABLE_FUNCTION: {
		auto &table_function = ref.Cast<TableFunctionRef>();
		if (!table_function.function) {
			result.notes.emplace_back("table function not analyzable");
			break;
		}
		CollectTableFunctionPaths(*table_function.function, result);
		break;
	}
	case TableReferenceType::JOIN: {
		auto &join_ref = ref.Cast<JoinRef>();
		result.notes.emplace_back("join scan bytes estimated independently per Parquet leaf");
		if (join_ref.left) {
			ExtractScanSources(*join_ref.left, result);
		}
		if (join_ref.right) {
			ExtractScanSources(*join_ref.right, result);
		}
		break;
	}
	case TableReferenceType::EMPTY_FROM:
		break;
	default:
		result.notes.emplace_back("table reference not analyzable in v1");
		break;
	}
}

static ProjectionInfo ExtractProjectionInfo(const SelectNode &node, vector<string> &notes) {
	ProjectionInfo result;
	result.known = true;
	result.all_columns = false;
	for (auto &expr : node.select_list) {
		if (!expr) {
			continue;
		}
		if (IsTopLevelStar(*expr)) {
			result.all_columns = true;
			result.columns.clear();
			return result;
		}
		CollectColumnRefs(*expr, result.columns);
	}
	if (!node.groups.group_expressions.empty()) {
		notes.emplace_back("grouping expressions included in required columns with low confidence");
		for (auto &expr : node.groups.group_expressions) {
			if (expr) {
				CollectColumnRefs(*expr, result.columns);
			}
		}
	}
	if (node.having) {
		notes.emplace_back("having expression included in required columns with low confidence");
		CollectColumnRefs(*node.having, result.columns);
	}
	if (node.qualify) {
		notes.emplace_back("qualify expression included in required columns with low confidence");
		CollectColumnRefs(*node.qualify, result.columns);
	}
	for (auto &modifier : node.modifiers) {
		if (!modifier) {
			continue;
		}
		if (modifier->type == ResultModifierType::ORDER_MODIFIER) {
			auto &order = modifier->Cast<OrderModifier>();
			for (auto &order_node : order.orders) {
				if (order_node.expression) {
					CollectColumnRefs(*order_node.expression, result.columns);
				}
			}
			notes.emplace_back("order expressions included in required columns with low confidence");
		} else if (modifier->type == ResultModifierType::DISTINCT_MODIFIER) {
			auto &distinct = modifier->Cast<DistinctModifier>();
			for (auto &target : distinct.distinct_on_targets) {
				if (target) {
					CollectColumnRefs(*target, result.columns);
				}
			}
			if (!distinct.distinct_on_targets.empty()) {
				notes.emplace_back("distinct expressions included in required columns with low confidence");
			}
		}
	}
	return result;
}

static void CollectJoinColumnRefs(const TableRef &ref, unordered_set<string> &columns) {
	if (ref.type != TableReferenceType::JOIN) {
		return;
	}
	auto &join_ref = ref.Cast<JoinRef>();
	if (join_ref.condition) {
		CollectColumnRefs(*join_ref.condition, columns);
	}
	for (auto &using_column : join_ref.using_columns) {
		columns.insert(NormalizeColumn(using_column));
	}
	for (auto &duplicate_eliminated_column : join_ref.duplicate_eliminated_columns) {
		if (duplicate_eliminated_column) {
			CollectColumnRefs(*duplicate_eliminated_column, columns);
		}
	}
	if (join_ref.left) {
		CollectJoinColumnRefs(*join_ref.left, columns);
	}
	if (join_ref.right) {
		CollectJoinColumnRefs(*join_ref.right, columns);
	}
}

static void MergeProjectionInfo(ProjectionInfo &target, const ProjectionInfo &source) {
	if (!source.known) {
		target.known = false;
		target.all_columns = true;
		target.columns.clear();
		return;
	}
	if (!target.known) {
		target = source;
		return;
	}
	if (target.all_columns || source.all_columns) {
		target.all_columns = true;
		target.columns.clear();
		return;
	}
	for (auto &column : source.columns) {
		target.columns.insert(column);
	}
}

static string StripConstant(string value) {
	value = TrimCopy(std::move(value));
	if (!value.empty() && value.back() == ';') {
		value.pop_back();
		value = TrimCopy(std::move(value));
	}
	auto typed_literal = std::regex(R"(^\s*(DATE|TIMESTAMP|TIME)\s+('.*')\s*$)", std::regex::icase);
	std::smatch typed_match;
	if (std::regex_match(value, typed_match, typed_literal)) {
		value = typed_match[2].str();
	}
	if (value.size() >= 2 && value.front() == '\'' && value.back() == '\'') {
		value = value.substr(1, value.size() - 2);
		string result;
		for (idx_t i = 0; i < value.size(); i++) {
			if (value[i] == '\'' && i + 1 < value.size() && value[i + 1] == '\'') {
				result += '\'';
				i++;
			} else {
				result += value[i];
			}
		}
		return result;
	}
	return value;
}

static bool TryGetColumnName(const ParsedExpression &expr, string &column_name) {
	if (expr.GetExpressionClass() != ExpressionClass::COLUMN_REF) {
		return false;
	}
	auto &column_ref = expr.Cast<ColumnRefExpression>();
	column_name = NormalizeColumn(column_ref.GetColumnName());
	return true;
}

static bool TryParseDouble(const string &value, double &result);

static bool IsNumericCastTargetName(const string &type_name) {
	return type_name == "tinyint" || type_name == "smallint" || type_name == "integer" || type_name == "int" ||
	       type_name == "bigint" || type_name == "hugeint" || type_name == "utinyint" || type_name == "usmallint" ||
	       type_name == "uinteger" || type_name == "ubigint" || type_name == "uhugeint" || type_name == "float" ||
	       type_name == "real" || type_name == "double" || StringUtil::StartsWith(type_name, "decimal");
}

static bool CastTargetCanUseLiteralString(const LogicalType &target_type, const string &value) {
	auto target_name = StringUtil::Lower(UnquoteIdentifier(target_type.ToString()));
	if (target_name == "blob" || target_name == "bytea" || target_name == "varbinary" || target_name == "binary" ||
	    target_name == "varchar" || target_name == "text" || target_name == "string") {
		return true;
	}
	if (IsNumericCastTargetName(target_name)) {
		double parsed_value;
		return TryParseDouble(value, parsed_value);
	}

	switch (target_type.id()) {
	case LogicalTypeId::BLOB:
	case LogicalTypeId::VARCHAR:
		return true;
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::HUGEINT:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::UBIGINT:
	case LogicalTypeId::UHUGEINT:
	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DOUBLE:
	case LogicalTypeId::DECIMAL: {
		double parsed_value;
		return TryParseDouble(value, parsed_value);
	}
	default:
		return false;
	}
}

static bool TryGetScalarLiteralValue(const ParsedExpression &expr, string &value) {
	if (expr.GetExpressionClass() == ExpressionClass::CONSTANT) {
		auto &constant = expr.Cast<ConstantExpression>();
		if (constant.value.type().id() == LogicalTypeId::BLOB) {
			value = StringValue::Get(constant.value);
		} else {
			value = StripConstant(constant.value.ToSQLString());
		}
		return true;
	}
	if (expr.GetExpressionClass() == ExpressionClass::CAST) {
		auto &cast = expr.Cast<CastExpression>();
		if (cast.try_cast || !cast.child) {
			return false;
		}
		string child_value;
		if (!TryGetScalarLiteralValue(*cast.child, child_value)) {
			return false;
		}
		if (!CastTargetCanUseLiteralString(cast.cast_type, child_value)) {
			return false;
		}
		value = std::move(child_value);
		return true;
	}
	return false;
}

static string OperatorString(ExpressionType type) {
	switch (type) {
	case ExpressionType::COMPARE_EQUAL:
		return "=";
	case ExpressionType::COMPARE_GREATERTHAN:
		return ">";
	case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
		return ">=";
	case ExpressionType::COMPARE_LESSTHAN:
		return "<";
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
		return "<=";
	default:
		return "";
	}
}

static string InvertOperator(const string &op) {
	if (op == ">") {
		return "<";
	}
	if (op == ">=") {
		return "<=";
	}
	if (op == "<") {
		return ">";
	}
	if (op == "<=") {
		return ">=";
	}
	return op;
}

static bool TryExtractPredicate(const ParsedExpression &expr, Predicate &predicate) {
	if (expr.GetExpressionClass() != ExpressionClass::COMPARISON) {
		return false;
	}
	auto op = OperatorString(expr.GetExpressionType());
	if (op.empty()) {
		return false;
	}
	auto &comparison = expr.Cast<ComparisonExpression>();
	if (!comparison.left || !comparison.right) {
		return false;
	}
	string column_name;
	string constant_value;
	if (TryGetColumnName(*comparison.left, column_name) && TryGetScalarLiteralValue(*comparison.right, constant_value)) {
		predicate.column = std::move(column_name);
		predicate.op = std::move(op);
		predicate.value = std::move(constant_value);
		return true;
	}
	if (TryGetScalarLiteralValue(*comparison.left, constant_value) && TryGetColumnName(*comparison.right, column_name)) {
		predicate.column = std::move(column_name);
		predicate.op = InvertOperator(op);
		predicate.value = std::move(constant_value);
		return true;
	}
	return false;
}

static void ExtractPredicates(const ParsedExpression &expr, vector<Predicate> &predicates, bool &complex_filter,
                              vector<string> &notes) {
	if (expr.GetExpressionClass() == ExpressionClass::CONJUNCTION &&
	    expr.GetExpressionType() == ExpressionType::CONJUNCTION_AND) {
		auto &conjunction = expr.Cast<ConjunctionExpression>();
		for (auto &child : conjunction.children) {
			if (child) {
				ExtractPredicates(*child, predicates, complex_filter, notes);
			}
		}
		return;
	}
	Predicate predicate;
	if (TryExtractPredicate(expr, predicate)) {
		predicates.push_back(std::move(predicate));
		return;
	}
	complex_filter = true;
	notes.emplace_back("filter not analyzable, assumed all row groups");
}

static void AnalyzeSelectNode(const SelectNode &node, ParsedQueryInfo &result, bool collect_predicates) {
	if (node.from_table) {
		ExtractScanSources(*node.from_table, result);
	}
	auto projection = ExtractProjectionInfo(node, result.notes);
	if (node.from_table) {
		CollectJoinColumnRefs(*node.from_table, projection.columns);
	}
	if (node.where_clause) {
		CollectColumnRefs(*node.where_clause, projection.columns);
		if (collect_predicates) {
			ExtractPredicates(*node.where_clause, result.predicates, result.complex_filter, result.notes);
		}
	}
	MergeProjectionInfo(result.projection, projection);
}

static void AnalyzeQueryNode(const QueryNode &node, ParsedQueryInfo &result, bool inside_set_operation = false) {
	switch (node.type) {
	case QueryNodeType::SELECT_NODE:
		AnalyzeSelectNode(node.Cast<SelectNode>(), result, !inside_set_operation);
		break;
	case QueryNodeType::SET_OPERATION_NODE: {
		auto &set_operation = node.Cast<SetOperationNode>();
		result.notes.emplace_back("set operation scan bytes estimated as sum of Parquet leaf scans");
		for (auto &child : set_operation.children) {
			if (child) {
				AnalyzeQueryNode(*child, result, true);
			}
		}
		break;
	}
	default:
		throw BinderException("dryrun could not analyze this query shape in v1");
	}
}

static ParsedQueryInfo ParseDryrunQuery(ClientContext &context, const string &sql) {
	Parser parser(context.GetParserOptions());
	parser.ParseQuery(sql);
	if (parser.statements.size() != 1) {
		throw BinderException("dryrun only supports exactly one SQL statement");
	}
	if (parser.statements[0]->type != StatementType::SELECT_STATEMENT) {
		throw BinderException("dryrun only supports read-only SELECT queries");
	}

	ParsedQueryInfo result;
	auto &statement = parser.statements[0]->Cast<SelectStatement>();
	if (!statement.node) {
		throw BinderException("dryrun could not analyze this query shape in v1");
	}
	AnalyzeQueryNode(*statement.node, result);
	if (result.paths.empty()) {
		auto lower_sql = StringUtil::Lower(sql);
		if (StringUtil::Contains(lower_sql, "read_csv") || StringUtil::Contains(lower_sql, "read_json")) {
			throw BinderException("dryrun only supports Parquet scans in v1");
		}
		throw BinderException("dryrun v1 requires Parquet file paths in the query text");
	}
	return result;
}

static bool TryParseDouble(const string &value, double &result) {
	char *end = nullptr;
	result = std::strtod(value.c_str(), &end);
	return end != value.c_str() && *end == '\0';
}

static int CompareStatsValue(const string &left, const string &right, bool &comparable) {
	double left_double;
	double right_double;
	auto left_is_double = TryParseDouble(left, left_double);
	auto right_is_double = TryParseDouble(right, right_double);
	if (left_is_double && right_is_double) {
		comparable = true;
		if (left_double < right_double) {
			return -1;
		}
		if (left_double > right_double) {
			return 1;
		}
		return 0;
	}
	if (left_is_double != right_is_double) {
		comparable = false;
		return 0;
	}
	comparable = true;
	if (left < right) {
		return -1;
	}
	if (left > right) {
		return 1;
	}
	return 0;
}

static bool PredicatePrunesRowGroup(const Predicate &predicate, const ColumnStats &stats, bool &usable) {
	usable = false;
	if (!stats.has_min || !stats.has_max) {
		return false;
	}
	bool min_comparable = false;
	bool max_comparable = false;
	auto min_cmp = CompareStatsValue(predicate.value, stats.min_value, min_comparable);
	auto max_cmp = CompareStatsValue(predicate.value, stats.max_value, max_comparable);
	if (!min_comparable || !max_comparable) {
		return false;
	}
	usable = true;
	if (predicate.op == "=") {
		return min_cmp < 0 || max_cmp > 0;
	}
	if (predicate.op == ">") {
		return max_cmp >= 0;
	}
	if (predicate.op == ">=") {
		return max_cmp > 0;
	}
	if (predicate.op == "<") {
		return min_cmp <= 0;
	}
	if (predicate.op == "<=") {
		return min_cmp < 0;
	}
	return false;
}

static string JoinNotes(const vector<string> &notes) {
	if (notes.empty()) {
		return "";
	}
	vector<string> unique_notes;
	for (auto &note : notes) {
		if (std::find(unique_notes.begin(), unique_notes.end(), note) == unique_notes.end()) {
			unique_notes.push_back(note);
		}
	}
	return StringUtil::Join(unique_notes, "; ");
}

static int64_t GetOptionalInt64(MaterializedQueryResult &result, idx_t column, idx_t row) {
	auto value = result.GetValue(column, row);
	if (value.IsNull()) {
		return 0;
	}
	return value.GetValue<int64_t>();
}

static int64_t GetOptionalUInt64AsInt64(MaterializedQueryResult &result, idx_t column, idx_t row) {
	auto value = result.GetValue(column, row);
	if (value.IsNull()) {
		return 0;
	}
	return UnsafeNumericCast<int64_t>(value.GetValue<uint64_t>());
}

static string GetOptionalString(MaterializedQueryResult &result, idx_t column, idx_t row) {
	auto value = result.GetValue(column, row);
	if (value.IsNull()) {
		return "";
	}
	return value.GetValue<string>();
}

static DryrunEstimate EstimateQuery(ClientContext &context, const ParsedQueryInfo &parsed) {
	DryrunEstimate estimate;
	estimate.notes = parsed.notes;
	if (!parsed.projection.known || parsed.complex_filter) {
		estimate.confidence = "low";
	} else if (!parsed.predicates.empty()) {
		estimate.confidence = "medium";
	} else if (!estimate.notes.empty()) {
		estimate.confidence = "low";
	}

	unordered_map<RowGroupKey, RowGroupEstimate, RowGroupKeyHash> row_groups;
	unordered_set<string> metadata_files;
	for (auto &path : parsed.paths) {
		Connection metadata_connection(*context.db);
		auto file_metadata_sql = "SELECT file_name, footer_size FROM parquet_file_metadata('" + EscapeSQLString(path) + "')";
		auto file_metadata = metadata_connection.Query(file_metadata_sql);
		if (file_metadata->HasError()) {
			throw BinderException("dryrun only supports Parquet scans in v1: %s", file_metadata->GetError());
		}
		for (idx_t row = 0; row < file_metadata->RowCount(); row++) {
			auto file_name = file_metadata->GetValue(0, row).GetValue<string>();
			if (metadata_files.insert(file_name).second) {
				estimate.estimated_metadata_bytes += GetOptionalUInt64AsInt64(*file_metadata, 1, row) + 8;
			}
		}

		auto metadata_sql = "SELECT file_name, row_group_id, path_in_schema, total_compressed_size, "
		                    "total_uncompressed_size, stats_min_value, stats_max_value, stats_min, stats_max "
		                    "FROM parquet_metadata('" +
		                    EscapeSQLString(path) + "')";
		auto metadata = metadata_connection.Query(metadata_sql);
		if (metadata->HasError()) {
			throw BinderException("dryrun only supports Parquet scans in v1: %s", metadata->GetError());
		}
		for (idx_t row = 0; row < metadata->RowCount(); row++) {
			RowGroupKey key;
			key.file_name = metadata->GetValue(0, row).GetValue<string>();
			key.row_group_id = metadata->GetValue(1, row).GetValue<int64_t>();
			auto column_name = NormalizeColumn(metadata->GetValue(2, row).GetValue<string>());
			auto &row_group = row_groups[key];
			row_group.file_name = key.file_name;
			row_group.row_group_id = key.row_group_id;

			bool include_column = parsed.projection.all_columns ||
			                      parsed.projection.columns.find(column_name) != parsed.projection.columns.end();
			if (include_column) {
				row_group.compressed_bytes += GetOptionalInt64(*metadata, 3, row);
				row_group.uncompressed_bytes += GetOptionalInt64(*metadata, 4, row);
			}

			ColumnStats stats;
			stats.min_value = GetOptionalString(*metadata, 5, row);
			stats.max_value = GetOptionalString(*metadata, 6, row);
			if (stats.min_value.empty()) {
				stats.min_value = GetOptionalString(*metadata, 7, row);
			}
			if (stats.max_value.empty()) {
				stats.max_value = GetOptionalString(*metadata, 8, row);
			}
			stats.has_min = !stats.min_value.empty();
			stats.has_max = !stats.max_value.empty();
			row_group.stats[column_name] = std::move(stats);
		}
	}

	unordered_set<string> scanned_files;
	bool filter_stats_missing = false;
	estimate.total_row_groups = UnsafeNumericCast<int64_t>(row_groups.size());
	for (auto &entry : row_groups) {
		auto &row_group = entry.second;
		bool pruned = false;
		if (!parsed.complex_filter) {
			for (auto &predicate : parsed.predicates) {
				auto stats_entry = row_group.stats.find(predicate.column);
				if (stats_entry == row_group.stats.end()) {
					filter_stats_missing = true;
					continue;
				}
				bool usable = false;
				if (PredicatePrunesRowGroup(predicate, stats_entry->second, usable)) {
					pruned = true;
					break;
				}
				if (!usable) {
					filter_stats_missing = true;
				}
			}
		}
		if (pruned) {
			continue;
		}
		estimate.estimated_compressed_bytes += row_group.compressed_bytes;
		estimate.estimated_uncompressed_bytes += row_group.uncompressed_bytes;
		estimate.estimated_row_groups++;
		scanned_files.insert(row_group.file_name);
	}

	if (filter_stats_missing && !parsed.predicates.empty()) {
		estimate.notes.emplace_back("some filter stats unavailable, assumed matching row groups");
		if (estimate.confidence == "high") {
			estimate.confidence = "medium";
		}
	}

	estimate.estimated_files = UnsafeNumericCast<int64_t>(scanned_files.size());
	estimate.estimated_compute_bytes = estimate.estimated_compressed_bytes;
	if (estimate.notes.empty()) {
		estimate.notes.emplace_back("estimated_compute_bytes equals estimated_compressed_bytes in v1");
	}
	return estimate;
}

static unique_ptr<FunctionData> DryrunBind(ClientContext &context, TableFunctionBindInput &input,
                                           vector<LogicalType> &return_types, vector<string> &names) {
	names.emplace_back("estimated_compute_bytes");
	return_types.emplace_back(LogicalType::BIGINT);
	names.emplace_back("estimated_compressed_bytes");
	return_types.emplace_back(LogicalType::BIGINT);
	names.emplace_back("estimated_uncompressed_bytes");
	return_types.emplace_back(LogicalType::BIGINT);
	names.emplace_back("estimated_files");
	return_types.emplace_back(LogicalType::BIGINT);
	names.emplace_back("estimated_row_groups");
	return_types.emplace_back(LogicalType::BIGINT);
	names.emplace_back("confidence");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("notes");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("estimated_metadata_bytes");
	return_types.emplace_back(LogicalType::BIGINT);
	names.emplace_back("total_row_groups");
	return_types.emplace_back(LogicalType::BIGINT);

	if (input.inputs.size() != 1 || input.inputs[0].IsNull()) {
		throw BinderException("dryrun requires a non-NULL constant SQL string");
	}
	auto sql = StringValue::Get(input.inputs[0]);
	auto parsed = ParseDryrunQuery(context, sql);
	auto bind_data = make_uniq<DryrunBindData>();
	bind_data->estimate = EstimateQuery(context, parsed);
	return std::move(bind_data);
}

struct DryrunGlobalState : public GlobalTableFunctionState {
	bool emitted = false;
};

static unique_ptr<GlobalTableFunctionState> DryrunInit(ClientContext &context, TableFunctionInitInput &input) {
	return make_uniq<DryrunGlobalState>();
}

static void DryrunFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &state = data_p.global_state->Cast<DryrunGlobalState>();
	if (state.emitted) {
		return;
	}
	auto &bind_data = data_p.bind_data->Cast<DryrunBindData>();
	auto &estimate = bind_data.estimate;
	output.SetValue(0, 0, Value::BIGINT(estimate.estimated_compute_bytes));
	output.SetValue(1, 0, Value::BIGINT(estimate.estimated_compressed_bytes));
	output.SetValue(2, 0, Value::BIGINT(estimate.estimated_uncompressed_bytes));
	output.SetValue(3, 0, Value::BIGINT(estimate.estimated_files));
	output.SetValue(4, 0, Value::BIGINT(estimate.estimated_row_groups));
	output.SetValue(5, 0, Value(estimate.confidence));
	output.SetValue(6, 0, Value(JoinNotes(estimate.notes)));
	output.SetValue(7, 0, Value::BIGINT(estimate.estimated_metadata_bytes));
	output.SetValue(8, 0, Value::BIGINT(estimate.total_row_groups));
	output.SetCardinality(1);
	state.emitted = true;
}

} // namespace

static void LoadInternal(ExtensionLoader &loader) {
	TableFunction dryrun("dryrun", {LogicalType::VARCHAR}, DryrunFunction, DryrunBind, DryrunInit);
	loader.RegisterFunction(dryrun);
}

void DryrunExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string DryrunExtension::Name() {
	return "dryrun";
}

std::string DryrunExtension::Version() const {
#ifdef EXT_VERSION_DRYRUN
	return EXT_VERSION_DRYRUN;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(dryrun, loader) {
	duckdb::LoadInternal(loader);
}
}
