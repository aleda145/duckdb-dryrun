PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=dryrun
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile

BENCHMARK_ARGS := $(strip $(filter-out benchmark,$(MAKECMDGOALS)))
BENCHMARK_INPUT := $(if $(SQL),$(SQL),$(BENCHMARK_ARGS))
BENCHMARK_FIRST_ARG := $(firstword $(BENCHMARK_INPUT))
BENCHMARK_SQL_MODE := $(if $(SQL),1,$(filter SELECT select WITH with,$(BENCHMARK_FIRST_ARG)))
BENCHMARK_ARGS_FOR_PY := $(if $(BENCHMARK_SQL_MODE),"$(BENCHMARK_INPUT)",$(BENCHMARK_INPUT))
BENCHMARK_COLUMN_ARG := $(if $(COLUMN),--column $(COLUMN),)
CLANG_FORMAT ?= clang-format
CLANG_FORMAT_FILES := src/dryrun_extension.cpp src/include/dryrun_extension.hpp
EMSDK ?= $(HOME)/dev/emsdk
EMSDK_ENV ?= $(EMSDK)/emsdk_env.sh
DRYRUN_DUCKDB_VERSION ?= v1.5.1
DRYRUN_WASM_PLATFORM ?= wasm_eh
DRYRUN_WASM_ARTIFACT := $(PROJ_DIR)build/$(DRYRUN_WASM_PLATFORM)/repository/$(DRYRUN_DUCKDB_VERSION)/$(DRYRUN_WASM_PLATFORM)/$(EXT_NAME).duckdb_extension.wasm
DRYRUN_WEB_EXTENSION_DIR := $(PROJ_DIR)web/public/extensions/$(DRYRUN_DUCKDB_VERSION)/$(DRYRUN_WASM_PLATFORM)

.PHONY: wasm
wasm:
	@test -f "$(EMSDK_ENV)" || (echo "Missing emsdk environment at $(EMSDK_ENV). Set EMSDK=/path/to/emsdk." >&2; exit 1)
	cd "$(EMSDK)" && . "$(EMSDK_ENV)" >/dev/null && cd "$(PROJ_DIR)" && $(MAKE) wasm_eh
	@test -f "$(DRYRUN_WASM_ARTIFACT)" || (echo "Missing wasm artifact at $(DRYRUN_WASM_ARTIFACT)" >&2; exit 1)
	@mkdir -p "$(DRYRUN_WEB_EXTENSION_DIR)"
	cp "$(DRYRUN_WASM_ARTIFACT)" "$(DRYRUN_WEB_EXTENSION_DIR)/"
	@printf "Copied %s to %s/\n" "$(DRYRUN_WASM_ARTIFACT)" "$(DRYRUN_WEB_EXTENSION_DIR)"

.PHONY: benchmark
benchmark:
	python3 scripts/benchmark.py $(BENCHMARK_COLUMN_ARG) $(if $(BENCHMARK_INPUT),$(BENCHMARK_ARGS_FOR_PY),)

.PHONY: clang-format
clang-format:
	$(CLANG_FORMAT) -i $(CLANG_FORMAT_FILES)

.PHONY: format-all
format-all: clang-format

ifneq ($(filter benchmark,$(MAKECMDGOALS)),)
%:
	@:
endif
