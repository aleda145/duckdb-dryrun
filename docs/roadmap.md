# Dryrun Roadmap

This roadmap lists planned work after the current V1 Parquet scan-byte estimator.

## 0. Existing Issues

Known issues to keep visible while planning follow-up work:

- Join predicates and filters are not yet attributed to individual scan aliases.
- Many simple join cases are estimated well but still reported with `low` confidence.
- Local Parquet profiling is hard to compare directly with dryrun bytes because filesystem caching and OS reads affect DuckDB's profiled byte signal.
- HTTP/HTTPS glob expansion is rejected; callers must pass explicit URLs or `read_parquet([...])`.
- Catalog table scans are not supported unless the underlying Parquet paths are visible in the SQL.

## 1. Join Attribution And Confidence

Track scan aliases and source ownership through joins.

Goals:

- Attribute projected columns, filter columns, and join keys to the correct Parquet scan.
- Apply predicates only to the scan source they reference.
- Estimate dimension-side filters more accurately.
- Move simple analyzable equi-join cases from `low` to `medium` confidence where appropriate.
- Keep complex joins, non-equality predicates, and partially understood join shapes conservative.

## 2. DuckLake Support

Extend dryrun from explicit Parquet paths to DuckLake table scans.

Goals:

- Resolve DuckLake catalog table references to active Parquet data files.
- Read DuckLake file and column statistics when available.
- Preserve the non-execution guarantee.
- Report the same public fields as Parquet scans: bytes, metadata bytes, files, row groups, confidence, and notes.
- Handle snapshots/time travel explicitly instead of guessing.

See `v2_ducklake_plan.md`.

## 3. Native DuckDB Table Support

Estimate scans over local DuckDB tables without requiring visible Parquet paths.

Goals:

- Resolve catalog table scans to DuckDB storage metadata.
- Map DuckDB storage segments or row groups to the existing row-group reporting model where possible.
- Estimate table-scan bytes conservatively.
- Clearly report when native storage metadata cannot support a precise estimate.

See `v3_duckdb_storage_plan.md`.

## 4. Better Local Parquet Profiling

Improve validation for local Parquet files.

Goals:

- Separate estimated scan bytes from local filesystem/cache effects.
- Make local profiling comparisons easier to interpret.
- Keep benchmark output useful for both local and remote sources.
- Document where local profiling cannot be compared directly to remote network I/O.

## 5. Richer Predicate Support

Support more pruning-safe predicate shapes.

Goals:

- Support `IN` predicates when all values are scalar literals.
- Support `BETWEEN` as a pair of range predicates.
- Support simple casts around compatible scalar literals.
- Keep unsupported functions and expression predicates conservative.

## 6. Explainability

Make estimates easier to debug without complicating the default result.

Goals:

- Add an optional detailed/explain mode.
- Report selected scan sources, required columns, and pruning decisions.
- Keep the default `dryrun(sql)` result compact.
