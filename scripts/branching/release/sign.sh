#!/usr/bin/env bash
# Signs a DuckDB extension with a KMS key, in the format DuckDB signs its own extensions (DXR-9): the last 256 bytes of
# the file hold an RSA-SHA256 signature of the composite hash of everything before them
# (duckdb/scripts/compute-extension-hash.sh). KMS signs that hash as a DIGEST, so the private key never leaves KMS.
#
#   scripts/branching/release/sign.sh <built extension> <public_key.pem> <signed output>
#
# SIGNING_KEY_ARN names the KMS key; the AWS credentials come from the environment (the signer role). The result is
# checked against the committed public key of the same key before it is written, so a wrong key, a wrong algorithm or
# a damaged signature fails here and nothing is uploaded. On any failure no output file is left behind.
set -euo pipefail
usage="usage: sign.sh <built extension> <public_key.pem> <signed output>"
EXT=${1:?$usage}
PUB=${2:?$usage}
OUT=${3:?$usage}
: "${SIGNING_KEY_ARN:?SIGNING_KEY_ARN must name the KMS signing key}"
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
HASH=${EXTENSION_HASH_SCRIPT:-$ROOT/duckdb/scripts/compute-extension-hash.sh}
OPENSSL=${OPENSSL:-openssl}
SIGNATURE_SIZE=256

size() { stat -c%s "$1" 2>/dev/null || stat -f%z "$1"; }
fail() { echo "::error::sign.sh: $*" >&2; exit 1; }
WORK=$(mktemp -d "${TMPDIR:-/tmp}/sign-extension.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

[ -f "$EXT" ] || fail "$EXT does not exist"
[ -f "$PUB" ] || fail "public key $PUB does not exist"
[ -f "$HASH" ] || fail "no composite hash script at $HASH (is the duckdb submodule checked out?)"
total=$(size "$EXT")
[ "$total" -gt $((2 * SIGNATURE_SIZE)) ] || fail "$EXT ($total bytes) is too small to hold the metadata and signature footer"

# Everything before the signature slot is signed, the metadata footer included
head -c $((total - SIGNATURE_SIZE)) "$EXT" > "$WORK/unsigned"
bash "$HASH" "$WORK/unsigned" > "$WORK/hash"
[ "$(size "$WORK/hash")" -eq 32 ] || fail "the composite hash is $(size "$WORK/hash") bytes, not 32"
echo "composite hash $(od -An -tx1 "$WORK/hash" | tr -d ' \n') of $((total - SIGNATURE_SIZE)) bytes"

echo "signing with $SIGNING_KEY_ARN (DIGEST, RSASSA_PKCS1_V1_5_SHA_256)"
aws kms sign --key-id "$SIGNING_KEY_ARN" --message-type DIGEST --signing-algorithm RSASSA_PKCS1_V1_5_SHA_256 \
	--message "fileb://$WORK/hash" --query Signature --output text > "$WORK/signature.b64" \
	|| fail "kms:Sign failed"
base64 -d < "$WORK/signature.b64" > "$WORK/signature" || fail "kms:Sign returned a signature that is not base64"
[ "$(size "$WORK/signature")" -eq "$SIGNATURE_SIZE" ] \
	|| fail "the signature is $(size "$WORK/signature") bytes, not $SIGNATURE_SIZE (the key must be RSA 2048)"

# DXR-9: checked against the committed public key before anything is written or uploaded
"$OPENSSL" pkeyutl -verify -pubin -inkey "$PUB" -sigfile "$WORK/signature" -in "$WORK/hash" -pkeyopt digest:sha256 \
	> /dev/null 2>&1 || fail "the KMS signature does not verify against $PUB; is SIGNING_KEY_ARN the key of that public key?"

cat "$WORK/unsigned" "$WORK/signature" > "$WORK/signed"
mv "$WORK/signed" "$OUT"
echo "signed $OUT, verified against $PUB (key $("$OPENSSL" pkey -pubin -in "$PUB" -outform DER | sha256sum | cut -c1-16)...)"
