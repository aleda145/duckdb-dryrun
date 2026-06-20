# DuckDB AST Parsing Plan

Goal:
Replace dryrun's current SQL text heuristics with DuckDB parser AST analysis so byte estimates are based on required scan columns, not only simple `SELECT <column>` string patterns.

This is needed for cases like:

```sql
SELECT COUNT(*) FROM 'https://dryrun.dahl.dev/gaia-5m.parquet' WHERE b = 1;
```

The correct scan-byte model is:

- projected data columns: none
- filter columns: `b`
- required scan columns: `b`
- row-group pruning: possible if Parquet stats for `b` are usable

The current string analyzer can classify this as an unknown projection and fall back to all columns.

## Principles

- Do not execute the target query.
- Use DuckDB's parser and AST types, not a custom SQL grammar.
- Keep byte estimation separate from filter-pruning confidence.
- Prefer conservative over precise-looking wrong estimates.
- Fall back explicitly with notes when AST shapes are unsupported.

## Target Model

Represent each parsed query with:

```text
scan_sources
  parquet paths or globs

required_columns
  columns DuckDB must read from Parquet data pages

projection_columns
  columns needed by SELECT expressions

filter_columns
  columns needed to evaluate WHERE

prunable_predicates
  simple column/operator/constant predicates usable with Parquet row-group stats

confidence
  high, medium, or low

notes
  fallback explanations
```

Important distinction:

- `required_columns` controls scanned column chunk bytes.
- `prunable_predicates` controls whether row groups can be skipped.

A complex predicate can still have useful `filter_columns` even if it is not prunable.

Example:

```sql
SELECT COUNT(*) FROM events.parquet WHERE abs(user_id) = 42;
```

Expected analysis:

- required columns: `user_id`
- prunable predicates: none
- confidence: low or medium-low
- note: filter expression not usable for row-group pruning

## AST Work Items

1. Parse and validate.

   Use DuckDB `Parser` to parse the input SQL and require exactly one `SELECT_STATEMENT`.

2. Find the query node.

   Start with simple `SelectNode` support. Reject or low-confidence fallback for set operations, recursive CTEs, subqueries, and other query-node shapes until explicitly supported.

3. Extract scan sources.

   Walk the `FROM` table references and identify:

   - direct Parquet replacement scans from string-literal table refs
   - `read_parquet(...)`
   - globs
   - multiple files

   Initially keep existing literal extraction as a fallback, but make AST source extraction the primary path.

4. Walk projection expressions.

   Collect column references needed by SELECT expressions:

   - `SELECT *`: all columns
   - `SELECT a, b`: `a`, `b`
   - `SELECT a + b`: `a`, `b`
   - `SELECT lower(name)`: `name`
   - `SELECT COUNT(*)`: no data columns
   - `SELECT COUNT(1)`: no data columns
   - `SELECT COUNT(a)`: `a`
   - `SELECT SUM(price)`: `price`

   Unsupported expressions should not automatically mean all columns. If column references can be collected from the expression tree, use them and lower confidence if needed.

5. Walk filter expressions.

   Collect all column references from `WHERE`, regardless of whether the filter is prunable.

   Then separately classify predicates usable for row-group pruning:

   - `column = constant`
   - `column > constant`
   - `column >= constant`
   - `column < constant`
   - `column <= constant`
   - top-level `AND` combinations of the above

   Unsupported filter expressions still contribute their referenced columns to `required_columns`, but do not contribute pruning predicates.

6. Combine required columns.

   ```text
   required_columns = projection_columns union filter_columns
   ```

   If projection is `*`, required columns are all columns regardless of filter columns.

7. Estimate Parquet bytes.

   Use `parquet_metadata(...)` as today:

   - sum selected column chunks for matching row groups
   - sum all columns for `SELECT *`
   - sum zero data-column bytes for metadata-only `COUNT(*)` with no filter columns

