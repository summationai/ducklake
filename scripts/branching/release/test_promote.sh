#!/usr/bin/env bash
# Tests promote.sh (DXR-12) without AWS: a stub `aws` keeps a versioned bucket in a directory and honours
# If-None-Match / If-Match / x-amz-copy-source-if-match on copy-object the way S3 does.
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
    if args[i] + " " in "--bucket --copy-source --key --metadata-directive --checksum-algorithm --if-none-match --if-match --copy-source-if-match --output --version-id --checksum-mode ":
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
    src_key = opts["--copy-source"].split("/", 1)[1]
    if os.environ.get("STUB_SRC_SWAP"):  # the build object changes between its check and the copy
        put(src_key, "9" * 64, load(src_key)[-1]["Metadata"])
    src = load(src_key)
    if not src: error("NoSuchKey", 404, "CopyObject")
    if "--copy-source-if-match" in opts and src[-1]["ETag"] != opts["--copy-source-if-match"]:
        error("PreconditionFailed", 412, "CopyObject")
    current = load(opts["--key"])
    if "--if-none-match" in opts and current: error("PreconditionFailed", 412, "CopyObject")
    if "--if-match" in opts:
        if os.environ.get("STUB_RACE"):  # another publish lands between the read and this copy
            put(opts["--key"], "f" * 64, {"gzip-sha256": "f" * 64}); current = load(opts["--key"])
        if not current or current[-1]["ETag"] != opts["--if-match"]: error("PreconditionFailed", 412, "CopyObject")
    sha = "0" * 64 if os.environ.get("STUB_CORRUPT") else src[-1]["sha"]
    v = put(opts["--key"], sha, src[-1]["Metadata"])
    if os.environ.get("STUB_LATE_WRITE"):  # another write lands right after this copy, before the read-back
        put(opts["--key"], "e" * 64, {"gzip-sha256": "e" * 64})
    print(json.dumps({"CopyObjectResult": {"ETag": v["ETag"]}, "VersionId": v["VersionId"]}))
elif op == "head-object":
    versions = load(opts["--key"])
    # the writer roles have s3:GetObject but not s3:GetObjectVersion, so S3 refuses a read of a given version
    if "--version-id" in opts: error("403", 403, "HeadObject")
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
chmod +x "$WORK/bin/aws"
export PATH="$WORK/bin:$PATH" STUB_BUCKET_DIR="$WORK/bucket"

seed() { # <build key> <sha256> [<sha256 of the bytes>]: an uploaded build, version b-<first 6 of the sha256>
	python3 -I -c '
import json, os, sys
key, sha, body = sys.argv[1], sys.argv[2], sys.argv[3]
json.dump([{"VersionId": "b-" + sha[:6], "ETag": "\"e-%s\"" % sha[:6], "sha": body, "Metadata": {"gzip-sha256": sha}}],
          open(os.path.join(os.environ["STUB_BUCKET_DIR"], key.replace("/", "%")), "w"))' "$1" "$2" "${3:-$2}"
}
versions() { python3 -I -c 'import json, sys; print(len(json.load(open(sys.argv[1]))))' "$WORK/bucket/$(tr / % <<< "$1")" 2> /dev/null || echo 0; }
promote() { # <build key> <sha256> [<build version>]
	bash "$HERE/promote.sh" bucket "$1" "${3:-b-${2:0:6}}" test/canary/v2.0.0/linux_arm64/ducklake.duckdb_extension.gz "$2" \
		> "$WORK/out" 2> "$WORK/err"
}
DST=test/canary/v2.0.0/linux_arm64/ducklake.duckdb_extension.gz
A=$(printf a%.0s {1..64})
B=$(printf b%.0s {1..64})
C=$(printf c%.0s {1..64})
seed test/build/aaa/x.gz "$A"
seed test/build/bbb/x.gz "$B"
seed test/build/ccc/x.gz "$C"
D=$(printf d%.0s {1..64})
seed test/build/ddd/x.gz "$D"

