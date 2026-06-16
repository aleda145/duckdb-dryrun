PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=dryrun
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile

BENCHMARK_ARGS := $(filter-out benchmark,$(MAKECMDGOALS))
BENCHMARK_COLUMN_ARG := $(if $(COLUMN),--column $(COLUMN),)

.PHONY: benchmark
benchmark:
	python3 scripts/benchmark.py $(BENCHMARK_COLUMN_ARG) $(BENCHMARK_ARGS)

ifneq ($(filter benchmark,$(MAKECMDGOALS)),)
%:
	@:
endif
