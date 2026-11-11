#!/usr/bin/env bash
# Uploads a signed build to build/<sha>/... once (DXR-11). The PutObject carries If-None-Match: * (the bucket policy
# refuses it without), so an object that is already there is never replaced.
#
# A rerun of the signing job after S3 accepted the upload (a later step failed) finds the object there. The rerun
# signs the same build artifact, and the KMS signature (PKCS1 v1.5) and gzip -n are deterministic, so its file is
# byte-identical: the existing object is read back through the bucket's public HTTPS URL (the signer can't read the
# bucket) and reused if its SHA-256 and recorded digest are this file's. Anything else fails, as a second upload of
# different bytes under the same commit must.
#
#   scripts/branching/release/upload.sh <bucket> <key> <file> <gzip sha256> <signed sha256> <unsigned sha256>
#
# Needs AWS_REGION. Logs go to stderr. On success it prints, for $GITHUB_OUTPUT: version_id, etag.
set -euo pipefail
usage="usage: upload.sh <bucket> <key> <file> <gzip sha256> <signed sha256> <unsigned sha256>"
BUCKET=${1:?$usage}
KEY=${2:?$usage}
FILE=${3:?$usage}
GZIP_SHA256=${4:?$usage}
SIGNED_SHA256=${5:?$usage}
UNSIGNED_SHA256=${6:?$usage}
: "${AWS_REGION:?upload.sh needs AWS_REGION}"

log() { echo "$*" >&2; }
fail() { echo "::error::upload: $*" >&2; exit 1; }
sha256_of() { sha256sum "$1" | cut -d' ' -f1; }

[ "$(sha256_of "$FILE")" = "$GZIP_SHA256" ] || fail "$FILE is not the signed build (SHA-256 $(sha256_of "$FILE"), not $GZIP_SHA256)"

log "creating s3://$BUCKET/$KEY"
if out=$(aws s3api put-object --bucket "$BUCKET" --key "$KEY" --body "$FILE" \
	--if-none-match '*' --checksum-algorithm SHA256 \
	--metadata "gzip-sha256=$GZIP_SHA256,signed-sha256=$SIGNED_SHA256,unsigned-sha256=$UNSIGNED_SHA256" \
	--output json 2>&1); then
	version_id=$(jq -r '.VersionId' <<< "$out")
	etag=$(jq -r '.ETag' <<< "$out")
	log "created s3://$BUCKET/$KEY, version $version_id, ETag $etag"
	echo "version_id=$version_id"
	echo "etag=$etag"
	exit 0
fi
grep -q -E 'PreconditionFailed|\(412\)' <<< "$out" || fail "upload failed: $out"

# The object is there: reuse it only if it is this exact file
url="https://$BUCKET.s3.$AWS_REGION.amazonaws.com/$KEY"
log "s3://$BUCKET/$KEY already exists; reading it back from $url"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
curl -fsS --retry 3 -D "$work/headers" -o "$work/existing" "$url" \
	|| fail "s3://$BUCKET/$KEY already exists and can't be read back from $url"
# header <name>: the last value of that response header (names are case-insensitive)
header() { tr -d '\r' < "$work/headers" | awk -v n="$1" 'index(tolower($0), n ": ") == 1 { v = substr($0, length(n) + 3) } END { print v }'; }
existing_sha=$(sha256_of "$work/existing")
recorded=$(header x-amz-meta-gzip-sha256)
version_id=$(header x-amz-version-id)
etag=$(header etag)
if [ "$existing_sha" != "$GZIP_SHA256" ] || [ "$recorded" != "$GZIP_SHA256" ]; then
	fail "s3://$BUCKET/$KEY already exists with SHA-256 $existing_sha (recorded ${recorded:-none}), not this build's" \
		"$GZIP_SHA256; a build is uploaded once and never replaced (DXR-11)"
fi
[ -n "$version_id" ] || fail "s3://$BUCKET/$KEY has no version ID in $url's response"
[ -n "$etag" ] || fail "s3://$BUCKET/$KEY has no ETag in $url's response"
echo "::notice::s3://$BUCKET/$KEY was uploaded by an earlier attempt with these exact bytes; reusing version $version_id" >&2
echo "version_id=$version_id"
echo "etag=$etag"
