# V1 Contract Plan

Goal:
Make the V1 behavior precise enough that users know when `dryrun` is giving a useful estimate, when it is conservative, and when it refuses to estimate.

V1 should be sold as a Parquet scan-byte estimator, not a general compute predictor. `estimated_compute_bytes` can remain for UX compatibility, but in V1 it is an alias for `estimated_compressed_bytes`.

## Principles

- Do not execute the target query.
- Prefer explicit rejection over misleading precision.
- Prefer conservative estimates over underestimates when a query is partially understood.
- Make confidence explainable from the returned row.
- Keep output stable unless a schema change is intentionally versioned.

## Public Contract

Supported in V1:

- Exactly one SQL statement.
- Read-only `SELECT`.
- Leaf scans that are explicit Parquet paths, globs, or `read_parquet(...)`.
- Local Parquet and remote Parquet when DuckDB can read metadata through the active filesystem/extensions.
- Projection and filter analysis only for AST shapes that are explicitly supported.
- Row-group pruning only from Parquet min/max metadata for simple predicates.

Rejected in V1:

- Non-`SELECT` statements.
- Multiple statements.
- Non-Parquet files and table functions.
- Catalog table scans unless the underlying Parquet source is visible and intentionally supported.
- Queries where scan sources cannot be identified safely.

Low-confidence fallback:

- Unsupported projection expressions when referenced columns cannot be recovered.
- Unsupported filter expressions when referenced columns cannot be recovered.
- Query shapes that are partially analyzable but not fully understood.

## Output Semantics

- `estimated_compressed_bytes`: compressed Parquet column-chunk bytes expected to be read from data pages for the selected row groups and required columns.
- `estimated_uncompressed_bytes`: uncompressed Parquet column-chunk bytes for the same row groups and required columns.
- `estimated_compute_bytes`: V1 compatibility alias for `estimated_compressed_bytes`.
- `estimated_metadata_bytes`: estimated Parquet footer request bytes, reported separately from data-column bytes.
- `estimated_files`: number of files with at least one unpruned row group in the estimate.
- `estimated_row_groups`: number of row groups included after supported pruning.
- `total_row_groups`: total row groups across the scan sources before pruning.
- `confidence`: coarse quality signal, not a statistical probability.
- `notes`: concise explanation of fallbacks and assumptions.

Open decision:

- For metadata-only queries such as `COUNT(*)`, keep data-column bytes at `0` when no data columns are required and report footer cost through `estimated_metadata_bytes`.

## Confidence Rules

Proposed V1 rules:

- `high`: scan sources and required columns are known, and no unsupported expression or pruning fallback was needed.
- `medium`: scan sources and required columns are known, and simple stat-based pruning was used or some pruning stats were unavailable.
- `low`: scan sources are known, but some projection/filter/query shape is not fully understood.

Avoid using `high` when:

- A string or regex fallback was used for source extraction.
- Required columns are guessed.
- A predicate was ignored because it could not be represented.

## Error Behavior

Errors should be stable and action-oriented:

- `dryrun only supports exactly one SQL statement`
- `dryrun only supports read-only SELECT queries`
- `dryrun only supports Parquet scans in v1`
- `dryrun v1 requires explicit Parquet scan sources`
- `dryrun could not analyze this query shape in v1`

Keep internal DuckDB or filesystem errors attached when they explain why metadata could not be read.

## Non-Execution Guarantee

Add tests that prove the target query is not executed:

- A query with an invalid cast in a filter should return a fallback estimate, not raise the runtime cast error.
- A query with a function that would fail at execution time should not fail during dryrun if parsing/binding is not required.
- A query that would create side effects must be rejected before any execution path.

## Implementation Sequence

1. Document the V1 contract in `README.md`.
2. Move detailed support matrix into a docs page if the README gets too long.
3. Align error messages in `src/dryrun_extension.cpp` with the contract.
4. Split confidence assignment into a helper so rules are centralized.
5. Add SQLLogic tests for each supported, rejected, and low-confidence category.
6. Revisit output column names only after V1 behavior is stable.

## Test Plan

- Golden tests for each error message.
- Golden tests for confidence transitions.
- Tests where `estimated_compute_bytes = estimated_compressed_bytes`.
- Tests for metadata-only `COUNT(*)` once the open decision is resolved.
- Tests proving unsupported but safe query shapes either reject or return low confidence with notes.
