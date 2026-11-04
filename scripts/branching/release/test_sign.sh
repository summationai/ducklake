#!/usr/bin/env bash
# Tests sign.sh (DXR-9) without KMS: a stub `aws` signs with a throwaway RSA key the way kms:Sign does for a DIGEST
# with RSASSA_PKCS1_V1_5_SHA_256, and checks that sign.sh asks for exactly that. A public key of another key, a failing
# or short signature, and a missing key ARN must each fail and leave no output.
#
#   scripts/branching/release/test_sign.sh
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
SIGN="$HERE/sign.sh"
ROOT=$(cd "$HERE/../../.." && pwd)
HASH=${EXTENSION_HASH_SCRIPT:-$ROOT/duckdb/scripts/compute-extension-hash.sh}
export EXTENSION_HASH_SCRIPT=$HASH
OPENSSL=${OPENSSL:-openssl}
export OPENSSL
WORK=$(mktemp -d "${TMPDIR:-/tmp}/test-sign.XXXXXX")
trap 'rm -rf "$WORK"' EXIT
failures=0
pass() { echo "ok    $1"; }
flunk() { echo "FAIL  $1"; failures=$((failures + 1)); }

"$OPENSSL" genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 -out "$WORK/key.pem" 2> /dev/null
"$OPENSSL" pkey -in "$WORK/key.pem" -pubout -out "$WORK/key.pub.pem"
"$OPENSSL" genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 -out "$WORK/other.pem" 2> /dev/null
"$OPENSSL" pkey -in "$WORK/other.pem" -pubout -out "$WORK/other.pub.pem"

# A 2.5 MiB body (three hash chunks), a 256-byte metadata footer and an empty signature slot, like a fresh build
head -c 2621440 /dev/urandom > "$WORK/ext"
head -c 256 /dev/urandom >> "$WORK/ext"
head -c 256 /dev/zero >> "$WORK/ext"

# kms:Sign stub: refuses any other request shape, then signs the 32-byte digest with STUB_KEY
mkdir "$WORK/bin"
cat > "$WORK/bin/aws" << 'EOF'
#!/usr/bin/env bash
set -euo pipefail
echo "$*" >> "$STUB_LOG"
[ "$STUB_MODE" = fail ] && { echo "An error occurred (AccessDeniedException)" >&2; exit 254; }
[ "$1 $2" = "kms sign" ] || { echo "unexpected aws $1 $2" >&2; exit 2; }
shift 2
while [ $# -gt 0 ]; do
	case "$1" in
	--key-id) key=$2 ;;
	--message-type) type=$2 ;;
	--signing-algorithm) alg=$2 ;;
	--message) msg=${2#fileb://} ;;
	--query | --output) ;;
	*) echo "unexpected argument $1" >&2; exit 2 ;;
	esac
	shift 2
done
[ "$key" = "$STUB_KEY_ARN" ] && [ "$type" = DIGEST ] && [ "$alg" = RSASSA_PKCS1_V1_5_SHA_256 ] || { echo "bad request" >&2; exit 2; }
[ "$(wc -c < "$msg" | tr -d ' ')" -eq 32 ] || { echo "a DIGEST must be 32 bytes" >&2; exit 2; }
if [ "$STUB_MODE" = short ]; then head -c 128 /dev/urandom | base64; exit 0; fi
"$OPENSSL" pkeyutl -sign -inkey "$STUB_KEY" -pkeyopt digest:sha256 -in "$msg" | base64
EOF
chmod +x "$WORK/bin/aws"
export PATH="$WORK/bin:$PATH" STUB_LOG="$WORK/aws.log" STUB_KEY="$WORK/key.pem" STUB_KEY_ARN=arn:aws:kms:us-west-2:000000000000:key/test

run() { # <mode> <public key> <output>; sign.sh's exit status
	STUB_MODE=$1 SIGNING_KEY_ARN=${ARN-$STUB_KEY_ARN} bash "$SIGN" "$WORK/ext" "$2" "$3" > "$WORK/out.log" 2>&1
}

# 1. A good signature: same size, same body, verifies with the throwaway public key through DuckDB's composite hash
if run ok "$WORK/key.pub.pem" "$WORK/signed"; then
	n=$(wc -c < "$WORK/ext" | tr -d ' ')
	head -c $((n - 256)) "$WORK/signed" > "$WORK/body"
	tail -c 256 "$WORK/signed" > "$WORK/sig"
	bash "$HASH" "$WORK/body" > "$WORK/hash"
	if [ "$(wc -c < "$WORK/signed" | tr -d ' ')" -eq "$n" ] && cmp -s "$WORK/body" <(head -c $((n - 256)) "$WORK/ext") \
		&& "$OPENSSL" pkeyutl -verify -pubin -inkey "$WORK/key.pub.pem" -sigfile "$WORK/sig" -in "$WORK/hash" \
			-pkeyopt digest:sha256 > /dev/null 2>&1; then
		pass "signs the composite hash; the signed file verifies and keeps the body"
	else
		flunk "the signed file does not verify or changed the body"
	fi
	if grep -q -- '--message-type DIGEST --signing-algorithm RSASSA_PKCS1_V1_5_SHA_256' "$STUB_LOG"; then
		pass "asks KMS for a DIGEST signature with RSASSA_PKCS1_V1_5_SHA_256"
	else
		flunk "unexpected kms:Sign request"
	fi
else
	flunk "signing failed: $(cat "$WORK/out.log")"
fi

# 2-5. Each must fail and write nothing
expect_refusal() { # <name> <mode> <public key> <log pattern>
	rm -f "$WORK/refused"
	if run "$2" "$3" "$WORK/refused"; then
		flunk "$1: sign.sh succeeded"
	elif [ -e "$WORK/refused" ]; then
		flunk "$1: sign.sh failed but left an output file"
	elif ! grep -q "$4" "$WORK/out.log"; then
		flunk "$1: no '$4' in: $(cat "$WORK/out.log")"
	else
		pass "$1"
	fi
}
expect_refusal "refuses a signature that does not match the committed public key" ok "$WORK/other.pub.pem" "does not verify"
expect_refusal "fails when kms:Sign fails" fail "$WORK/key.pub.pem" "kms:Sign failed"
expect_refusal "refuses a signature that is not 256 bytes" short "$WORK/key.pub.pem" "not 256"
ARN="" expect_refusal "refuses to run without SIGNING_KEY_ARN" ok "$WORK/key.pub.pem" "SIGNING_KEY_ARN"

echo
echo "$failures failure(s)"
exit $((failures > 0))
