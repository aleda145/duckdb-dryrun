Build a very small DuckDB extension named `dryrun` using `duckdb/extension-template`.

Context:
We want a v1 DuckDB extension that estimates how much compute a query will probably use before running it. Scope is intentionally tiny: SELECT queries over Parquet files only. The first metric should be estimated Parquet scan bytes, not actual CPU time.

Environment:

* Linux
* DuckDB installed
* Working inside a repo created from `https://github.com/duckdb/extension-template`

Target UX:

```sql
INSTALL dryrun;
LOAD dryrun;

SELECT * FROM dryrun('SELECT * FROM events');
-- or
CALL dryrun('SELECT * FROM events');
```

V1 behavior:

* Add a table function `dryrun(sql VARCHAR)`.
* Only support a single read-only `SELECT`.
* Only support queries whose leaf scans are Parquet files/tables backed by Parquet.
* Return one row with at least:

  * `estimated_compute_bytes BIGINT`
  * `estimated_compressed_bytes BIGINT`
  * `estimated_uncompressed_bytes BIGINT`
  * `estimated_files BIGINT`
  * `estimated_row_groups BIGINT`
  * `confidence VARCHAR`
  * `notes VARCHAR`

Implementation approach:

1. Register a DuckDB table function called `dryrun`.
2. Validate input SQL:

   * must be constant string
   * must parse as exactly one statement
   * must be SELECT-only
   * reject INSERT/CREATE/COPY/EXPORT/ATTACH/PRAGMA/etc.
3. Generate a plan without executing:

   * use DuckDB planning APIs if practical
   * otherwise execute `EXPLAIN (FORMAT JSON) <query>` internally and parse the result
   * never use `EXPLAIN ANALYZE`
4. Identify scan sources:

   * find Parquet scan nodes / `read_parquet` / direct `'file.parquet'` scans
   * v1 may reject complex catalog table scans unless the source path is recoverable from the plan
5. For each Parquet file:

   * read footer/metadata only
   * use DuckDB’s existing Parquet metadata facilities where practical
   * collect row group count, row counts, compressed byte sizes, uncompressed byte sizes, and column names
6. Projection estimate:

   * if projected columns are known, sum only those column chunks
   * if projection cannot be determined, assume all columns and set confidence to `low`
7. Filter estimate:

   * v1 only handles simple row-group pruning using Parquet min/max/null stats
   * support simple predicates like `col = constant`, `col > constant`, `col >= constant`, `col < constant`, `col <= constant`, and simple ANDs
   * if filters are too complex, ignore filter pruning and add a note
8. Result:

   * `estimated_compute_bytes` should initially equal `estimated_compressed_bytes`
   * `estimated_uncompressed_bytes` should also be returned as a CPU/decode proxy
   * `confidence` should be `high`, `medium`, or `low`
   * `notes` should explain fallbacks, e.g. “filter not analyzable, assumed all row groups”

Non-goals for v1:

* no exact CPU prediction
* no wall-clock prediction
* no join/sort/group-by memory estimation
* no non-Parquet formats
* no remote billing model
* no custom SQL grammar
* no agent policy enforcement
* no executing the target query

Suggested test cases:

1. One local Parquet file, `SELECT *`.
2. One local Parquet file, projected columns only.
3. Multiple Parquet files via glob.
4. Simple filter that prunes at least one row group.
5. Complex filter that falls back to full scan with `confidence = low` or `medium`.
6. Query over CSV/JSON should fail with a clear “only Parquet supported in v1” error.
7. Non-SELECT query should fail.
8. Multiple SQL statements should fail.
9. Confirm dryrun does not execute the target query.

Build/test commands:

```bash
git submodule update --init --recursive
python3 ./scripts/bootstrap-template.py dryrun
make
make test
./build/release/duckdb
```

Inside DuckDB:

```sql
LOAD './build/release/extension/dryrun/dryrun.duckdb_extension';
SELECT * FROM dryrun('SELECT * FROM ''test/data/events.parquet''');
```

Deliverable:
A PR that replaces the template example function with the `dryrun` table function, adds SQL tests under `test/sql`, and includes a short README section documenting the v1 limitations and meaning of `estimated_compute_bytes`.
