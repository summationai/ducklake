#!/usr/bin/env bash
# Tests promote.sh and still_head.sh (DXR-12) without AWS or GitHub: a stub `aws` keeps a versioned bucket in a
# directory and honours If-None-Match / If-Match on copy-object the way S3 does; a stub `gh` returns a branch head.
#
#   scripts/branching/release/test_promote.sh
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=$(mktemp -d "${TMPDIR:-/tmp}/test-promote.XXXXXX")
trap 'rm -rf "$WORK"' EXIT
failures=0
pass() { echo "ok    $1"; }
flunk() { echo "FAIL  $1"; failures=$((failures + 1)); }

mkdir "$WORK/bin" "$WORK/bucket"
cat > "$WORK/s3.py" << 'EOF'
# A versioned S3 bucket in a directory: one JSON file per key, holding its versions (newest last).
import hashlib, json, os, sys, uuid
root = os.environ["STUB_BUCKET_DIR"]
args = sys.argv[1:]
op, opts, i = args[1], {}, 2
while i < len(args):
    if args[i] + " " in "--bucket --copy-source --key --metadata-directive --checksum-algorithm --if-none-match --if-match --output --version-id --checksum-mode ":
        opts[args[i]] = args[i + 1]; i += 2
    else:
        sys.exit(f"stub aws: unexpected argument {args[i]}")
def path(key): return os.path.join(root, key.replace("/", "%"))
def load(key): return json.load(open(path(key))) if os.path.exists(path(key)) else []
def save(key, versions): json.dump(versions, open(path(key), "w"))
def error(code, status, call):
    print(f"\nAn error occurred ({code}) when calling the {call} operation: stub {status}", file=sys.stderr); sys.exit(254)
def put(key, body_sha, metadata):
    versions = load(key)
    v = {"VersionId": uuid.uuid4().hex, "ETag": '"%s"' % uuid.uuid4().hex, "sha": body_sha, "Metadata": metadata}
    save(key, versions + [v]); return v
if op == "copy-object":
    if os.environ.get("STUB_DENY"): error("AccessDenied", 403, "CopyObject")
    src = load(opts["--copy-source"].split("/", 1)[1])
    if not src: error("NoSuchKey", 404, "CopyObject")
    current = load(opts["--key"])
    if "--if-none-match" in opts and current: error("PreconditionFailed", 412, "CopyObject")
    if "--if-match" in opts:
        if os.environ.get("STUB_RACE"):  # another publish lands between the read and this copy
            put(opts["--key"], "f" * 64, {"gzip-sha256": "f" * 64}); current = load(opts["--key"])
        if not current or current[-1]["ETag"] != opts["--if-match"]: error("PreconditionFailed", 412, "CopyObject")
    sha = "0" * 64 if os.environ.get("STUB_CORRUPT") else src[-1]["sha"]
    v = put(opts["--key"], sha, src[-1]["Metadata"])
    print(json.dumps({"CopyObjectResult": {"ETag": v["ETag"]}, "VersionId": v["VersionId"]}))
elif op == "head-object":
    versions = load(opts["--key"])
    if "--version-id" in opts: versions = [v for v in versions if v["VersionId"] == opts["--version-id"]]
    if not versions: error("403", 403, "HeadObject")  # without s3:ListBucket a missing key is a 403
    v = versions[-1]
    out = {"ETag": v["ETag"], "VersionId": v["VersionId"], "Metadata": v["Metadata"]}
    if opts.get("--checksum-mode") == "ENABLED":
        import base64; out["ChecksumSHA256"] = base64.b64encode(bytes.fromhex(v["sha"])).decode()
    print(json.dumps(out))
else:
    sys.exit(f"stub aws: unexpected s3api {op}")
EOF
# the stubs expand their variables when they run
# shellcheck disable=SC2016
printf '#!/usr/bin/env bash\nexec python3 -I "%s" "$@"\n' "$WORK/s3.py" > "$WORK/bin/aws"
# shellcheck disable=SC2016
printf '#!/usr/bin/env bash\necho "$STUB_HEAD"\n' > "$WORK/bin/gh"
chmod +x "$WORK/bin/aws" "$WORK/bin/gh"
export PATH="$WORK/bin:$PATH" STUB_BUCKET_DIR="$WORK/bucket"

