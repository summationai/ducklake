PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=ducklake
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# Core extensions that we need for crucial testing
DEFAULT_TEST_EXTENSION_DEPS=
# For cloud testing we also need these extensions
FULL_TEST_EXTENSION_DEPS=httpfs

# Stabilize all tests in CI
ifdef CI
TEST_FLAGS:=--stabilize-tests
endif
# ~[.] excludes hidden/slow (.test_slow) tests
T ?= $(TEST_FLAGS) "~[.]test/*"

# Aws and Azure have vcpkg dependencies and therefore need vcpkg merging
ifeq (${BUILD_EXTENSION_TEST_DEPS}, full)
	USE_MERGED_VCPKG_MANIFEST:=1
endif

# The DuckDB this line builds against (.github/duckdb-version) is v2.0.0, the one GizmoSQL v1.42.2 embeds.
# DuckDB 2.0 takes its version from this override (or DUCKDB_VERSION), never from git describe: without it
# a checkout is v2.0.0-dev<count>, the extension's footer names a commit hash and the server refuses it.
# The toolchain passes this to CMake as -DOVERRIDE_GIT_DESCRIBE.
ifeq ($(strip $(OVERRIDE_GIT_DESCRIBE)),)
OVERRIDE_GIT_DESCRIBE := v2.0.0
endif

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile

# Release build with clang source-based coverage of the DuckLake sources only, like `make codecov` in DuckDB
.PHONY: codecov
codecov: ${EXTENSION_CONFIG_STEP}
	mkdir -p build/codecov
	cmake $(GENERATOR) $(BUILD_FLAGS) $(EXT_RELEASE_FLAGS) $(VCPKG_MANIFEST_FLAGS) -DCMAKE_BUILD_TYPE=Release -DDUCKLAKE_COVERAGE=1 -S $(DUCKDB_SRCDIR) -B build/codecov
	cmake --build build/codecov --config Release

unittest_relassert:
	python3 duckdb/scripts/ci/run_tests.py build/relassert/test/unittest --batch-timeout 900 --max-retries 10 $(T)