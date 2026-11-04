#!/usr/bin/env bash
# Signs a DuckDB extension with our own RSA key, in the format DuckDB signs its own extensions: the last 256 bytes of
# the file hold an RSA-SHA256 signature of the composite hash of everything before them (SHA-256 of each 1 MiB chunk,
# then SHA-256 of those digests; duckdb/scripts/compute-extension-hash.sh). A DuckDB that trusts the public key
# (CREATE EXTENSION REPOSITORY ... USING PUBLIC KEY) then loads the extension without allowing unsigned ones.
#
#   scripts/branching/sign_extension.sh <extension> <private_key.pem> [<signed output>]
#
# Without an output path the extension is signed in place. The private key never leaves the given file; CI passes it
# from a secret. Checks the result against the public key derived from the private key.
set -euo pipefail
EXT=${1:?usage: sign_extension.sh <extension> <private_key.pem> [<signed output>]}
KEY=${2:?usage: sign_extension.sh <extension> <private_key.pem> [<signed output>]}
OUT=${3:-$EXT}
HERE=$(cd "$(dirname "$0")/../.." && pwd)
HASH="$HERE/duckdb/scripts/compute-extension-hash.sh"
# pkeyutl with -pkeyopt digest needs OpenSSL 3 (macOS ships LibreSSL)
OPENSSL=${OPENSSL:-$(command -v /opt/homebrew/opt/openssl@3/bin/openssl || command -v openssl)}
SIGNATURE_SIZE=256

size() { stat -c%s "$1" 2>/dev/null || stat -f%z "$1"; }
WORK=$(mktemp -d "${TMPDIR:-/tmp}/sign-extension.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

total=$(size "$EXT")
[ "$total" -gt "$SIGNATURE_SIZE" ] || { echo "$EXT is smaller than a signature footer"; exit 1; }
head -c $((total - SIGNATURE_SIZE)) "$EXT" > "$WORK/unsigned"
bash "$HASH" "$WORK/unsigned" > "$WORK/hash"
"$OPENSSL" pkeyutl -sign -inkey "$KEY" -pkeyopt digest:sha256 -in "$WORK/hash" -out "$WORK/signature"
[ "$(size "$WORK/signature")" -eq "$SIGNATURE_SIZE" ] || { echo "the key must be RSA 2048 (256-byte signatures)"; exit 1; }
cat "$WORK/unsigned" "$WORK/signature" > "$WORK/signed"

"$OPENSSL" pkey -in "$KEY" -pubout -out "$WORK/public.pem"
"$OPENSSL" pkeyutl -verify -pubin -inkey "$WORK/public.pem" -sigfile "$WORK/signature" -in "$WORK/hash" \
	-pkeyopt digest:sha256 > /dev/null
mv "$WORK/signed" "$OUT"
echo "signed $OUT (key $("$OPENSSL" pkey -in "$KEY" -pubout -outform DER | shasum -a 256 | cut -c1-16)...)"
