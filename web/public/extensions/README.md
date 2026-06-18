# DuckDB extension artifacts

This directory is served as the DuckDB extension repository for the browser demo.

The first demo targets `@duckdb/duckdb-wasm@1.33.1-dev45.0`, whose DuckDB
runtime reports `v1.5.1`. It uses only the modern exception-handling WASM
runtime. Copy the matching side-module extension build here:

```text
v1.5.1/wasm_eh/dryrun.duckdb_extension.wasm
```

Do not put the native `dryrun.duckdb_extension` file here. DuckDB-Wasm can only
load the WASM side-module artifact.

If the app reports `need to see wasm magic number`, the URL DuckDB fetched did
not start with raw WebAssembly bytes. Common causes:

- the file is missing and the dev server returned `index.html`
- the native extension was copied instead of `dryrun.duckdb_extension.wasm`
- a Brotli/gzip-compressed artifact was copied without serving the matching
  `Content-Encoding` header

If the app reports a `LinkError` with an imported DuckDB C++ symbol, rebuild the
extension with the same Emscripten version as the DuckDB-Wasm runtime. For
`@duckdb/duckdb-wasm@1.33.1-dev45.0`, use Emscripten `3.1.71` and clean
`build/wasm_eh` before rebuilding.
