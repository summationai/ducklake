#!/usr/bin/env bash
# Checks, right before a publish, that the approved build is still the newest commit of its branch (spec:
# summationai/agent-cloud docs/specs/ducklake-extension-release.md, DXR-12 step 4). An older run approved after a newer
# commit landed must not move the channel backwards.
#
#   scripts/branching/release/still_head.sh <owner/repo> <branch> <commit sha>
#
# Exits 0 when the commit is the branch's head, 3 when the branch has moved on (it logs "superseded by <sha>"), and 1
# when the head can't be read. Needs GH_TOKEN.
set -uo pipefail
usage="usage: still_head.sh <owner/repo> <branch> <commit sha>"
REPO=${1:?$usage}
BRANCH=${2:?$usage}
SHA=${3:?$usage}
if ! head=$(gh api "repos/$REPO/git/ref/heads/$BRANCH" --jq .object.sha 2>&1) || [[ ! $head =~ ^[0-9a-f]{40}$ ]]; then
	echo "::error::still_head: can't read the head of $BRANCH: $head" >&2
	exit 1
fi
if [ "$head" != "$SHA" ]; then
	echo "superseded by $head: $SHA is no longer the head of $BRANCH; not publishing it"
	exit 3
fi
echo "$SHA is still the head of $BRANCH"
