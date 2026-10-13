#!/usr/bin/env bash
# Brings the branching code of the main-line stack (feature/branching-*) into this DuckLake v1.5 branch.
#
# It copies src/branching, its headers, the branch tests, docs/branching and scripts/branching from the stack as a
# "Copy branching from <sha>" commit, then reapplies this branch's v1.5 adaptations: everything this branch changed
# in those paths since its previous copy. It also lists DuckLake files outside those paths that the stack changed
# since the previous copy, whose calls into DuckLakeBranching need porting by hand.
#
# Usage: scripts/branching/sync_v1_5.sh [stack-ref]    (default: origin/feature/branching-close)
# Then: build, run test/sql/branch on every config, and commit the reapplied adaptations.
set -euo pipefail

# the whole script is read before it runs: the copy replaces this file
main() {
	cd "$(dirname "$0")/../.."
	local source=${1:-origin/feature/branching-close}
	local paths=(src/branching src/include/branching test/sql/branch docs/branching scripts/branching)

	if ! git diff --quiet || ! git diff --cached --quiet; then
		echo "Commit or set aside your changes first"
		exit 1
	fi
	local previous_copy
	previous_copy=$(git log -1 --format=%H --grep='^Copy branching from ' HEAD)
	if [ -z "$previous_copy" ]; then
		echo "No 'Copy branching from <sha>' commit on this branch"
		exit 1
	fi
	local previous_source
	previous_source=$(git log -1 --format=%s "$previous_copy" | awk '{print $4}')
	local source_sha
	source_sha=$(git rev-parse --short "$source")

	# this branch's adaptations since the previous copy
	local adaptations
	adaptations=$(mktemp)
	git diff --binary "$previous_copy" HEAD -- "${paths[@]}" >"$adaptations"

	git rm -rq --ignore-unmatch -- "${paths[@]}"
	git checkout "$source" -- "${paths[@]}"
	git commit -q -m "Copy branching from $source_sha ($source)" \
		-m "The branching code of the stack at $source_sha, unchanged; the adaptations to DuckLake v1.5 follow."
	echo "Copied the branching code from $source_sha"

	if git apply --3way "$adaptations"; then
		echo "Reapplied the v1.5 adaptations; review, build and test, then commit them"
	else
		echo "The v1.5 adaptations did not all apply: resolve the conflicts, build and test, then commit"
	fi
	rm -f "$adaptations"

	echo "DuckLake files outside src/branching the stack changed since $previous_source (port their calls by hand):"
	git diff --stat "$previous_source" "$source" -- . ':!src/branching' ':!src/include/branching' ':!test' \
		':!docs' ':!scripts' || true
}

main "$@"
exit
