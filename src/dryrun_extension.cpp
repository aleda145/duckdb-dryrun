#define DUCKDB_EXTENSION_MAIN

#include "dryrun_extension.hpp"

#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/materialized_query_result.hpp"
#include "duckdb/parser/parser.hpp"

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
	int64_t estimated_files = 0;
	int64_t estimated_row_groups = 0;
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
		       estimate.estimated_files == other.estimate.estimated_files &&
		       estimate.estimated_row_groups == other.estimate.estimated_row_groups &&
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

static bool EndsWithCI(const string &value, const string &suffix) {
	auto lower_value = StringUtil::Lower(value);
	auto lower_suffix = StringUtil::Lower(suffix);
	return StringUtil::EndsWith(lower_value, lower_suffix);
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

static vector<string> SplitTopLevel(const string &input, char delimiter) {
	vector<string> result;
	idx_t start = 0;
	idx_t depth = 0;
	bool in_single_quote = false;
	bool in_double_quote = false;
	for (idx_t i = 0; i < input.size(); i++) {
		auto c = input[i];
		if (in_single_quote) {
			if (c == '\'' && i + 1 < input.size() && input[i + 1] == '\'') {
				i++;
			} else if (c == '\'') {
				in_single_quote = false;
			}
			continue;
		}
		if (in_double_quote) {
			if (c == '"' && i + 1 < input.size() && input[i + 1] == '"') {
				i++;
			} else if (c == '"') {
				in_double_quote = false;
			}
			continue;
		}
		if (c == '\'') {
			in_single_quote = true;
		} else if (c == '"') {
			in_double_quote = true;
		} else if (c == '(') {
			depth++;
		} else if (c == ')' && depth > 0) {
			depth--;
		} else if (c == delimiter && depth == 0) {
			result.push_back(TrimCopy(input.substr(start, i - start)));
			start = i + 1;
		}
	}
	result.push_back(TrimCopy(input.substr(start)));
	return result;
}

static optional_idx FindTopLevelKeyword(const string &input, const string &keyword, idx_t start_pos = 0) {
	auto lower_input = StringUtil::Lower(input);
	auto lower_keyword = StringUtil::Lower(keyword);
	idx_t depth = 0;
	bool in_single_quote = false;
	bool in_double_quote = false;
	for (idx_t i = start_pos; i + lower_keyword.size() <= lower_input.size(); i++) {
		auto c = lower_input[i];
		if (in_single_quote) {
			if (c == '\'' && i + 1 < lower_input.size() && lower_input[i + 1] == '\'') {
				i++;
			} else if (c == '\'') {
				in_single_quote = false;
			}
			continue;
		}
		if (in_double_quote) {
			if (c == '"' && i + 1 < lower_input.size() && lower_input[i + 1] == '"') {
				i++;
			} else if (c == '"') {
				in_double_quote = false;
			}
			continue;
		}
		if (c == '\'') {
			in_single_quote = true;
			continue;
		}
		if (c == '"') {
			in_double_quote = true;
			continue;
		}
		if (c == '(') {
			depth++;
			continue;
		}
		if (c == ')' && depth > 0) {
			depth--;
			continue;
		}
		if (depth != 0 || lower_input.compare(i, lower_keyword.size(), lower_keyword) != 0) {
			continue;
		}
		bool before_ok = i == 0 || !StringUtil::CharacterIsAlphaNumeric(lower_input[i - 1]);
		auto after_pos = i + lower_keyword.size();
		bool after_ok = after_pos == lower_input.size() || !StringUtil::CharacterIsAlphaNumeric(lower_input[after_pos]);
		if (before_ok && after_ok) {
			return optional_idx(i);
		}
	}
	return optional_idx();
}

static vector<string> ExtractStringLiterals(const string &sql) {
	vector<string> result;
	for (idx_t i = 0; i < sql.size(); i++) {
		if (sql[i] != '\'') {
			continue;
		}
		string literal;
		i++;
		for (; i < sql.size(); i++) {
			if (sql[i] == '\'' && i + 1 < sql.size() && sql[i + 1] == '\'') {
				literal += '\'';
				i++;
			} else if (sql[i] == '\'') {
				break;
			} else {
				literal += sql[i];
			}
		}
		result.push_back(std::move(literal));
	}
	return result;
}

static ProjectionInfo ExtractProjectionInfo(const string &sql, vector<string> &notes) {
	ProjectionInfo result;
	auto select_pos = FindTopLevelKeyword(sql, "select");
	auto from_pos = FindTopLevelKeyword(sql, "from");
	if (!select_pos.IsValid() || !from_pos.IsValid() || from_pos.GetIndex() <= select_pos.GetIndex()) {
		notes.emplace_back("projection not analyzable, assumed all columns");
		return result;
	}

	auto projection_text = sql.substr(select_pos.GetIndex() + 6, from_pos.GetIndex() - (select_pos.GetIndex() + 6));
	projection_text = TrimCopy(std::move(projection_text));
	if (projection_text == "*" || EndsWithCI(projection_text, ".*")) {
		result.known = true;
		result.all_columns = true;
		return result;
	}

	auto expressions = SplitTopLevel(projection_text, ',');
	result.known = true;
	result.all_columns = false;
	for (auto &expr : expressions) {
		auto lower_expr = StringUtil::Lower(expr);
		if (expr == "*" || EndsWithCI(expr, ".*") || StringUtil::Contains(expr, '(')) {
			result.known = false;
			result.all_columns = true;
			result.columns.clear();
			notes.emplace_back("projection not analyzable, assumed all columns");
			return result;
		}

		auto as_pos = FindTopLevelKeyword(expr, "as");
		if (as_pos.IsValid()) {
			expr = expr.substr(0, as_pos.GetIndex());
		} else {
			auto pieces = StringUtil::Split(expr, ' ');
			if (pieces.size() > 1) {
				expr = pieces[0];
			}
		}
		result.columns.insert(NormalizeColumn(expr));
	}
	return result;
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

static vector<Predicate> ExtractPredicates(const string &sql, bool &complex_filter, vector<string> &notes) {
	vector<Predicate> result;
	auto where_pos = FindTopLevelKeyword(sql, "where");
	if (!where_pos.IsValid()) {
		return result;
	}

	idx_t end_pos = sql.size();
	for (auto &keyword : {"group", "order", "limit", "offset", "union", "except", "intersect"}) {
		auto pos = FindTopLevelKeyword(sql, keyword, where_pos.GetIndex() + 5);
		if (pos.IsValid()) {
			end_pos = MinValue(end_pos, pos.GetIndex());
		}
	}
	auto where_text = TrimCopy(sql.substr(where_pos.GetIndex() + 5, end_pos - (where_pos.GetIndex() + 5)));
	if (where_text.empty()) {
		return result;
	}

	if (FindTopLevelKeyword(where_text, "or").IsValid()) {
		complex_filter = true;
		notes.emplace_back("filter not analyzable, assumed all row groups");
		return result;
	}

	static const std::regex predicate_regex(
	    R"(^\s*((?:"[^"]+"|[A-Za-z_][A-Za-z0-9_]*)(?:\.(?:"[^"]+"|[A-Za-z_][A-Za-z0-9_]*))?)\s*(=|>=|<=|>|<)\s*(.+?)\s*$)",
	    std::regex::icase);

	vector<string> parts;
	idx_t start = 0;
	while (true) {
		auto and_pos = FindTopLevelKeyword(where_text, "and", start);
		if (!and_pos.IsValid()) {
			parts.push_back(TrimCopy(where_text.substr(start)));
			break;
		}
		parts.push_back(TrimCopy(where_text.substr(start, and_pos.GetIndex() - start)));
		start = and_pos.GetIndex() + 3;
	}

	for (auto &part : parts) {
		std::smatch match;
		if (!std::regex_match(part, match, predicate_regex)) {
			complex_filter = true;
			notes.emplace_back("filter not analyzable, assumed all row groups");
			result.clear();
			return result;
		}
		Predicate predicate;
		predicate.column = NormalizeColumn(match[1].str());
		predicate.op = match[2].str();
		auto constant = TrimCopy(match[3].str());
		if (StringUtil::Contains(constant, '(') || StringUtil::Contains(constant, ')')) {
			complex_filter = true;
			notes.emplace_back("filter not analyzable, assumed all row groups");
			result.clear();
			return result;
		}
		predicate.value = StripConstant(std::move(constant));
		result.push_back(std::move(predicate));
	}
	return result;
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
	for (auto &literal : ExtractStringLiterals(sql)) {
		auto lower = StringUtil::Lower(literal);
		if (StringUtil::Contains(lower, ".parquet")) {
			result.paths.push_back(literal);
		} else if (StringUtil::Contains(lower, ".csv") || StringUtil::Contains(lower, ".json")) {
			throw BinderException("dryrun only supports Parquet scans in v1");
		}
	}
	if (result.paths.empty()) {
		auto lower_sql = StringUtil::Lower(sql);
		if (StringUtil::Contains(lower_sql, "read_csv") || StringUtil::Contains(lower_sql, "read_json")) {
			throw BinderException("dryrun only supports Parquet scans in v1");
		}
		throw BinderException("dryrun v1 requires Parquet file paths in the query text");
	}

	result.projection = ExtractProjectionInfo(sql, result.notes);
	result.predicates = ExtractPredicates(sql, result.complex_filter, result.notes);
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
	if (TryParseDouble(left, left_double) && TryParseDouble(right, right_double)) {
		comparable = true;
		if (left_double < right_double) {
			return -1;
		}
		if (left_double > right_double) {
			return 1;
		}
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
	}

	unordered_map<RowGroupKey, RowGroupEstimate, RowGroupKeyHash> row_groups;
	for (auto &path : parsed.paths) {
		Connection metadata_connection(*context.db);
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
