#!/usr/bin/env bash
# Checks extensions against our public key; exits non-zero if any is unsigned, tampered or signed with another key.
# (duckdb/scripts/verify-extension-signing.sh reports success even when the check fails: its openssl call runs inside
# an `if ! { ...; }` block, where set -e does not apply.)
#
#   scripts/branching/verify_extension_signature.sh <public_key.pem> <extension> [<extension> ...]
set -uo pipefail
PUB=${1:?usage: verify_extension_signature.sh <public_key.pem> <extension> ...}
shift
HERE=$(cd "$(dirname "$0")/../.." && pwd)
HASH="$HERE/duckdb/scripts/compute-extension-hash.sh"
OPENSSL=${OPENSSL:-$(command -v /opt/homebrew/opt/openssl@3/bin/openssl || command -v openssl)}
size() { stat -c%s "$1" 2>/dev/null || stat -f%z "$1"; }
WORK=$(mktemp -d "${TMPDIR:-/tmp}/verify-extension.XXXXXX")
trap 'rm -rf "$WORK"' EXIT
bad=0
for ext in "$@"; do
	n=$(size "$ext")
	head -c $((n - 256)) "$ext" > "$WORK/unsigned"
	tail -c 256 "$ext" > "$WORK/signature"
	bash "$HASH" "$WORK/unsigned" > "$WORK/hash"
	if "$OPENSSL" pkeyutl -verify -pubin -inkey "$PUB" -sigfile "$WORK/signature" -in "$WORK/hash" \
		-pkeyopt digest:sha256 > /dev/null 2>&1; then
		echo "valid    $ext"
	else
		echo "INVALID  $ext"
		bad=1
	fi
done
exit $bad
