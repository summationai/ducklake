#!/usr/bin/env bash
# Tests record.sh (DXR-18): complete build and canary records pass and list every field; each missing field, a
# malformed digest and a malformed commit fail; `none` is a valid before-version on a first publish.
#
#   scripts/branching/release/test_record.sh
set -uo pipefail
RECORD="$(cd "$(dirname "$0")" && pwd)/record.sh"
failures=0
pass() { echo "ok    $1"; }
flunk() { echo "FAIL  $1"; failures=$((failures + 1)); }
H=$(printf 'a%.0s' {1..64})

both=(GITHUB_SHA=0123456789abcdef0123456789abcdef01234567 DUCKDB_VERSION=v2.0.0
	UNSIGNED_SHA256="$H" SIGNED_SHA256="$H" GZIP_SHA256="$H" KEY_FINGERPRINT="$H")
build=("${both[@]}" GITHUB_REF=refs/heads/develop
	OBJECT=s3://bucket/build/x/v2.0.0/linux_arm64/ducklake.duckdb_extension.gz OBJECT_VERSION=v1)
canary=("${both[@]}" FROM=s3://bucket/build/x.gz TO=s3://bucket/canary/x.gz
	PREVIOUS_VERSION=v0 PREVIOUS_SHA256="$H" VERSION=v1 APPROVED_BY=pssaravanan
	APPROVED_AT=2026-11-04T10:00:00Z PUBLISHED_AT=2026-11-04T10:01:00Z)

run() { # <kind> <VAR=value...>: record.sh with exactly these variables
	local kind=$1
	shift
	env -i PATH="$PATH" "$@" bash "$RECORD" "$kind" > out 2> err
}
cd "$(mktemp -d "${TMPDIR:-/tmp}/test-record.XXXXXX")" || exit 1

check_kind() { # <kind> <VAR=value...>: the complete record, then each field left out in turn
	local kind=$1 i name missing all_fail=1
	shift
	local fields=("$@")
	if run "$kind" "${fields[@]}"; then
		missing=$(for f in "${fields[@]}"; do grep -qF -- "${f#*=}" out || echo "${f%%=*}"; done)
		if [ -z "$missing" ]; then
			pass "a complete $kind record passes and shows every field"
		else
			flunk "the $kind record leaves out: $missing"
		fi
	else
		flunk "a complete $kind record failed: $(cat err)"
	fi
	for i in "${!fields[@]}"; do
		name=${fields[$i]%%=*}
		if run "$kind" "${fields[@]:0:$i}" "${fields[@]:$((i + 1))}" || ! grep -q "$name is missing" err; then
			flunk "$kind without $name did not fail naming it"
			all_fail=0
		fi
	done
	[ $all_fail = 0 ] || pass "a $kind record fails, naming the field, when any of its ${#fields[@]} fields is missing"
}
check_kind build "${build[@]}"
check_kind canary "${canary[@]}"
check_kind release "${canary[@]/canary\//release/}"

if run build "${build[@]}" GZIP_SHA256=abc123 || ! grep -q "GZIP_SHA256 is 'abc123', not a SHA-256" err; then
	flunk "a malformed digest passed"
else
	pass "a malformed digest fails"
fi
if run canary "${canary[@]}" GITHUB_SHA=main || ! grep -q "GITHUB_SHA is 'main', not a commit SHA" err; then
	flunk "a malformed commit passed"
else
	pass "a malformed commit fails"
fi
# shellcheck disable=SC2016 # the backticks are markdown
if run canary "${canary[@]}" PREVIOUS_VERSION=none PREVIOUS_SHA256=none && grep -q 'object version before | `none (gzip none)`' out; then
	pass "none is accepted as the before-version of a first publish"
else
	flunk "none was refused: $(cat err)"
fi
if run canary "${canary[@]}" APPROVED_AT=yesterday || ! grep -q "APPROVED_AT is 'yesterday'" err; then
	flunk "a malformed approval time passed"
else
	pass "a malformed approval time fails"
fi

echo
echo "$failures failure(s)"
exit $((failures > 0))
