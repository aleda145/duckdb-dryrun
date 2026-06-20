# V3 DuckDB Storage Plan

Goal:
Extend dryrun from external Parquet and DuckLake scans to local DuckDB database tables, estimating bytes for queries over native DuckDB storage without executing the target query.

## Motivation

V1 estimates explicit Parquet scans. V2 is planned around DuckLake, where table metadata resolves to Parquet data files. A natural V3 is native DuckDB storage:

```sql
SELECT * FROM dryrun('SELECT price FROM local_table WHERE price > 100');
```

This would make dryrun useful for ordinary DuckDB databases, not only lake-style Parquet layouts.

## Principle

Treat DuckDB-native tables as a separate storage backend:

```text
SQL table reference
  -> DuckDB catalog table
  -> storage metadata / table statistics
  -> estimated columns, row groups/segments, and bytes
```

Do not force native DuckDB storage into the Parquet model. Reuse AST analysis for required columns and predicates, but build a backend-specific byte estimator.

## Initial Scope

Support:

- Local attached DuckDB databases.
- Persistent base tables.
- Read-only `SELECT`.
- Projection-aware estimates.
- Conservative table/column byte estimates from catalog or storage metadata available through DuckDB APIs.
- Notes explaining when estimates are storage-level rather than Parquet-footer-level.

Defer:

- Exact execution-plan I/O prediction.
- Index lookup modeling.
- Temporary tables and in-memory-only objects.
- Views and macros, unless expansion can be inspected safely.
- Attached non-DuckDB catalogs.
- Transaction visibility edge cases beyond the current connection snapshot.

## Discovery Questions

Before implementation, investigate available DuckDB APIs and SQL metadata:

- Can an extension access table storage statistics for a `TableCatalogEntry`?
- Are per-column compressed sizes exposed for native storage?
- Can row group or segment counts be retrieved without scanning?
- How does DuckDB expose zonemap/min/max stats for native storage?
- Are estimates stable across checkpointed and uncheckpointed data?
- What changes for attached databases versus the main database?

If no reliable public API exists, V3 should start with a conservative catalog-level estimate rather than private-storage introspection.

## Output Model

Preserve current dryrun columns:

- `estimated_compute_bytes`
- `estimated_compressed_bytes`
- `estimated_uncompressed_bytes`
- `estimated_files`
- `estimated_row_groups`
- `total_row_groups`
- `confidence`
- `notes`
- `estimated_metadata_bytes`

Interpretation changes for native DuckDB:

- `estimated_files`: likely `1` per database/table storage unit, or `0` if file attribution is not meaningful.
- `estimated_row_groups` / `total_row_groups`: map to native storage row groups/segments if available; otherwise report `0` and explain in notes.
- `estimated_metadata_bytes`: likely `0` unless catalog/storage metadata I/O is modeled separately.

Possible future additions:

- `source_type`
- `estimated_catalog_bytes`
- `estimated_index_bytes`
- `storage_backend`

## Design Phases

### Phase 1: Detect Native DuckDB Table Scans

1. Parse table refs using the existing AST traversal.
2. For non-Parquet base table refs, inspect the catalog entry.
3. Classify the source as:

   - explicit Parquet path,
   - DuckLake table,
   - native DuckDB table,
   - unsupported catalog object.

### Phase 2: Minimal Conservative Estimate

If only table-level metadata is available:

- estimate full table storage size for any query touching the table,
- include notes: `DuckDB native table estimate is table-level only`,
- confidence `low`.

This is useful as a first nonzero signal but not enough for V3 quality.

### Phase 3: Projection-Aware Estimate

If per-column storage metadata is available:

- use existing required-column extraction,
- sum required column storage sizes,
- keep `COUNT(*)` at zero data bytes only if DuckDB can answer it from metadata without reading table data,
- otherwise use a conservative table/column estimate.

### Phase 4: Predicate and Segment Pruning

If native storage exposes zonemap or segment stats:

- map simple predicates to segment pruning,
- report `estimated_row_groups / total_row_groups`,
- confidence `medium` when pruning is attempted and metadata is usable.

If stats are unavailable:

- include required filter columns,
- scan all native segments,
- confidence `medium` or `low` depending on how much storage metadata is known.

### Phase 5: Views and Replacement Scans

Only after base tables work:

- investigate view expansion without executing the target query,
- decide whether replacement scans can expose enough source metadata,
- keep unsupported objects rejected or low confidence.

## Test Plan

Generated local DuckDB database fixture:

1. Create a persistent table with multiple row groups/segments.
2. Add columns with different sizes.
3. Insert sorted data so simple predicates can prune if native stats are exposed.
4. Checkpoint where needed to make storage metadata stable.

Core tests:

- Full native table scan.
- Single-column projection.
- Multi-column projection.
- `COUNT(*)`.
- Simple numeric filter.
- Complex filter requiring a column but not pruning.
- Attached DuckDB database table.
- View rejected or marked low confidence with notes.

Benchmark validation:

- Compare dryrun to DuckDB profiling bytes for local native tables.
- Keep this separate from Parquet and DuckLake benchmarks because storage semantics differ.

## Risks

- DuckDB may not expose stable public storage metadata APIs for extensions.
- Native storage byte accounting may not map cleanly to compressed Parquet bytes.
- Profiling bytes may include buffer/cache behavior that makes ratios harder to interpret.
- `COUNT(*)` behavior can differ depending on metadata availability and storage state.

## Acceptance Criteria

V3 is useful when:

- dryrun accepts a simple query over a local DuckDB table,
- reports a conservative byte estimate without executing the query,
- distinguishes native DuckDB storage from Parquet/DuckLake in notes,
- uses projection information when reliable metadata is available,
- has tests proving unsupported native objects do not produce misleading precision.

