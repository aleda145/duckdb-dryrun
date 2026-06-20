# Validation And Benchmarking Plan

Goal:
Create a repeatable way to answer whether dryrun estimates track actual DuckDB scan behavior closely enough to be useful.

Validation should separate three signals:

- Parquet metadata math: whether dryrun sums the intended column chunks and row groups.
- DuckDB profiling bytes: whether the estimate is directionally aligned with bytes DuckDB reports while executing comparable queries.
- User-facing interpretation: whether docs explain known differences between metadata bytes and profiled bytes.

## Principles

- Do not use benchmarks as correctness tests for exact byte equality.
- Use Parquet metadata comparisons for exact unit-style checks.
- Use DuckDB profiling comparisons for directional validation.
- Keep local validation independent from network access.
- Keep remote validation available as a manual or scheduled check.

## Local Validation

Use checked-in or generated Parquet files to cover:

- Full scan.
- Single-column projection.
- Multi-column projection.
- `COUNT(*)`.
- Simple filter that prunes row groups.
- Simple filter that cannot prune because stats are unavailable.
- Complex filter that still requires filter columns.
- Limit, order, aggregate, and expression projections.

Expected checks:

- `dryrun.estimated_compressed_bytes` equals `sum(total_compressed_size)` from `parquet_metadata(...)` for the expected required columns and row groups.
- `dryrun.estimated_uncompressed_bytes` equals `sum(total_uncompressed_size)` for the same chunks.
- Filter pruning changes row-group count only when supported stats make that safe.
- `LIMIT` does not reduce column-chunk bytes unless a future implementation can prove DuckDB avoids those reads.

## Profiling Validation

For each representative query:

1. Run `dryrun(query)`.
2. Execute the actual query with DuckDB JSON profiling enabled.
3. Extract `total_bytes_read`.
4. Compare the ratio `profiled_bytes / dryrun_bytes`.
5. Record confidence, notes, and query shape.

Expected interpretation:

- Exact equality is not required.
- Projection estimates should usually be below full-scan estimates.
- Filter estimates should be directionally lower when row groups are pruned.
- Large divergences should produce either a bug report or a documented limitation.

## Remote Validation

Keep remote benchmarks manual by default because they depend on network, remote server behavior, DuckDB `httpfs`, and caching.

Representative remote cases:

- Small public Parquet full scan.
- Small public Parquet single-column projection.
- Larger remote Parquet full scan.
- Larger remote Parquet projection.
- Remote `COUNT(*)`.
- Remote `COUNT(*) WHERE b = 1`.
- Remote complex filter.

Use `scripts/benchmark.py` as the entry point once the project has been built locally.

## Benchmark Output Improvements

Enhance `scripts/benchmark.py` to show:

- `estimated_compressed_bytes`
- `estimated_uncompressed_bytes`
- `estimated_metadata_bytes`
- `total_row_groups`
- `profiled_total_bytes_read`
- ratio
- confidence
- notes
- source URLs
- DuckDB version
- extension version or git commit

Add a `--no-profile` mode to print dryrun-only results for remote URLs when executing the full query would be too expensive.

Add a `--json` mode so benchmark output can be archived by CI or a manual release checklist.

## CI Strategy

Fast CI:

- Run SQLLogic tests over generated local Parquet.
- Avoid network.
- Avoid large benchmark datasets.

Optional scheduled/manual CI:

- Run remote benchmarks.
- Upload JSON benchmark output as an artifact.
- Fail only on script errors, not on ratio drift until thresholds are proven stable.

## Implementation Sequence

1. Expand local SQLLogic tests to cover AST-era required-column cases.
2. Add a smaller generated Parquet fixture for fast required-column tests.
3. Keep one moderately realistic checked-in Parquet fixture if it pays for itself.
4. Add benchmark script JSON output.
5. Add benchmark summary docs with example output.
6. Decide whether any remote benchmark belongs in GitHub Actions.

## Open Questions

- What ratio range is acceptable for each query class?
- How closely should `estimated_metadata_bytes` track DuckDB's observed HTTP range request size across local, remote, and multi-file scans?
- Should remote cache state be controlled or merely documented?
- Which public remote Parquet files are stable enough for long-term examples?
