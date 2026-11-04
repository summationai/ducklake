#!/usr/bin/env bash
# Proves on one machine that a DuckDB 2.0 trusts our DuckLake build through its own signature, with unsigned extensions
# still refused: signs the build with a throwaway RSA key (and a second key and a tampered copy, which must be refused),
# serves the files over http on 127.0.0.1 in the layout of an S3 repository, then installs and loads through
# CREATE EXTENSION REPOSITORY ... USING PUBLIC KEY and runs a branch, a merge and the refusal cases.
#
#   scripts/branching/prove_signed_install.sh <work dir>
#
# DUCKDB must be a DuckDB without ducklake built in (default: duckdb/build/release/duckdb, a plain build of the
# submodule, made with OVERRIDE_GIT_DESCRIBE set to the version the extension was built for). Every DuckDB process runs
# with HOME inside the work dir, so ~/.duckdb is never read or written. The keys are generated in the work dir.
set -uo pipefail

W=${1:?usage: prove_signed_install.sh <work dir>}
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
DUCKDB=${DUCKDB:-$ROOT/duckdb/build/release/duckdb}
EXT=${EXT:-$ROOT/build/release/extension/ducklake/ducklake.duckdb_extension}
# pkeyutl with -pkeyopt digest needs OpenSSL 3 (macOS ships LibreSSL)
OPENSSL=${OPENSSL:-$(command -v /opt/homebrew/opt/openssl@3/bin/openssl || command -v openssl)}
PORT=${PORT:-18765}
URL=http://127.0.0.1:$PORT
mkdir -p "$W"
VER=$(HOME=$W "$DUCKDB" -no-agent -csv -noheader :memory: -c "SELECT library_version FROM pragma_version()")
PLAT=$(HOME=$W "$DUCKDB" -no-agent -csv -noheader :memory: -c "PRAGMA platform")

R=$W/run-$(date +%Y%m%d-%H%M%S)
mkdir -p "$R"
echo "run dir: $R"

step() { printf '\n######## %s\n' "$*"; }
# sql <home> : runs stdin in a fresh DuckDB process; errors are printed and the script continues
sql() { mkdir -p "$1"; HOME=$1 "$DUCKDB" -no-agent -csv :memory: 2>&1; }

step "0. inputs"
HOME=$R/h0 "$DUCKDB" -no-agent -csv :memory: -c "PRAGMA version; PRAGMA platform;" 2>&1
tail -c 512 "$EXT" | tr -c '[:print:]' ' ' | tr -s ' '; echo

step "1. keys (RSA 2048; private keys stay in $R/keys, mode 600)"
mkdir -m 700 "$R/keys"
(umask 077
 "$OPENSSL" genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 -out "$R/keys/agc_private.pem" 2>/dev/null
 "$OPENSSL" genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 -out "$R/keys/other_private.pem" 2>/dev/null)
"$OPENSSL" pkey -in "$R/keys/agc_private.pem" -pubout -out "$R/keys/agc_public.pem"
ls -l "$R/keys" | awk 'NR>1{print $1, $NF}'
PUB=$(cat "$R/keys/agc_public.pem")

step "2. sign (ours, a foreign key, and a tampered copy of ours)"
mkdir -p "$R/build"
"$ROOT/scripts/branching/sign_extension.sh" "$EXT" "$R/keys/agc_private.pem" "$R/build/signed.duckdb_extension"
"$ROOT/scripts/branching/sign_extension.sh" "$EXT" "$R/keys/other_private.pem" "$R/build/foreign.duckdb_extension"
cp "$R/build/signed.duckdb_extension" "$R/build/tampered.duckdb_extension"
python3 -I -c '
import sys
p, off = sys.argv[1], 20_000_000
with open(p, "r+b") as f:
    f.seek(off); b = f.read(1); f.seek(off); f.write(bytes([b[0] ^ 0xFF]))
print(f"flipped byte at {off} of {p}")' "$R/build/tampered.duckdb_extension"
"$ROOT/scripts/branching/verify_extension_signature.sh" "$R/keys/agc_public.pem" \
	"$R/build/signed.duckdb_extension" "$R/build/foreign.duckdb_extension" "$R/build/tampered.duckdb_extension" "$EXT"
