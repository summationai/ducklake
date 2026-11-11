#!/usr/bin/env bash
# Tests record.sh (DXR-18): complete build, canary and release records pass and list every field; each missing field,
# a malformed digest, commit, upstream base or ETag fails; `none` is a valid before-version and before-ETag on a first
# publish.
#
#   scripts/branching/release/test_record.sh
set -uo pipefail
RECORD="$(cd "$(dirname "$0")" && pwd)/record.sh"
failures=0
pass() { echo "ok    $1"; }
flunk() { echo "FAIL  $1"; failures=$((failures + 1)); }
H=$(printf 'a%.0s' {1..64})
E='"0123456789abcdef0123456789abcdef"'

both=(GITHUB_SHA=0123456789abcdef0123456789abcdef01234567 UPSTREAM_BASE=9a680f905c106ba9f549fe6f883103547791b4d3
	EXTENSION_VERSION=0123456789 DUCKDB_VERSION=v2.0.0
	UNSIGNED_SHA256="$H" SIGNED_SHA256="$H" GZIP_SHA256="$H" KEY_FINGERPRINT="$H")
build=("${both[@]}" GITHUB_REF=refs/heads/develop
	OBJECT=s3://bucket/build/x/v2.0.0/linux_arm64/ducklake.duckdb_extension.gz OBJECT_VERSION=v1 OBJECT_ETAG="$E")
canary=("${both[@]}" FROM=s3://bucket/build/x.gz TO=s3://bucket/canary/x.gz
	PREVIOUS_VERSION=v0 PREVIOUS_SHA256="$H" PREVIOUS_ETAG='"fedcba9876543210fedcba9876543210"' VERSION=v1 ETAG="$E"
	APPROVED_BY=pssaravanan
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
if run canary "${canary[@]}" PREVIOUS_VERSION=none PREVIOUS_SHA256=none PREVIOUS_ETAG=none \
	&& grep -q 'object version before | `none (gzip none, ETag none)`' out; then
	pass "none is accepted as the before-version of a first publish"
else
	flunk "none was refused: $(cat err)"
fi
if run release "${canary[@]}" UPSTREAM_BASE=release/v2.0.0 || ! grep -q "UPSTREAM_BASE is 'release/v2.0.0', not a commit SHA" err; then
	flunk "a branch name passed as the upstream base"
else
	pass "an upstream base that isn't a commit SHA fails"
fi
if run build "${build[@]}" OBJECT_ETAG=none || ! grep -q "OBJECT_ETAG is 'none', not an ETag" err; then
	flunk "a build record without a real ETag passed"
else
	pass "a build's ETag can't be none"
fi
if run canary "${canary[@]}" ETAG=none || ! grep -q "ETAG is 'none', not an ETag" err; then
	flunk "a publication without the new ETag passed"
else
	pass "a publication's new ETag can't be none"
fi
if run canary "${canary[@]}" APPROVED_AT=yesterday || ! grep -q "APPROVED_AT is 'yesterday'" err; then
	flunk "a malformed approval time passed"
else
	pass "a malformed approval time fails"
fi

echo
echo "$failures failure(s)"
exit $((failures > 0))
