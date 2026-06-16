# This file is included by DuckDB's build system. It specifies which extension to load

if(MSVC)
    # DuckDB v1.5.x vendors an older fmt version whose legacy _SECURE_SCL
    # checked-iterator branch does not compile with the VS 2026 MSVC toolchain.
    # Undefine it for this build so fmt uses the plain pointer fallback.
    add_compile_options("/U_SECURE_SCL")
endif()

# Extension from this repo
duckdb_extension_load(parquet)

duckdb_extension_load(dryrun
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
)

# Any extra extensions that should be built
# e.g.: duckdb_extension_load(json)