echo "verify exit: $? (1 expected: only signed is valid)"

step "3. repositories, served over http on 127.0.0.1:$PORT (the S3 layout)"
for name in agc foreign tampered; do mkdir -p "$R/www/$name/$VER/$PLAT"; done
gzip -c "$R/build/signed.duckdb_extension" > "$R/www/agc/$VER/$PLAT/ducklake.duckdb_extension.gz"
gzip -c "$R/build/foreign.duckdb_extension" > "$R/www/foreign/$VER/$PLAT/ducklake.duckdb_extension.gz"
gzip -c "$R/build/tampered.duckdb_extension" > "$R/www/tampered/$VER/$PLAT/ducklake.duckdb_extension.gz"
mkdir -p "$R/www/agc/.well-known"
python3 -I -c 'import json, sys; print(json.dumps({"signature_keys": [open(sys.argv[1]).read()]}, indent=2))' \
	"$R/keys/agc_public.pem" > "$R/www/agc/.well-known/duckdb-extension-repo.json"
(cd "$R/www" && find . -type f | sort)
python3 -I -m http.server "$PORT" --bind 127.0.0.1 --directory "$R/www" > "$R/http.log" 2>&1 &
SRV=$!
trap 'kill $SRV 2>/dev/null' EXIT
for _ in $(seq 50); do curl -sf "$URL/agc/.well-known/duckdb-extension-repo.json" > /dev/null && break; sleep 0.1; done

step "T1. default policy: adding a repository is refused without opting in"
sql "$R/h1" <<SQL
SELECT current_setting('allow_extension_repositories') AS allow_extension_repositories;
CREATE EXTENSION REPOSITORY agc WITH PREFIX '$URL/agc' USING PUBLIC KEY '$PUB';
SQL

step "T2. install + load our build from the agc repository, then branch on it"
H2=$R/h2
sql "$H2" <<SQL
SET autoinstall_known_extensions = false;
SET allow_extension_repositories = 'allowed';
CREATE EXTENSION REPOSITORY agc WITH PREFIX '$URL/agc' USING PUBLIC KEY '$PUB';
SELECT repository_name, prefix, type, key_fingerprints FROM duckdb_extension_repositories() WHERE type = 'USER_PROVIDED';
INSTALL ducklake FROM agc;
LOAD ducklake FROM agc;
SELECT extension_name, loaded, installed, install_mode, installed_from, extension_version, install_path
  FROM duckdb_extensions() WHERE extension_name = 'ducklake';
SELECT current_setting('allow_unsigned_extensions') AS allow_unsigned_extensions;
SET ducklake_max_retry_count = 5;
ATTACH 'ducklake:$H2/lake.ducklake' AS lake (DATA_PATH '$H2/lake_files', DATA_INLINING_ROW_LIMIT 0);
USE lake;
CREATE TABLE prices(item VARCHAR, price INTEGER);
INSERT INTO prices VALUES ('Jacket', 100), ('Boots', 80), ('Scarf', 20);
CREATE BRANCH analysis FROM main;
SET BRANCH analysis;
UPDATE prices SET price = 110 WHERE item = 'Jacket';
INSERT INTO prices VALUES ('Hat', 25);
DELETE FROM prices WHERE item = 'Scarf';
SELECT 'on branch' AS side, item, price FROM prices ORDER BY item;
SET BRANCH main;
SELECT 'on main' AS side, item, price FROM prices ORDER BY item;
MERGE BRANCH analysis;
SELECT 'merged' AS side, item, price FROM prices ORDER BY item;
SQL

step "T3. restart with the same HOME: the pinned repository is still trusted (setting back to 'undecided')"
sql "$H2" <<SQL
SET autoinstall_known_extensions = false;
SELECT current_setting('allow_extension_repositories') AS allow_extension_repositories;
LOAD ducklake FROM agc;
SELECT extension_name, loaded, installed_from, extension_version FROM duckdb_extensions() WHERE extension_name = 'ducklake';
ATTACH 'ducklake:$H2/lake.ducklake' AS lake (DATA_PATH '$H2/lake_files');
SELECT item, price FROM lake.prices ORDER BY item;
SQL

