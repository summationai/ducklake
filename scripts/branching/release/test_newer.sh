#!/usr/bin/env bash
# Tests newer.sh (DXR-15 step 4) without AWS: a stub `aws` answers head-object from STUB_HELD (the commit release/
# records), STUB_EMPTY (403, as S3 answers a missing object to a role that can't list), STUB_NOCOMMIT or STUB_DENY,
# against a scratch repository whose history is A - B - C, with D on a side branch off A.
#
#   scripts/branching/release/test_newer.sh
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=$(mktemp -d "${TMPDIR:-/tmp}/test-newer.XXXXXX")
trap 'rm -rf "$WORK"' EXIT
failures=0
pass() { echo "ok    $1"; }
flunk() { echo "FAIL  $1"; failures=$((failures + 1)); }

mkdir "$WORK/bin"
cat > "$WORK/bin/aws" <<'STUB'
#!/usr/bin/env bash
[ "$1 $2" = "s3api head-object" ] || { echo "stub aws: unexpected $*" >&2; exit 2; }
if [ -n "${STUB_DENY-}" ]; then
	echo "An error occurred (InternalError) when calling the HeadObject operation: stub" >&2; exit 254
elif [ -n "${STUB_EMPTY-}" ]; then
	echo "An error occurred (403) when calling the HeadObject operation: Forbidden" >&2; exit 254
elif [ -n "${STUB_NOCOMMIT-}" ]; then
	echo '{"VersionId": "v1", "Metadata": {"gzip-sha256": "x"}}'
else
	printf '{"VersionId": "v1", "Metadata": {"commit": "%s"}}\n' "$STUB_HELD"
fi
STUB
chmod +x "$WORK/bin/aws"
export PATH="$WORK/bin:$PATH"

cd "$WORK" && git init -q repo && cd repo || exit 1
git config user.email t@t && git config user.name t
commit() { git commit -q --allow-empty -m "$1" && git rev-parse HEAD; }
A=$(commit A); B=$(commit B); C=$(commit C)
git checkout -q -b side "$A" && D=$(commit D) && git checkout -q -
GONE=$(printf 'f%.0s' {1..40}) # a commit a rebase rewrote away
KEY=release/v2.0.0/linux_arm64/ducklake.duckdb_extension.gz

newer() { # <commit> <develop head>
	bash "$HERE/newer.sh" bucket "$KEY" "$1" "$2" > "$WORK/out" 2> "$WORK/err"
}
expect() { # <what> <publish=...> <status 0|1> <commit> <head>
	local status=0
	newer "$4" "$5" || status=1
	if [ "$status" = "$3" ] && { [ "$3" = 1 ] || [ "$(cat "$WORK/out")" = "$2" ]; }; then
		pass "$1"
	else
		flunk "$1: status $status, $(cat "$WORK/out") $(cat "$WORK/err")"
	fi
}

STUB_EMPTY=1 expect "release/ empty: publishes" publish=true 0 "$B" "$C"
STUB_HELD=$B expect "release/ holds this commit: publishes (a rerun)" publish=true 0 "$B" "$C"
STUB_HELD=$A expect "release/ holds an ancestor: publishes" publish=true 0 "$C" "$C"
STUB_HELD=$C expect "release/ holds a newer commit: skips as superseded" publish=false 0 "$B" "$C"
grep -q 'superseded: release/ holds' "$WORK/err" || flunk "the skip doesn't say superseded"
STUB_HELD=$D expect "release/ holds a commit off this one's history: skips" publish=false 0 "$C" "$C"
STUB_HELD=$GONE expect "release/ holds a rebased-away commit, run is develop's head: publishes" publish=true 0 "$C" "$C"
STUB_HELD=$GONE expect "release/ holds a rebased-away commit, run isn't the head: skips" publish=false 0 "$B" "$C"
STUB_NOCOMMIT=1 expect "release/ records no commit: fails closed" "" 1 "$C" "$C"
grep -q 'records no commit' "$WORK/err" || flunk "the missing-commit failure doesn't say why"
STUB_DENY=1 expect "any other read error: fails closed" "" 1 "$C" "$C"

exit $((failures > 0))
