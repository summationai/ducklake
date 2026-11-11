#!/usr/bin/env bash
# Writes the DXR-18 record of a build or a publication as a markdown table, after checking that every field is there
# and well formed (DXR-18). A missing or
# malformed field fails the step instead of leaving an empty cell in the record.
#
#   scripts/branching/release/record.sh build  >> "$GITHUB_STEP_SUMMARY"
#   scripts/branching/release/record.sh canary >> "$GITHUB_STEP_SUMMARY"
#   scripts/branching/release/record.sh release >> "$GITHUB_STEP_SUMMARY"
#
# The fields come from the environment:
#   both:   GITHUB_SHA, UPSTREAM_BASE (the upstream release commit develop is based on), EXTENSION_VERSION (the
#           version stamped in the build, which DuckDB reports), DUCKDB_VERSION, UNSIGNED_SHA256, SIGNED_SHA256,
#           GZIP_SHA256, KEY_FINGERPRINT
#   build:  GITHUB_REF, OBJECT (s3://bucket/key), OBJECT_VERSION, OBJECT_ETAG
#   canary, release: FROM, TO (s3://bucket/key), PREVIOUS_VERSION, PREVIOUS_SHA256 and PREVIOUS_ETAG (`none` on a first
#           publish), VERSION, ETAG, APPROVED_BY, APPROVED_AT, PUBLISHED_AT (ISO 8601 UTC)
set -uo pipefail
KIND=${1:?usage: record.sh <build|canary|release>}
errors=0
bad() { echo "::error::record: $1" >&2; errors=$((errors + 1)); }
value() { printf '%s' "${!1-}"; }
# need <name> <regex> <what>
need() {
	local v
	v=$(value "$1")
	if [ -z "$v" ]; then
		bad "$1 is missing"
	elif [[ ! $v =~ $2 ]]; then
		bad "$1 is '$v', not $3"
	fi
}
SHA256='^[0-9a-f]{64}$'
COMMIT='^[0-9a-f]{40}$'
ETAG_RE='^"?[0-9a-f]{32}(-[0-9]+)?"?$'
ANY='.'
TIME='^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z$'

need GITHUB_SHA "$COMMIT" "a commit SHA"
need UPSTREAM_BASE "$COMMIT" "a commit SHA"
need EXTENSION_VERSION '^[0-9A-Za-z][0-9A-Za-z._+-]*$' "an extension version"
need DUCKDB_VERSION '^v[0-9]+\.[0-9]+\.[0-9]+$' "a DuckDB release"
for f in UNSIGNED_SHA256 SIGNED_SHA256 GZIP_SHA256 KEY_FINGERPRINT; do need "$f" "$SHA256" "a SHA-256"; done
case $KIND in
build)
	need GITHUB_REF '^refs/' "a ref"
	need OBJECT '^s3://[^/]+/.+' "an s3:// object"
	need OBJECT_VERSION "$ANY" "an object version"
	need OBJECT_ETAG "$ETAG_RE" "an ETag"
	;;
canary | release)
	need FROM '^s3://[^/]+/.+' "an s3:// object"
	need TO '^s3://[^/]+/.+' "an s3:// object"
	need PREVIOUS_VERSION "$ANY" "an object version or none"
	need PREVIOUS_SHA256 '^([0-9a-f]{64}|none)$' "a SHA-256 or none"
	need PREVIOUS_ETAG '^("?[0-9a-f]{32}(-[0-9]+)?"?|none)$' "an ETag or none"
	need VERSION "$ANY" "an object version"
	need ETAG "$ETAG_RE" "an ETag"
	need APPROVED_BY "$ANY" "an approver"
	need APPROVED_AT "$TIME" "an ISO 8601 UTC time"
	need PUBLISHED_AT "$TIME" "an ISO 8601 UTC time"
	;;
*)
	echo "::error::record: unknown record '$KIND' (build, canary or release)" >&2
	exit 1
	;;
esac
[ "$errors" = 0 ] || { echo "::error::record: the $KIND record is incomplete ($errors field(s)); see above" >&2; exit 1; }

BT='`'
row() { printf '| %s | %s%s%s |\n' "$1" "$BT" "$2" "$BT"; }
if [ "$KIND" = build ]; then
	echo "### Build"
else
	echo "### $KIND/"
fi
echo "| | |"
echo "|---|---|"
row commit "$GITHUB_SHA"
[ "$KIND" = build ] && row ref "$GITHUB_REF"
row "upstream base" "$UPSTREAM_BASE"
row "extension version" "$EXTENSION_VERSION"
row DuckDB "$DUCKDB_VERSION"
[ "$KIND" = build ] && row platform linux_arm64
row "unsigned body SHA-256" "$UNSIGNED_SHA256"
row "signed file SHA-256" "$SIGNED_SHA256"
row "gzip SHA-256" "$GZIP_SHA256"
row "key fingerprint (SHA-256 of the DER public key)" "$KEY_FINGERPRINT"
if [ "$KIND" = build ]; then
	row object "$OBJECT"
	row "object version" "$OBJECT_VERSION"
	row "object ETag" "$OBJECT_ETAG"
else
	row from "$FROM"
	row to "$TO"
	row "object version before" "$PREVIOUS_VERSION (gzip $PREVIOUS_SHA256, ETag $PREVIOUS_ETAG)"
	row "object version after" "$VERSION (ETag $ETAG)"
	row "approved by" "$APPROVED_BY"
	row "approved at (the gated job's start)" "$APPROVED_AT"
	row "published at" "$PUBLISHED_AT"
fi
