#!/usr/bin/env bash
# Decides whether a release run may publish to release/: release/ only moves forward along develop's history (DXR-15
# step 4). Run right before the copy, after the approval, from a checkout with develop's full history.
#   - release/ is empty: publish;
#   - release/ holds this commit, or one of its ancestors: publish;
#   - release/ holds a commit that is no longer in develop's history (a DXR-4 rebase): publish only if this run's commit
#     is develop's head;
#   - otherwise release/ holds something newer: skip, as superseded.
# The commit comes from the object's metadata (upload.sh records it; promote.sh copies it). An object without one, or
# an error reading it, fails closed.
#
#   scripts/branching/release/newer.sh <bucket> <release key> <commit> <develop head>
#
# Logs go to stderr. Prints, for $GITHUB_OUTPUT: publish=true or publish=false.
set -euo pipefail
usage="usage: newer.sh <bucket> <release key> <commit> <develop head>"
BUCKET=${1:?$usage}
KEY=${2:?$usage}
COMMIT=${3:?$usage}
HEAD=${4:?$usage}

log() { echo "$*" >&2; }
fail() { echo "::error::newer: $*" >&2; exit 1; }
decide() { log "$2"; echo "publish=$1"; exit 0; }

# The release writer may read release/ but can't list the bucket, so S3 answers a missing object with 403, not 404
if ! current=$(aws s3api head-object --bucket "$BUCKET" --key "$KEY" --output json 2>&1); then
	grep -q -E '\((403|404)\)|Forbidden|Not Found|NoSuchKey' <<< "$current" \
		|| fail "can't read s3://$BUCKET/$KEY: $current"
	decide true "release/ is empty (s3://$BUCKET/$KEY not found); publishing $COMMIT"
fi
held=$(jq -r '.Metadata.commit // empty' <<< "$current")
[[ $held =~ ^[0-9a-f]{40}$ ]] \
	|| fail "s3://$BUCKET/$KEY records no commit ('$held'); publish this one by hand, with commit metadata"
log "release/ holds $held"

[ "$held" = "$COMMIT" ] && decide true "release/ already holds $COMMIT; promote.sh checks its bytes"
if git cat-file -e "$held^{commit}" 2> /dev/null; then
	git merge-base --is-ancestor "$held" "$COMMIT" && decide true "$held is an ancestor of $COMMIT; publishing"
	echo "::warning::newer: superseded: release/ holds $held, which $COMMIT doesn't descend from; not publishing" >&2
	echo "publish=false"
	exit 0
fi
# $held was rewritten away by a rebase: only develop's head may replace it
[ "$COMMIT" = "$HEAD" ] \
	&& decide true "release/ holds $held, no longer in develop's history; $COMMIT is develop's head, publishing"
echo "::warning::newer: superseded: release/ holds $held (not in develop's history) and $COMMIT isn't develop's head $HEAD; not publishing" >&2
echo "publish=false"
