#!/usr/bin/env bash
# Smoke-tests a signed DuckLake build the way a cluster loads it (DXR-16). It is not DLB-0 qualification.
#
#   scripts/branching/release/smoke.sh <repository prefix> <public_key.pem> <signed ducklake.duckdb_extension.gz>
#
# With an official DuckDB CLI (DUCKDB; no ducklake built in, allow_unsigned_extensions=false):
#   1. CREATE EXTENSION REPOSITORY <prefix> USING PUBLIC KEY, then INSTALL and LOAD ducklake FROM it;
#   2. ducklake_branches exists; a branch is created, written and merged, and another created, written and dropped,
#      on a scratch catalog;
#   3. the same file re-signed with another key, and a tampered copy, both served under the public key from a local
#      repository on 127.0.0.1, are refused.
# The prefix is where <DuckDB version>/<platform>/ducklake.duckdb_extension.gz is served: the build's S3 prefix, or a
# local mirror of it. Every DuckDB process runs with HOME in a scratch dir, so ~/.duckdb is never touched.
set -uo pipefail
usage="usage: smoke.sh <repository prefix> <public_key.pem> <signed ducklake.duckdb_extension.gz>"
PREFIX=${1:?$usage}
PUB_FILE=${2:?$usage}
SIGNED_GZ=${3:?$usage}
DUCKDB=${DUCKDB:?DUCKDB must be the official DuckDB CLI}
DUCKDB_VERSION=${DUCKDB_VERSION:-v2.0.0}
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
HASH=${EXTENSION_HASH_SCRIPT:-$ROOT/duckdb/scripts/compute-extension-hash.sh}
OPENSSL=${OPENSSL:-openssl}
PORT=${SMOKE_REFUSAL_PORT:-18766}
WORK=$(mktemp -d "${TMPDIR:-/tmp}/smoke.XXXXXX")
SERVER=
trap '[ -n "$SERVER" ] && kill "$SERVER" 2>/dev/null; rm -rf "$WORK"' EXIT

fail() { echo "::error::smoke: $*" >&2; exit 1; }
# duck <home>: runs stdin in a fresh DuckDB process, stopping at the first error
duck() { mkdir -p "$1"; HOME=$1 "$DUCKDB" -no-agent -bail -list -noheader :memory:; }
expect() { # <what> <expected> <actual>
	if [ "$2" = "$3" ]; then
		echo "ok    $1"
	else
		printf '%s\n--- expected\n%s\n--- got\n%s\n' "::error::smoke: $1" "$2" "$3" >&2
		exit 1
	fi
}
PUB=$(cat "$PUB_FILE") || fail "cannot read $PUB_FILE"

# 0. the CLI is a plain DuckDB of the right version that refuses unsigned extensions
got=$(duck "$WORK/h0" << 'SQL' 2>&1
SELECT library_version FROM pragma_version();
SELECT current_setting('allow_unsigned_extensions');
SELECT count(*) FROM duckdb_extensions() WHERE extension_name = 'ducklake' AND (loaded OR install_mode = 'STATICALLY_LINKED');
SQL
)
expect "official DuckDB $DUCKDB_VERSION, unsigned extensions refused, no ducklake built in" \
	"$(printf '%s\nfalse\n0' "$DUCKDB_VERSION")" "$got"
PLATFORM=$(duck "$WORK/h0" <<< "PRAGMA platform;")

