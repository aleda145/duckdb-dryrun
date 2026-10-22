# DuckDB 2.0 target

The extension source and distribution workflow target DuckDB `v2.0.0` and
`extension-ci-tools` branch `v2.0-cyanoptera`. Use CMake 3.14 or newer and a
C++17 compiler.

Before committing this upgrade, update and stage the submodules yourself:

```sh
git -C duckdb fetch origin tag v2.0.0
git -C duckdb checkout v2.0.0
git -C extension-ci-tools fetch origin v2.0-cyanoptera
git -C extension-ci-tools checkout --detach origin/v2.0-cyanoptera
git add duckdb extension-ci-tools
```

The code quality workflow uses the checked-in DuckDB submodule; its deprecated
`duckdb_version` input does not switch versions. The distribution workflow
explicitly builds against `v2.0.0`.

The browser demo still uses its bundled DuckDB 1.5.1 runtime and extension.
The 2.0 Wasm build is copied to `web/public/extensions/v2.0.0/wasm_eh` and
requires a matching DuckDB-Wasm runtime before the demo can load it. Do not
replace the existing 1.5.1 artifact with a 2.0 binary.

# Extension updating
When cloning this template, the target version of DuckDB should be the latest stable release of DuckDB. However, there 
will inevitably come a time when a new DuckDB is released and the extension repository needs updating. This process goes
as follows:

- Bump submodules
  - `./duckdb` should be set to latest tagged release
  - `./extension-ci-tools` should be set to updated branch corresponding to latest DuckDB release. So if you're building for DuckDB `v1.1.0` there will be a branch in `extension-ci-tools` named `v1.1.0` to which you should check out. 
- Bump versions in `./github/workflows`
  - `duckdb_version` input in `duckdb-stable-build` job in `MainDistributionPipeline.yml` should be set to latest tagged release
  - `duckdb_version` input in `duckdb-stable-deploy` job in `MainDistributionPipeline.yml` should be set to latest tagged release
  - the reusable workflow `duckdb/extension-ci-tools/.github/workflows/_extension_distribution.yml` for the `duckdb-stable-build` job should be set to latest tagged release

# API changes
DuckDB extensions built with this extension template are built against the internal C++ API of DuckDB. This API is not guaranteed to be stable.
What this means for extension development is that when updating your extensions DuckDB target version using the above steps, you may run into the fact that your extension no longer builds properly.

Currently, DuckDB does not (yet) provide a specific change log for these API changes, but it is generally not too hard to figure out what has changed.

For figuring out how and why the C++ API changed, we recommend using the following resources:
- DuckDB's [Release Notes](https://github.com/duckdb/duckdb/releases)
- DuckDB's history of [Core extension patches](https://github.com/duckdb/duckdb/commits/main/.github/patches/extensions)
- The git history of the relevant C++ Header file of the API that has changed
