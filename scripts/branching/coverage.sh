#!/usr/bin/env bash
# Measures test coverage of src/branching: instruments only the branching objects, runs every branch test on
# every local configuration and writes docs/branching/COVERAGE.md. The uninstrumented release build is restored
# afterwards. Usage: scripts/branching/coverage.sh [--keep-build]
set -euo pipefail
cd "$(dirname "$0")/../.."

OUT=build/branching-coverage
CONFIGS="default no_inline deletion_vectors sqlite ducklake_version"
KEEP_BUILD=0
if [ "${1:-}" = "--keep-build" ]; then
	KEEP_BUILD=1
fi

build() {
	local coverage=$1 link=""
	if [ "$coverage" = ON ]; then
		link=-fprofile-instr-generate
	fi
	echo "building release with branching coverage $coverage"
	if ! make release EXT_FLAGS="-DDUCKLAKE_BRANCHING_COVERAGE=$coverage -DCMAKE_EXE_LINKER_FLAGS=$link \
		-DCMAKE_SHARED_LINKER_FLAGS=$link -DCMAKE_MODULE_LINKER_FLAGS=$link" >"$OUT/build-$coverage.log" 2>&1; then
		tail -30 "$OUT/build-$coverage.log"
		exit 1
	fi
}

restore() {
	if [ "$KEEP_BUILD" = 0 ]; then
		build OFF
	fi
}

rm -rf "$OUT" duckdb_unittest_tempdir
mkdir -p "$OUT/raw"
trap restore EXIT
build ON

for test in test/sql/branch/*.test; do
	name=$(basename "$test" .test)
	for config in $CONFIGS; do
		args=()
		if [ "$config" != default ]; then
			args=(--test-config "test/configs/$config.json")
		fi
		if ! LLVM_PROFILE_FILE="$OUT/raw/$name@$config.profraw" build/release/test/unittest ${args[@]+"${args[@]}"} \
			"$test" >"$OUT/raw/$name@$config.log" 2>&1; then
			echo "FAILED: $test ($config)"
			tail -30 "$OUT/raw/$name@$config.log"
			exit 1
		fi
	done
	echo "ran $name"
done

# the tests run the branching code from the loadable extension
python3 scripts/branching/coverage_report.py "$OUT" build/release/extension/ducklake/ducklake.duckdb_extension \
	docs/branching/COVERAGE.md