step "T4a. a bare LOAD does not pick up our build (only an explicit FROM agc does)"
sql "$H2" <<SQL
SET autoinstall_known_extensions = false;
LOAD ducklake;
SQL

step "T4b. a ducklake_* setting before our LOAD autoloads the stock name; autoinstall off makes it fail, not fetch"
sql "$H2" <<SQL
SET autoinstall_known_extensions = false;
SET ducklake_max_retry_count = 5;
SELECT extension_name, loaded, installed_from FROM duckdb_extensions() WHERE extension_name = 'ducklake';
SQL

step "T4c. a ducklake ATTACH before our LOAD: same autoload path"
sql "$H2" <<SQL
SET autoinstall_known_extensions = false;
ATTACH 'ducklake:$H2/lake.ducklake' AS lake (DATA_PATH '$H2/lake_files');
SQL

step "T4d. once ours is loaded, a later bare LOAD is a no-op and keeps ours"
sql "$H2" <<SQL
SET autoinstall_known_extensions = false;
LOAD ducklake FROM agc;
LOAD ducklake;
SELECT extension_name, loaded, installed_from, extension_version FROM duckdb_extensions() WHERE extension_name = 'ducklake';
SQL

step "T5. tampered file, served under our key: refused"
sql "$R/h5" <<SQL
SET autoinstall_known_extensions = false;
SET allow_extension_repositories = 'allowed';
CREATE EXTENSION REPOSITORY tampered WITH PREFIX '$URL/tampered' USING PUBLIC KEY '$PUB';
INSTALL ducklake FROM tampered;
LOAD ducklake FROM tampered;
SELECT extension_name, loaded, installed FROM duckdb_extensions() WHERE extension_name = 'ducklake';
SQL

step "T6. file signed with another key, served under our key: refused"
sql "$R/h6" <<SQL
SET autoinstall_known_extensions = false;
SET allow_extension_repositories = 'allowed';
CREATE EXTENSION REPOSITORY foreign_signed WITH PREFIX '$URL/foreign' USING PUBLIC KEY '$PUB';
INSTALL ducklake FROM foreign_signed;
LOAD ducklake FROM foreign_signed;
SELECT extension_name, loaded, installed FROM duckdb_extensions() WHERE extension_name = 'ducklake';
SQL

step "T7. key pinned from the repository's .well-known metadata (no key in the statement; the fetch needs httpfs)"
sql "$R/h7" <<SQL
SET autoinstall_known_extensions = false;
SET allow_extension_repositories = 'allowed';
CREATE EXTENSION REPOSITORY agc WITH PREFIX '$URL/agc';
SELECT repository_name, type, key_fingerprints FROM duckdb_extension_repositories() WHERE type = 'USER_PROVIDED';
INSTALL ducklake FROM agc;
LOAD ducklake FROM agc;
SELECT extension_name, loaded, installed_from, extension_version FROM duckdb_extensions() WHERE extension_name = 'ducklake';
SQL

step "T8. unsigned build and our signed file loaded by path: both refused (our key is trusted only for agc)"
sql "$R/h8" <<SQL
SET autoinstall_known_extensions = false;
LOAD '$EXT';
LOAD '$R/build/signed.duckdb_extension';
SELECT extension_name, loaded FROM duckdb_extensions() WHERE extension_name = 'ducklake';
SQL

step "T9. allow_extension_repositories='forbidden' distrusts the pinned repository"
sql "$H2" <<SQL
SET autoinstall_known_extensions = false;
SET allow_extension_repositories = 'forbidden';
LOAD ducklake FROM agc;
SQL

step "T10. the extension directory and the pinned repository file"
find "$H2/.duckdb" -type f | sed "s|$H2|<HOME>|" | sort
grep -c 'BEGIN PUBLIC KEY\|MII' "$H2/.duckdb/extension_repositories/agc.duckdb_extension.repo.json" >/dev/null && echo "agc repo file holds the public key only"
grep -rl -e '-----BEGIN .*PRIVATE KEY-----' "$H2/.duckdb" || echo "no private key under <HOME>/.duckdb"

step "http requests served"
awk '{print $6, $7, $9}' "$R/http.log" | sort | uniq -c
