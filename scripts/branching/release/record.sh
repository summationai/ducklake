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
#   both:   GITHUB_SHA, DUCKDB_VERSION, UNSIGNED_SHA256, SIGNED_SHA256, GZIP_SHA256, KEY_FINGERPRINT
#   build:  GITHUB_REF, OBJECT (s3://bucket/key), OBJECT_VERSION
#   canary, release: FROM, TO (s3://bucket/key), PREVIOUS_VERSION and PREVIOUS_SHA256 (`none` on a first publish), VERSION,
#           APPROVED_BY, APPROVED_AT, PUBLISHED_AT (ISO 8601 UTC)
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
ANY='.'
TIME='^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z$'

need GITHUB_SHA '^[0-9a-f]{40}$' "a commit SHA"
need DUCKDB_VERSION '^v[0-9]+\.[0-9]+\.[0-9]+$' "a DuckDB release"
for f in UNSIGNED_SHA256 SIGNED_SHA256 GZIP_SHA256 KEY_FINGERPRINT; do need "$f" "$SHA256" "a SHA-256"; done
case $KIND in
build)
	need GITHUB_REF '^refs/' "a ref"
	need OBJECT '^s3://[^/]+/.+' "an s3:// object"
	need OBJECT_VERSION "$ANY" "an object version"
	;;
canary | release)
	need FROM '^s3://[^/]+/.+' "an s3:// object"
	need TO '^s3://[^/]+/.+' "an s3:// object"
	need PREVIOUS_VERSION "$ANY" "an object version or none"
	need PREVIOUS_SHA256 '^([0-9a-f]{64}|none)$' "a SHA-256 or none"
	need VERSION "$ANY" "an object version"
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
row DuckDB "$DUCKDB_VERSION"
[ "$KIND" = build ] && row platform linux_arm64
row "unsigned body SHA-256" "$UNSIGNED_SHA256"
row "signed file SHA-256" "$SIGNED_SHA256"
row "gzip SHA-256" "$GZIP_SHA256"
row "key fingerprint (SHA-256 of the DER public key)" "$KEY_FINGERPRINT"
if [ "$KIND" = build ]; then
	row object "$OBJECT"
	row "object version" "$OBJECT_VERSION"
else
	row from "$FROM"
	row to "$TO"
	row "object version before" "$PREVIOUS_VERSION (gzip $PREVIOUS_SHA256)"
	row "object version after" "$VERSION"
	row "approved by" "$APPROVED_BY"
	row "approved at (the gated job's start)" "$APPROVED_AT"
	row "published at" "$PUBLISHED_AT"
fi
