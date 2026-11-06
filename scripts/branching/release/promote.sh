#!/usr/bin/env bash
# Publishes a build to a channel (canary/ or release/) without ever losing an update (DXR-12 and DXR-15). The spec's
# ifGenerationMatch is S3's conditional copy here:
#   0. the build object must be the version the signing job uploaded, and its SHA-256 (S3's own checksum and the
#      recorded digest) the build's; every copy names its ETag (x-amz-copy-source-if-match), so exactly those bytes
#      are published and nothing unverified ever reaches the channel;
#   1. copy build/<sha>/... to the channel with If-None-Match: *, which only succeeds on a first publish;
#   2. if an object is there (412), read its ETag and copy again with If-Match on that ETag. If it already holds the
#      build's bytes (by S3's checksum, not only the copied metadata), nothing is copied;
#   3. a 412 or 409 on that copy means something else wrote in between: the copy fails and nothing is overwritten.
# The roles can't list the bucket, so a missing object can't be told from a forbidden one by reading it first; the
# copy goes first and any error other than a lost condition fails closed. After the copy, the new object's SHA-256
# must equal the build's.
#
#   scripts/branching/release/promote.sh <bucket> <build key> <build version> <channel key> <expected gzip sha256>
#
# Logs go to stderr. On success it prints, for $GITHUB_OUTPUT: previous_version, previous_gzip_sha256, version, etag.
set -euo pipefail
usage="usage: promote.sh <bucket> <build key> <build version> <channel key> <expected gzip sha256>"
BUCKET=${1:?$usage}
SRC=${2:?$usage}
SRC_VERSION=${3:?$usage}
DST=${4:?$usage}
EXPECTED=${5:?$usage}
[[ $EXPECTED =~ ^[0-9a-f]{64}$ ]] || { echo "::error::promote: '$EXPECTED' is not a SHA-256" >&2; exit 1; }

log() { echo "$*" >&2; }
fail() { echo "::error::promote: $*" >&2; exit 1; }
lost_condition() { grep -q -E 'PreconditionFailed|\(412\)|ConditionalRequestConflict|\(409\)' <<< "$1"; }
b64_of_hex() { python3 -I -c 'import base64, sys; print(base64.b64encode(bytes.fromhex(sys.argv[1])).decode())' "$1"; }
EXPECTED_B64=$(b64_of_hex "$EXPECTED")
# holds_build <head-object json>: S3's checksum of the object and its recorded digest are both the build's
holds_build() {
	[ "$(jq -r '.ChecksumSHA256 // empty' <<< "$1")" = "$EXPECTED_B64" ] \
		&& [ "$(jq -r '.Metadata["gzip-sha256"] // empty' <<< "$1")" = "$EXPECTED" ]
}

# 0. The build object, verified before anything is copied. The roles can't read a given version (no
# s3:GetObjectVersion), so this reads the current object and requires it to be the uploaded version.
src=$(aws s3api head-object --bucket "$BUCKET" --key "$SRC" --checksum-mode ENABLED --output json 2>&1) \
	|| fail "can't read the build $SRC: $src"
src_version=$(jq -r '.VersionId // "none"' <<< "$src")
[ "$src_version" = "$SRC_VERSION" ] \
	|| fail "$SRC is version $src_version, not $SRC_VERSION that the signing job uploaded; nothing copied"
holds_build "$src" || fail "$SRC version $src_version has SHA-256 $(jq -r '.ChecksumSHA256 // "none"' <<< "$src") \
(recorded $(jq -r '.Metadata["gzip-sha256"] // "none"' <<< "$src")), not the build's $EXPECTED; nothing copied"
src_etag=$(jq -r '.ETag' <<< "$src")
log "build $SRC version $src_version verified (SHA-256 $EXPECTED, ETag $src_etag)"

# copy <extra args...>: copies the verified build only (a 412 if it changed since), and prints the response, or the
# error text with a non-zero status
copy() {
	aws s3api copy-object --bucket "$BUCKET" --copy-source "$BUCKET/$SRC" --copy-source-if-match "$src_etag" \
		--key "$DST" --metadata-directive COPY --checksum-algorithm SHA256 "$@" --output json 2>&1
}

previous_version=none
previous_sha=none
log "publishing s3://$BUCKET/$SRC to s3://$BUCKET/$DST"
if out=$(copy --if-none-match '*'); then
	log "first publish to $DST (it held nothing)"
else
	if ! grep -q -E 'PreconditionFailed|\(412\)' <<< "$out"; then
		lost_condition "$out" && echo "::warning::promote: another write to $DST was in progress; not retrying" >&2
		fail "copy to $DST failed: $out"
	fi
	current=$(aws s3api head-object --bucket "$BUCKET" --key "$DST" --checksum-mode ENABLED --output json 2>&1) \
		|| fail "$DST exists but can't be read: $current"
	etag=$(jq -r '.ETag' <<< "$current")
	previous_version=$(jq -r '.VersionId // "none"' <<< "$current")
	previous_sha=$(jq -r '.Metadata["gzip-sha256"] // "none"' <<< "$current")
	if holds_build "$current"; then
		log "$DST already holds this build (version $previous_version, SHA-256 $EXPECTED); nothing to copy"
		jq -r '"previous_version=\(.VersionId // "none")\nprevious_gzip_sha256=\(.Metadata["gzip-sha256"] // "none")\nversion=\(.VersionId // "none")\netag=\(.ETag)"' <<< "$current"
		exit 0
	fi
	if [ "$previous_sha" = "$EXPECTED" ]; then
		echo "::warning::promote: $DST version $previous_version records this build but its SHA-256 is" \
			"$(jq -r '.ChecksumSHA256 // "none"' <<< "$current"); replacing it with the verified build" >&2
	fi
	log "replacing $DST version $previous_version (gzip $previous_sha) if its ETag is still $etag"
	if ! out=$(copy --if-match "$etag"); then
		if lost_condition "$out"; then
			echo "::warning::promote: $DST changed after it was read (ETag $etag); another publish won, nothing overwritten" >&2
		fi
		fail "copy to $DST failed: $out"
	fi
fi
version=$(jq -r '.VersionId // "none"' <<< "$out")

# The copy must hold exactly the build's bytes. The roles can't read a given version (no s3:GetObjectVersion), so this
# reads the current object and requires it to be the version the copy created.
new=$(aws s3api head-object --bucket "$BUCKET" --key "$DST" --checksum-mode ENABLED --output json 2>&1) \
	|| fail "can't read back $DST: $new"
current_version=$(jq -r '.VersionId // "none"' <<< "$new")
[ "$current_version" = "$version" ] \
	|| fail "$DST is now version $current_version, not $version that this copy created; another write landed after it"
holds_build "$new" || fail "$DST version $version has SHA-256 $(jq -r '.ChecksumSHA256 // "none"' <<< "$new") \
(recorded $(jq -r '.Metadata["gzip-sha256"] // "none"' <<< "$new")), not the build's $EXPECTED"
log "published $DST version $version, SHA-256 $EXPECTED"
printf 'previous_version=%s\nprevious_gzip_sha256=%s\nversion=%s\netag=%s\n' \
	"$previous_version" "$previous_sha" "$version" "$(jq -r '.ETag' <<< "$new")"