# 1-2. install from the repository, then branch on a scratch catalog
H=$WORK/h1
echo "installing from $PREFIX"
# INSTALL over https needs httpfs: it is installed from DuckDB's own (signed) repository before autoinstall is turned off
got=$(duck "$H" 2>&1 << SQL
INSTALL httpfs;
LOAD httpfs;
SELECT 'httpfs loaded from ' || installed_from FROM duckdb_extensions() WHERE extension_name = 'httpfs' AND loaded;
SET autoinstall_known_extensions = false;
SET allow_extension_repositories = 'allowed';
CREATE EXTENSION REPOSITORY smoke WITH PREFIX '$PREFIX' USING PUBLIC KEY '$PUB';
INSTALL ducklake FROM smoke;
LOAD ducklake FROM smoke;
SELECT 'loaded, install mode ' || install_mode || ', allow_unsigned_extensions=' || current_setting('allow_unsigned_extensions')
  FROM duckdb_extensions() WHERE extension_name = 'ducklake' AND loaded;
ATTACH 'ducklake:$H/lake.ducklake' AS lake (DATA_PATH '$H/lake_files', DATA_INLINING_ROW_LIMIT 0);
USE lake;
SELECT 'branches before: ' || count(*) FROM ducklake_branches('lake');
CREATE TABLE prices(item VARCHAR, price INTEGER);
INSERT INTO prices VALUES ('Jacket', 100), ('Boots', 80), ('Scarf', 20);
CREATE BRANCH merged FROM main;
SET BRANCH merged;
UPDATE prices SET price = 110 WHERE item = 'Jacket';
INSERT INTO prices VALUES ('Hat', 25);
DELETE FROM prices WHERE item = 'Scarf';
SET BRANCH main;
SELECT 'main before merge: ' || string_agg(item || '=' || price, ',' ORDER BY item) FROM prices;
MERGE BRANCH merged;
SELECT 'main after merge: ' || string_agg(item || '=' || price, ',' ORDER BY item) FROM prices;
CREATE BRANCH dropped FROM main;
SET BRANCH dropped;
INSERT INTO prices VALUES ('Gloves', 15);
SET BRANCH main;
DROP BRANCH dropped;
SELECT 'main after drop: ' || string_agg(item || '=' || price, ',' ORDER BY item) FROM prices;
SELECT 'branches after: ' || count(*) FROM ducklake_branches('lake');
SQL
)
# SET BRANCH echoes the branch name, and MERGE BRANCH its result row; only the labelled lines are checked
expect "installs and loads from the repository, then creates, writes, merges and drops branches" \
	"httpfs loaded from core
loaded, install mode REPOSITORY, allow_unsigned_extensions=false
branches before: 0
main before merge: Boots=80,Jacket=100,Scarf=20
main after merge: Boots=80,Hat=25,Jacket=110
main after drop: Boots=80,Hat=25,Jacket=110
branches after: 0" "$(grep -E '^(httpfs|loaded|branches|main)[ ,]' <<< "$got" || echo "$got")"

# 3. refusals, from a local repository that serves the bad copies under the same public key
[ -f "$HASH" ] || fail "no composite hash script at $HASH (is the duckdb submodule checked out?)"
gzip -dc "$SIGNED_GZ" > "$WORK/signed" || fail "cannot read $SIGNED_GZ"
n=$(wc -c < "$WORK/signed" | tr -d ' ')
head -c $((n - 256)) "$WORK/signed" > "$WORK/body"
"$OPENSSL" genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 -out "$WORK/other.pem" 2> /dev/null
bash "$HASH" "$WORK/body" > "$WORK/hash"
"$OPENSSL" pkeyutl -sign -inkey "$WORK/other.pem" -pkeyopt digest:sha256 -in "$WORK/hash" -out "$WORK/other.sig"
for name in foreign tampered; do mkdir -p "$WORK/www/$name/$DUCKDB_VERSION/$PLATFORM"; done
cat "$WORK/body" "$WORK/other.sig" | gzip -n > "$WORK/www/foreign/$DUCKDB_VERSION/$PLATFORM/ducklake.duckdb_extension.gz"
# one byte flipped in the middle of the body; the signature is left as it was
python3 -I -c '
import sys
with open(sys.argv[1], "r+b") as f:
    f.seek(int(sys.argv[2])); b = f.read(1); f.seek(int(sys.argv[2])); f.write(bytes([b[0] ^ 0xFF]))' \
	"$WORK/signed" $((n / 2))
gzip -n < "$WORK/signed" > "$WORK/www/tampered/$DUCKDB_VERSION/$PLATFORM/ducklake.duckdb_extension.gz"
python3 -I -m http.server "$PORT" --bind 127.0.0.1 --directory "$WORK/www" > "$WORK/http.log" 2>&1 &
SERVER=$!
disown "$SERVER"
for _ in $(seq 50); do curl -sf -o /dev/null "http://127.0.0.1:$PORT/foreign/" && break; sleep 0.1; done

for name in foreign tampered; do
	if got=$(duck "$WORK/h-$name" 2>&1 << SQL
SET autoinstall_known_extensions = false;
SET allow_extension_repositories = 'allowed';
CREATE EXTENSION REPOSITORY ${name}_copy WITH PREFIX 'http://127.0.0.1:$PORT/$name' USING PUBLIC KEY '$PUB';
INSTALL ducklake FROM ${name}_copy;
LOAD ducklake FROM ${name}_copy;
SQL
	); then
		fail "the $name copy was installed and loaded: $got"
	elif ! grep -qi signature <<< "$got"; then
		fail "the $name copy failed for another reason than its signature: $got"
	fi
	echo "ok    refuses the $name copy: $(grep -i -m1 signature <<< "$got")"
done
echo "smoke test passed for $PREFIX"
