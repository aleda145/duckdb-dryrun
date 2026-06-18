# DuckDB Dryrun Web Demo

React/Vite app for demonstrating the `dryrun` DuckDB extension in the browser.

## Development

```sh
npm install
npm run dev
```

## Extension artifacts

The app uses `@duckdb/duckdb-wasm@1.33.1-dev45.0`, which reports DuckDB
`v1.5.1`. It intentionally targets only the modern exception-handling WASM
runtime, so build one matching side-module artifact:

```text
public/extensions/v1.5.1/wasm_eh/dryrun.duckdb_extension.wasm
```

From the repo root, after activating Emscripten:

```sh
git -C duckdb checkout v1.5.1

# Use the same Emscripten version as @duckdb/duckdb-wasm@1.33.1-dev45.0.
# Do not use `latest`; mismatched LLVM/libc++ versions can fail at LOAD time.
cd ~/dev/emsdk
./emsdk install 3.1.71
./emsdk activate 3.1.71
source ~/dev/emsdk/emsdk_env.sh

cd /path/to/duckdb-dryrun
rm -rf build/wasm_eh build/extension_configuration
make wasm_eh

cp build/wasm_eh/repository/v1.5.1/wasm_eh/dryrun.duckdb_extension.wasm \
  web/public/extensions/v1.5.1/wasm_eh/
```

The native `dryrun.duckdb_extension` cannot be used by DuckDB-Wasm.

If DuckDB reports `need to see wasm magic number`, the served file is not raw
WebAssembly. Check that the URL returns `dryrun.duckdb_extension.wasm` bytes,
not Vite's `index.html`, a native extension, or a compressed artifact without
the correct `Content-Encoding` header.

## Cloudflare Pages

```sh
npm run build
npm run deploy
```

Wrangler is configured with `pages_build_output_dir` set to `./dist`.