seed() { # <build key> <sha256>: an uploaded build
	python3 -I -c '
import json, os, sys
key, sha = sys.argv[1], sys.argv[2]
json.dump([{"VersionId": "b-" + sha[:6], "ETag": "\"e-%s\"" % sha[:6], "sha": sha, "Metadata": {"gzip-sha256": sha}}],
          open(os.path.join(os.environ["STUB_BUCKET_DIR"], key.replace("/", "%")), "w"))' "$1" "$2"
}
versions() { python3 -I -c 'import json, sys; print(len(json.load(open(sys.argv[1]))))' "$WORK/bucket/$(tr / % <<< "$1")" 2> /dev/null || echo 0; }
promote() { bash "$HERE/promote.sh" bucket "$1" test/canary/v2.0.0/linux_arm64/ducklake.duckdb_extension.gz "$2" > "$WORK/out" 2> "$WORK/err"; }
DST=test/canary/v2.0.0/linux_arm64/ducklake.duckdb_extension.gz
A=$(printf a%.0s {1..64})
B=$(printf b%.0s {1..64})
C=$(printf c%.0s {1..64})
seed test/build/aaa/x.gz "$A"
seed test/build/bbb/x.gz "$B"
seed test/build/ccc/x.gz "$C"

# 1. the first publish to an empty channel works
if promote test/build/aaa/x.gz "$A" && grep -q '^previous_version=none$' "$WORK/out" && [ "$(versions "$DST")" = 1 ]; then
	pass "the first publish to an empty channel works (If-None-Match)"
else
	flunk "first publish: $(cat "$WORK/err")"
fi

# 2. a newer build replaces it through If-Match, and the previous version is reported
if promote test/build/bbb/x.gz "$B" && grep -q "^previous_gzip_sha256=$A\$" "$WORK/out" && [ "$(versions "$DST")" = 2 ]; then
	pass "a newer build replaces it (If-Match on the current ETag); the previous version is kept and reported"
else
	flunk "replace: $(cat "$WORK/err")"
fi

# 3. the same build again copies nothing
if promote test/build/bbb/x.gz "$B" && [ "$(versions "$DST")" = 2 ] && grep -q 'already holds this build' "$WORK/err"; then
	pass "publishing the build the channel already holds copies nothing"
else
	flunk "republish: $(cat "$WORK/err")"
fi

# 4. a lost race fails and overwrites nothing: only the other writer's version is added
if STUB_RACE=1 promote test/build/ccc/x.gz "$C"; then
	flunk "a lost race succeeded"
elif [ "$(versions "$DST")" = 3 ] && grep -q '::warning::promote: .*another publish won' "$WORK/err"; then
	pass "a write between the read and the copy makes the copy fail, with a warning, and nothing is overwritten"
else
	flunk "lost race: $(versions "$DST") versions; $(cat "$WORK/err")"
fi

# 5. a copy whose checksum isn't the build's fails
if STUB_CORRUPT=1 promote test/build/ccc/x.gz "$C"; then
	flunk "a corrupt copy passed"
elif grep -q 'not the build' "$WORK/err"; then
	pass "a copy whose SHA-256 differs from the build's fails"
else
	flunk "corrupt copy: $(cat "$WORK/err")"
fi

# 6. any other error fails closed
if STUB_DENY=1 promote test/build/ccc/x.gz "$C"; then
	flunk "an AccessDenied copy succeeded"
elif grep -q 'AccessDenied' "$WORK/err" && ! grep -q '::warning' "$WORK/err"; then
	pass "an access error fails closed"
else
	flunk "denied: $(cat "$WORK/err")"
fi

# 7-8. still_head.sh: publish only the branch's newest commit
S1=$(printf 1%.0s {1..40})
S2=$(printf 2%.0s {1..40})
if STUB_HEAD=$S1 bash "$HERE/still_head.sh" o/r develop "$S1" > /dev/null; then
	pass "still_head: the head commit may publish"
else
	flunk "still_head refused the head"
fi
STUB_HEAD=$S2 bash "$HERE/still_head.sh" o/r develop "$S1" > "$WORK/out" 2>&1
status=$?
if [ $status = 3 ] && grep -q "superseded by $S2" "$WORK/out"; then
	pass "still_head: an older commit skips as superseded"
else
	flunk "still_head on an older commit: exit $status, $(cat "$WORK/out")"
fi
if STUB_HEAD="Not Found" bash "$HERE/still_head.sh" o/r develop "$S1" > /dev/null 2>&1; then
	flunk "still_head passed without a head"
else
	pass "still_head: an unreadable head fails"
fi

echo
echo "$failures failure(s)"
exit $((failures > 0))
