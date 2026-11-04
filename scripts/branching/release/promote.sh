#!/usr/bin/env bash
# Publishes a build to a channel (canary/ or release/) without ever losing an update (spec: summationai/agent-cloud
# docs/specs/ducklake-extension-release.md, DXR-12 and DXR-15; comment #6). The spec's ifGenerationMatch is S3's
# conditional copy here:
#   1. copy build/<sha>/... to the channel with If-None-Match: *, which only succeeds on a first publish;
#   2. if an object is there (412), read its ETag and copy again with If-Match on that ETag;
#   3. a 412 or 409 on that copy means something else wrote in between: the copy fails and nothing is overwritten.
# The roles can't list the bucket, so a missing object can't be told from a forbidden one by reading it first; the
# copy goes first and any error other than a lost condition fails closed. After the copy, the new object's SHA-256
# (S3's own checksum and the build's recorded digest) must equal the build's.
#
#   scripts/branching/release/promote.sh <bucket> <build key> <channel key> <expected gzip sha256>
#
# Logs go to stderr. On success it prints, for $GITHUB_OUTPUT: previous_version, previous_gzip_sha256, version, etag.
set -euo pipefail
usage="usage: promote.sh <bucket> <build key> <channel key> <expected gzip sha256>"
BUCKET=${1:?$usage}
SRC=${2:?$usage}
DST=${3:?$usage}
EXPECTED=${4:?$usage}
[[ $EXPECTED =~ ^[0-9a-f]{64}$ ]] || { echo "::error::promote: '$EXPECTED' is not a SHA-256" >&2; exit 1; }

log() { echo "$*" >&2; }
fail() { echo "::error::promote: $*" >&2; exit 1; }
# copy <extra args...>: prints the response, or the error text with a non-zero status
copy() {
	aws s3api copy-object --bucket "$BUCKET" --copy-source "$BUCKET/$SRC" --key "$DST" \
		--metadata-directive COPY --checksum-algorithm SHA256 "$@" --output json 2>&1
}
lost_condition() { grep -q -E 'PreconditionFailed|\(412\)|ConditionalRequestConflict|\(409\)' <<< "$1"; }
b64_of_hex() { python3 -I -c 'import base64, sys; print(base64.b64encode(bytes.fromhex(sys.argv[1])).decode())' "$1"; }

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
	current=$(aws s3api head-object --bucket "$BUCKET" --key "$DST" --output json 2>&1) \
		|| fail "$DST exists but can't be read: $current"
	etag=$(jq -r '.ETag' <<< "$current")
	previous_version=$(jq -r '.VersionId // "none"' <<< "$current")
	previous_sha=$(jq -r '.Metadata["gzip-sha256"] // "none"' <<< "$current")
	if [ "$previous_sha" = "$EXPECTED" ]; then
		log "$DST already holds this build (version $previous_version); nothing to copy"
		jq -r '"previous_version=\(.VersionId // "none")\nprevious_gzip_sha256=\(.Metadata["gzip-sha256"] // "none")\nversion=\(.VersionId // "none")\netag=\(.ETag)"' <<< "$current"
		exit 0
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
checksum=$(jq -r '.ChecksumSHA256 // empty' <<< "$new")
recorded=$(jq -r '.Metadata["gzip-sha256"] // empty' <<< "$new")
[ "$checksum" = "$(b64_of_hex "$EXPECTED")" ] && [ "$recorded" = "$EXPECTED" ] \
	|| fail "$DST version $version has SHA-256 $checksum (recorded $recorded), not the build's $EXPECTED"
log "published $DST version $version, SHA-256 $EXPECTED"
printf 'previous_version=%s\nprevious_gzip_sha256=%s\nversion=%s\netag=%s\n' \
	"$previous_version" "$previous_sha" "$version" "$(jq -r '.ETag' <<< "$new")"