8. Assign confidence.

   Proposed rules:

   - `high`: scan sources and required columns are known, and no pruning estimate is needed.
   - `medium`: scan sources and required columns are known, and simple stat-based pruning is used.
   - `low`: required columns are known, but filters or table/query shape are not fully understood.
   - fallback low: scan all columns only when column extraction itself failed.

## Examples

```sql
SELECT COUNT(*) FROM 'events.parquet';
```

Expected:

- required columns: none
- estimated compressed bytes: `0`
- confidence: `high`

```sql
SELECT COUNT(*) FROM 'events.parquet' WHERE b = 1;
```

Expected:

- required columns: `b`
- pruning predicate: `b = 1`
- estimated compressed bytes: `b` chunks for unpruned row groups
- confidence: `medium`

```sql
SELECT payload FROM 'events.parquet' WHERE b = 1;
```

Expected:

- required columns: `payload`, `b`
- pruning predicate: `b = 1`
- estimated compressed bytes: `payload + b` chunks for unpruned row groups
- confidence: `medium`

```sql
SELECT COUNT(*) FROM 'events.parquet' WHERE abs(b) = 1;
```

Expected:

- required columns: `b`
- pruning predicate: none
- estimated compressed bytes: `b` chunks for all row groups
- confidence: `low`

## Implementation Sequence

1. Add internal AST traversal helpers.

   - `CollectColumnRefs(ParsedExpression &expr)`
   - `CollectProjectionColumns(SelectNode &node)`
   - `CollectFilterColumns(ParsedExpression &where)`
   - `ExtractPrunablePredicates(ParsedExpression &where)`

2. Keep existing string analyzer behind a fallback boundary.

   If AST traversal cannot handle a query shape, call the old analyzer and preserve current behavior.

3. Add targeted tests before broad rewrites.

   Start with:

   - `COUNT(*)`
   - `COUNT(*) WHERE simple_column = constant`
   - `COUNT(column)`
   - `SUM(column)`
   - expression projection such as `a + b`
   - complex filter such as `abs(b) = 1`
   - `SELECT payload WHERE b = 1` to verify filter columns are included in bytes
   - string literals containing `.parquet` outside `FROM`/`read_parquet(...)` are not scan sources
   - filter constants containing `.parquet` are not scan sources

4. Replace string projection extraction.

   Once tests pass, make AST projection extraction the default.

5. Replace string predicate extraction.

   Keep the same row-group pruning semantics, but source predicates from AST classification.

6. Remove or shrink old text heuristics.

   Only keep text fallback for recoverable Parquet paths if the AST table-ref path extraction is incomplete.

## Test Plan

Unit-style SQLLogic tests:

- Compare dryrun bytes to `parquet_metadata(...)` sums for expected columns.
- Compare `COUNT(*) WHERE b=1` to `SELECT b WHERE b=1`.
- Compare `SELECT payload WHERE b=1` to metadata sum of `payload + b`.
- Verify complex filters scan the referenced filter columns but do not claim row-group pruning.
- Verify unsupported query shapes lower confidence and include notes.
- Verify `SELECT 'events.parquet' AS value` does not estimate a file scan.
- Verify `WHERE name = 'other.parquet'` does not add `other.parquet` as a scan source.

Benchmark tests:

- Remote `SELECT *`.
- Remote single-column projection.
- Remote `COUNT(*)`.
- Remote `COUNT(*) WHERE b=1`.
- Remote complex filter.

Manual benchmark expectation:

```bash
make benchmark 'SELECT COUNT(*) FROM https://dryrun.dahl.dev/gaia-5m.parquet WHERE b=1'
```

After AST parsing, dryrun should estimate roughly the same byte class as DuckDB's profiled scan for column `b`, not the full file.

## Open Questions

- Should metadata-only `COUNT(*)` report `0` data bytes or a small metadata/footer byte estimate?
- Should confidence be split into `projection_confidence` and `filter_confidence` later?
- How much table-ref support is needed before catalog table scans are worth handling?
- Should joins be rejected initially, or should scan bytes be estimated independently per Parquet leaf?
