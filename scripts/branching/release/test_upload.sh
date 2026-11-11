#!/usr/bin/env bash
# Tests upload.sh (DXR-11) without AWS: a stub `aws` keeps a versioned bucket in a directory and honours If-None-Match
# on put-object; a stub `curl` serves its objects as the public HTTPS URL does.
#
#   scripts/branching/release/test_upload.sh
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=$(mktemp -d "${TMPDIR:-/tmp}/test-upload.XXXXXX")
trap 'rm -rf "$WORK"' EXIT
failures=0
pass() { echo "ok    $1"; }
flunk() { echo "FAIL  $1"; failures=$((failures + 1)); }

mkdir "$WORK/bin" "$WORK/bucket"
cat > "$WORK/s3.py" << 'EOF'
# put-object into a directory (one body file and one JSON file per key), and a public GET of it
import json, os, shutil, sys, uuid
root = os.environ["STUB_BUCKET_DIR"]
def path(key): return os.path.join(root, key.replace("/", "%"))
def error(code, status):
    print(f"\nAn error occurred ({code}) when calling the PutObject operation: stub {status}", file=sys.stderr); sys.exit(254)
if sys.argv[1] == "aws":
    args, opts, i = sys.argv[4:], {}, 0
    if sys.argv[2:4] != ["s3api", "put-object"]: sys.exit(f"stub aws: unexpected {sys.argv[2:4]}")
    while i < len(args):
        if args[i] + " " in "--bucket --key --body --if-none-match --checksum-algorithm --metadata --output ":
            opts[args[i]] = args[i + 1]; i += 2
        else:
            sys.exit(f"stub aws: unexpected argument {args[i]}")
    if os.environ.get("STUB_DENY"): error("AccessDenied", 403)
    if opts.get("--if-none-match") != "*": error("AccessDenied", 403)  # the bucket policy's BuildIsCreateOnly
    if os.path.exists(path(opts["--key"])): error("PreconditionFailed", 412)
    version = uuid.uuid4().hex
    shutil.copy(opts["--body"], path(opts["--key"]))
    meta = dict(kv.split("=", 1) for kv in opts["--metadata"].split(","))
    etag = '"%s"' % uuid.uuid4().hex
    json.dump({"VersionId": version, "ETag": etag, "Metadata": meta}, open(path(opts["--key"]) + ".json", "w"))
    print(json.dumps({"ETag": etag, "VersionId": version}))
else:  # curl -fsS --retry 3 -D <headers> -o <out> <url>
    args = sys.argv[2:]
    headers, out, url = args[args.index("-D") + 1], args[args.index("-o") + 1], args[-1]
    prefix = "https://bucket.s3.us-west-2.amazonaws.com/"
    key = url[len(prefix):] if url.startswith(prefix) else None
    if os.environ.get("STUB_PRIVATE") or key is None or not os.path.exists(path(key)):
        print("curl: (22) The requested URL returned error: 403", file=sys.stderr); sys.exit(22)
    info = json.load(open(path(key) + ".json"))
    shutil.copy(path(key), out)
    with open(headers, "w") as f:
        f.write("HTTP/1.1 200 OK\r\n")
        f.write("X-Amz-Version-Id: %s\r\n" % info["VersionId"])
        f.write("ETag: %s\r\n" % info["ETag"])
        for k, v in info["Metadata"].items(): f.write("x-amz-meta-%s: %s\r\n" % (k, v))
        f.write("\r\n")
EOF
# the stubs expand their variables when they run
# shellcheck disable=SC2016
for tool in aws curl; do
	printf '#!/usr/bin/env bash\nexec python3 -I "%s" %s "$@"\n' "$WORK/s3.py" "$tool" > "$WORK/bin/$tool"
	chmod +x "$WORK/bin/$tool"
done
export PATH="$WORK/bin:$PATH" STUB_BUCKET_DIR="$WORK/bucket" AWS_REGION=us-west-2

sha256_of() { sha256sum "$1" | cut -d' ' -f1; }
H=$(printf 'a%.0s' {1..64})
printf 'signed build one' > "$WORK/one.gz"
printf 'signed build two' > "$WORK/two.gz"
ONE=$(sha256_of "$WORK/one.gz")
TWO=$(sha256_of "$WORK/two.gz")
KEY=build/0123/v2.0.0/linux_arm64/ducklake.duckdb_extension.gz
upload() { # <key> <file> <gzip sha256>
	bash "$HERE/upload.sh" bucket "$1" "$2" "$3" "$H" "$H" > "$WORK/out" 2> "$WORK/err"
}

# 1. the first upload creates the object
if upload "$KEY" "$WORK/one.gz" "$ONE" && grep -q '^version_id=[0-9a-f]\{32\}$' "$WORK/out" \
	&& grep -q '^etag="[0-9a-f]\{32\}"$' "$WORK/out"; then
	first=$(cat "$WORK/out")
	pass "the first upload creates the object (If-None-Match) and reports its version and ETag"
else
	flunk "first upload: $(cat "$WORK/err")"
fi

# 2. a rerun with the same bytes reuses it and reports the same version
if upload "$KEY" "$WORK/one.gz" "$ONE" && [ "$(cat "$WORK/out")" = "${first:-}" ] && grep -q 'reusing version' "$WORK/err"; then
	pass "a rerun with the same bytes reuses the existing object, its version and its ETag"
else
	flunk "rerun: $(cat "$WORK/out") $(cat "$WORK/err")"
fi

# 3. different bytes under the same commit fail and replace nothing
if upload "$KEY" "$WORK/two.gz" "$TWO"; then
	flunk "different bytes were accepted"
elif grep -q 'never replaced (DXR-11)' "$WORK/err" && cmp -s "$WORK/one.gz" "$WORK/bucket/$(tr / % <<< "$KEY")"; then
	pass "different bytes under the same commit fail (DXR-11) and the object is unchanged"
else
	flunk "different bytes: $(cat "$WORK/err")"
fi

# 4. an existing object that can't be read back (no public read yet) fails
if STUB_PRIVATE=1 upload "$KEY" "$WORK/one.gz" "$ONE"; then
	flunk "an unreadable existing object was reused"
elif grep -q "can't be read back" "$WORK/err"; then
	pass "an existing object that can't be read back fails"
else
	flunk "unreadable: $(cat "$WORK/err")"
fi

# 5. any other upload error fails closed
if STUB_DENY=1 upload build/4567/x.gz "$WORK/one.gz" "$ONE"; then
	flunk "an AccessDenied upload succeeded"
elif grep -q 'upload failed' "$WORK/err" && grep -q AccessDenied "$WORK/err"; then
	pass "an access error fails closed"
else
	flunk "denied: $(cat "$WORK/err")"
fi

# 6. a file that isn't the signed build is refused before the upload
if upload build/89ab/x.gz "$WORK/two.gz" "$ONE"; then
	flunk "a file with another SHA-256 was uploaded"
elif grep -q 'is not the signed build' "$WORK/err" && [ ! -e "$WORK/bucket/build%89ab%x.gz" ]; then
	pass "a file whose SHA-256 isn't the signed build's is refused before the upload"
else
	flunk "wrong file: $(cat "$WORK/err")"
fi

echo
echo "$failures failure(s)"
exit $((failures > 0))
