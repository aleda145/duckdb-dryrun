# Typed Literal Predicate Plan

Goal:
Teach dryrun to recognize typed literal predicates such as `town = BLOB 'YORK'` as simple prunable predicates when the expression is still a column compared with a constant value.

## Motivation

The `house_prices_york_avg_price_by_date` benchmark currently executes much more efficiently than dryrun predicts:

```sql
SELECT date, avg(price) AS avg_price
FROM 'https://dryrun-data.dahl.dev/house_prices_all.parquet'
WHERE town = BLOB 'YORK'
GROUP BY date
ORDER BY date;
```

DuckDB can use Parquet statistics for `town`, but dryrun marks the filter as non-analyzable because `TryGetConstantValue` only accepts `ExpressionClass::CONSTANT`. The result is conservative but too noisy for a common query shape.

## Desired Behavior

For a predicate with a column on one side and a typed literal or safe literal cast on the other side:

```sql
town = BLOB 'YORK'
date = DATE '2024-01-01'
timestamp_col >= TIMESTAMP '2024-01-01 00:00:00'
int_col = CAST(42 AS INTEGER)
```

dryrun should:

- collect the filter column as required scan input,
- extract the literal value for Parquet min/max comparison,
- keep confidence at `medium` when pruning is attempted,
- only lower confidence when the expression is not safely reducible to a literal.

This should remain generic. Do not special-case `town`, `YORK`, or the house-prices benchmark.

## Non-Goals

- Do not evaluate arbitrary SQL expressions.
- Do not fold non-literal functions such as `lower('YORK')`, `concat(...)`, or runtime-dependent functions.
- Do not introduce DuckDB binding or target query execution.
- Do not claim exact semantic equivalence for collation, binary encodings, or complex nested types.

## Investigation Step

Before implementation, add a temporary local parser inspection or debugger breakpoint for these expressions:

```sql
WHERE town = BLOB 'YORK'
WHERE town = CAST('YORK' AS BLOB)
WHERE date = DATE '2024-01-01'
WHERE price = CAST(500000000 AS UINTEGER)
```

Record the `ExpressionClass`, `ExpressionType`, and `ToSQLString()` for the literal side. This determines which AST nodes need support.

Expected candidates:

- `ConstantExpression` for plain constants and some typed constants.
- Cast/operator expressions wrapping a constant.
- Function-like expression nodes for some typed literal syntaxes.

## Implementation Plan

1. Replace `TryGetConstantValue` with a slightly broader helper:

   ```text
   TryGetScalarLiteralValue(expr, value)
   ```

2. Keep the existing `ConstantExpression` behavior as the base case.

3. Add support for safe literal wrappers:

   - explicit casts where the child is a scalar literal,
   - typed literal forms that DuckDB parses as a constant or cast-like AST node,
   - no column references anywhere inside the literal-side expression.

4. Reuse `StripConstant` for SQL spelling cleanup, but extend it only where the parser inspection proves necessary.

5. If the literal side contains a column reference or non-literal child expression, return false and keep the current low-confidence fallback.

6. Keep `PredicatePrunesRowGroup` unchanged initially. It already compares strings or numeric strings and marks values unusable when comparison is unsafe.

## Test Plan

Add SQLLogic tests with generated Parquet data:

- `blob_col = BLOB 'YORK'` prunes row groups and reports `medium`.
- `blob_col = CAST('YORK' AS BLOB)` behaves the same if DuckDB accepts the query.
- `int_col = CAST(42 AS INTEGER)` remains prunable.
- `abs(int_col) = 42` remains low confidence and scans the referenced column.
- `blob_col = lower('YORK')` remains low confidence unless a later constant-folding phase is explicitly added.

Add remote benchmark expectation:

- `house_prices_york_avg_price_by_date` should move from low confidence and all row groups toward medium confidence and the row groups DuckDB can prove from Parquet stats.

## Acceptance Criteria

- No benchmark-specific SQL rewrites.
- Existing simple predicates still produce the same estimates.
- The house-prices York benchmark no longer scans all 51 row groups when the Parquet stats can prove fewer are possible.
- Tests prove typed literal support without relying on remote network access.

