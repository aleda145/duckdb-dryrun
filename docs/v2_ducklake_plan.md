# V2 DuckLake Plan

Goal:
Extend dryrun from explicit Parquet-path scans to DuckLake table scans while preserving the V1 non-execution guarantee and conservative byte semantics.

## Background

DuckLake stores table data in Parquet files and stores metadata in a SQL catalog database. The DuckLake 1.0 specification describes two main components: a catalog database and Parquet data storage. The DuckLake extension exposes metadata such as table files through `ducklake_list_files`, including data file path, size, and footer size. The specification also defines catalog tables such as `ducklake_data_file` and `ducklake_file_column_stats`, including file size, footer size, record count, and column-level stats.

Primary references:

- https://ducklake.select/docs/stable/specification/introduction
- https://ducklake.select/docs/stable/duckdb/usage/connecting
- https://ducklake.select/docs/stable/duckdb/metadata/list_files
- https://ducklake.select/docs/stable/specification/tables/ducklake_data_file
- https://ducklake.select/docs/stable/specification/tables/ducklake_file_column_stats

## V2 Principle

Treat DuckLake as a source resolver plus metadata provider:

```text
SQL table reference
  -> DuckLake catalog/table/snapshot resolution
  -> active data files and delete files
  -> current Parquet leaf estimator
```

The estimator should not execute the target query. It may issue metadata queries against DuckLake catalog functions/tables, analogous to how V1 uses `parquet_metadata(...)`.

## Initial Scope

Support:

- Read-only `SELECT`.
- Table references to attached DuckLake tables.
- Explicit schema/table references where the catalog can be identified.
- Projection and filter column extraction using the existing AST helpers.
- File-level estimates from DuckLake metadata.
- Optional Parquet row-group estimates when the data files are readable through the active filesystem.

Defer:

- DML, `MERGE`, `UPDATE`, `DELETE`, and writes.
- Exact delete-file application.
- Complex snapshot/time-travel syntax until table-reference parsing is understood.
- Catalog table scans that are not DuckLake user tables.
- Multi-engine semantics outside DuckDB's DuckLake extension.

## Output Model

V2 should preserve the existing columns:

- `estimated_compressed_bytes`
- `estimated_uncompressed_bytes`
- `estimated_compute_bytes`
- `estimated_files`
- `estimated_row_groups`
- `total_row_groups`
- `estimated_metadata_bytes`
- `confidence`
- `notes`

Possible additions once behavior is stable:

- `estimated_delete_file_bytes`
- `estimated_catalog_metadata_bytes`
- `source_type` or `source_summary`
- `snapshot_id` when available

Avoid adding too many columns before the first DuckLake prototype. Notes may be enough for early V2 experiments.

## Design Phases

### Phase 1: Detect DuckLake Table Sources

1. Parse table references from the AST.
2. Identify catalog/schema/table names for non-Parquet table refs.
3. Determine whether the catalog is a DuckLake catalog.

Open implementation question:

- Is there a stable DuckDB catalog API exposed to extensions for identifying an attached DuckLake catalog, or should V2 start with explicit user syntax/options?

Fallback option for the prototype:

```sql
SELECT * FROM dryrun('SELECT ...', source_type => 'ducklake')
```

or a new function:

```sql
SELECT * FROM dryrun_ducklake('catalog', 'schema', 'table', 'SELECT ...')
```

Prefer automatic detection only if it is robust.

### Phase 2: Resolve Active Files

Use DuckLake metadata to list active files for each table reference.

Candidate API:

```sql
FROM ducklake_list_files('catalog', 'table_name', schema => 'main')
```

The result includes:

- `data_file`
- `data_file_size_bytes`
- `data_file_footer_size`
- `delete_file`
- `delete_file_size_bytes`
- `delete_file_footer_size`

This can immediately support:

- total file count,
- footer metadata bytes,
- conservative full-file bytes when column-level stats are not yet used.

### Phase 3: Reuse Parquet Leaf Estimation

For each active data file:

1. Resolve the final path using DuckLake path rules and data path.
2. Run the existing Parquet metadata estimator on the file list.
3. Apply existing required-column and row-group pruning logic.
4. Sum estimates across files.

This gives a direct path to parity with V1 for DuckLake tables whose files are reachable.

### Phase 4: Catalog-Only Estimates

When data files are not reachable, use DuckLake catalog stats:

- `ducklake_data_file.file_size_bytes`
- `ducklake_data_file.footer_size`
- `ducklake_file_column_stats.column_size_bytes`
- `ducklake_file_column_stats.min_value`
- `ducklake_file_column_stats.max_value`

This mode should be clearly marked in notes because it estimates file-level or column-level bytes from DuckLake metadata rather than reading Parquet footers directly.

Confidence suggestion:

- `medium` if required columns and stats are available from DuckLake metadata.
- `low` if only file-level data is available.

### Phase 5: Delete Files

DuckLake can associate delete files with data files. V2 should initially report delete-file bytes separately or add notes rather than blending them into data-column bytes.

Initial behavior:

- Include delete-file footer bytes in metadata bytes if listed.
- Add `notes`: `DuckLake delete files present; delete application bytes not modeled`.
- Keep confidence `low` or `medium` depending on whether delete files materially affect the read plan.

Later:

- Add `estimated_delete_file_bytes`.
- Model equality/position delete file reads if DuckDB exposes enough metadata.

## Test Plan

Local generated DuckLake fixture:

1. Create a small DuckLake catalog.
2. Insert data into multiple Parquet files.
3. Ensure at least one filter prunes files or row groups.
4. Validate dryrun output against DuckLake file metadata and Parquet metadata.

Core tests:

- Full DuckLake table scan.
- Single-column projection.
- `COUNT(*)`.
- Simple numeric filter.
- Query with delete files, marked with notes.
- Missing or unreachable data path produces a clear error or catalog-only fallback.

Benchmarks:

- Local DuckLake fixture with no network.
- Optional remote/object-storage DuckLake example once a stable public fixture exists.

## Open Questions

- Should V2 expose a new table function for DuckLake, or keep one `dryrun(sql)` entry point?
- Can dryrun reliably detect DuckLake catalogs through DuckDB extension APIs?
- How should time travel and snapshots be represented in dryrun output?
- Should DuckLake catalog-only estimates be allowed when Parquet files are inaccessible?
- How should delete files affect confidence and byte accounting?

## Suggested First Milestone

Build the smallest end-to-end prototype:

```sql
INSTALL ducklake;
ATTACH 'ducklake:metadata.ducklake' AS lake;
USE lake;

SELECT * FROM dryrun('SELECT price FROM main.sales WHERE price > 100');
```

Acceptance criteria:

- dryrun detects `lake.main.sales` as a DuckLake table or accepts an explicit DuckLake prototype API.
- dryrun lists active data files.
- dryrun returns a conservative byte estimate without executing the target query.
- output includes notes describing DuckLake source resolution.