# 1. the first publish to an empty channel works
if promote test/build/aaa/x.gz "$A" && grep -q '^previous_version=none$' "$WORK/out" \
	&& grep -q '^previous_etag=none$' "$WORK/out" && [ "$(versions "$DST")" = 1 ]; then
	first_etag=$(sed -n 's/^etag=//p' "$WORK/out")
	pass "the first publish to an empty channel works (If-None-Match); there is no previous ETag"
else
	flunk "first publish: $(cat "$WORK/err")"
fi

# 2. a newer build replaces it through If-Match, and the previous version is reported
if promote test/build/bbb/x.gz "$B" && grep -q "^previous_gzip_sha256=$A\$" "$WORK/out" && [ "$(versions "$DST")" = 2 ] \
	&& [ -n "${first_etag:-}" ] && grep -qxF "previous_etag=$first_etag" "$WORK/out" && ! grep -qxF "etag=$first_etag" "$WORK/out"; then
	pass "a newer build replaces it (If-Match on the current ETag); the previous version and ETag are kept and reported"
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

# 5b. the retry after that (the channel records the build but holds other bytes) replaces it instead of skipping
if promote test/build/ccc/x.gz "$C" && ! grep -q 'already holds' "$WORK/err" \
	&& grep -q "::warning::promote: .*records this build but its SHA-256" "$WORK/err"; then
	pass "a retry replaces a channel object whose metadata is the build's but whose bytes aren't"
else
	flunk "retry after a corrupt copy: $(cat "$WORK/err")"
fi

# 6. a write that lands between the copy and the read-back is caught, not checked in place of ours
if STUB_LATE_WRITE=1 promote test/build/ddd/x.gz "$D"; then
	flunk "a write after the copy went unnoticed"
elif grep -q 'is now version' "$WORK/err"; then
	pass "a write right after the copy fails the read-back instead of checking the other object"
else
	flunk "late write: $(cat "$WORK/err")"
fi

# 7. any other error fails closed
if STUB_DENY=1 promote test/build/ccc/x.gz "$C"; then
	flunk "an AccessDenied copy succeeded"
elif grep -q 'AccessDenied' "$WORK/err" && ! grep -q '::warning' "$WORK/err"; then
	pass "an access error fails closed"
else
	flunk "denied: $(cat "$WORK/err")"
fi

# 8. a build object whose bytes aren't its recorded digest is never copied
E=$(printf e%.0s {1..64})
seed test/build/eee/x.gz "$E" "$(printf 0%.0s {1..64})"
before=$(versions "$DST")
if promote test/build/eee/x.gz "$E"; then
	flunk "a corrupt build was published"
elif grep -q 'nothing copied' "$WORK/err" && [ "$(versions "$DST")" = "$before" ]; then
	pass "a build whose bytes differ from its digest fails before any copy"
else
	flunk "corrupt build: $(versions "$DST") versions; $(cat "$WORK/err")"
fi

# 9. a build object that isn't the version the signing job uploaded is never copied
if promote test/build/aaa/x.gz "$A" b-other; then
	flunk "another version of the build was published"
elif grep -q "not b-other that the signing job uploaded" "$WORK/err" && [ "$(versions "$DST")" = "$before" ]; then
	pass "a build object other than the uploaded version fails before any copy"
else
	flunk "wrong build version: $(cat "$WORK/err")"
fi

# 10. a build object that changes after its check is not copied
if STUB_SRC_SWAP=1 promote test/build/aaa/x.gz "$A"; then
	flunk "a build that changed after its check was published"
elif [ "$(versions "$DST")" = "$before" ]; then
	pass "a build that changes between its check and the copy is not copied (copy-source If-Match)"
else
	flunk "swapped build: $(versions "$DST") versions; $(cat "$WORK/err")"
fi

echo
echo "$failures failure(s)"
exit $((failures > 0))
