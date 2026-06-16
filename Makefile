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
