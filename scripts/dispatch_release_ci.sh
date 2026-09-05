#!/usr/bin/env bash
# Give the release-please pull request a CI check.
#
# GITHUB_TOKEN may not start workflows, so the pull_request run for that branch is created in
# action_required and waits for someone to press Approve, every release. A push to the branch does
# not create a run at all. workflow_dispatch is the exception GitHub allows, so dispatch CI on the
# release branch: the checks land on the pull request's head commit, which is both what a reviewer
# needs to see before merging and what required_status_checks reads (MSIME-Windows#121).
#
# Only confirms the run started. Nobody is waiting on the result here, because a human merges the
# release pull request in this repository; the point is that the check exists for them to look at.
#
# Requires GH_TOKEN, GH_REPO, RELEASE_PR (the JSON release-please emits).
set -euo pipefail

number=$(jq -er .number <<< "$RELEASE_PR")

pr_json=$(gh pr view "$number" --repo "$GH_REPO" --json state,headRefName,headRefOid)
state=$(jq -er .state <<< "$pr_json")
head_branch=$(jq -er .headRefName <<< "$pr_json")
head_sha=$(jq -er .headRefOid <<< "$pr_json")

if [[ "$state" != OPEN ]]; then
    echo "Release pull request #$number is $state; nothing to dispatch." >&2
    exit 0
fi
if [[ "$head_branch" != release-please--* ]]; then
    echo "Release pull request #$number has an unexpected head branch: $head_branch" >&2
    exit 1
fi

echo "Dispatching CI for release pull request #$number at $head_sha"
gh workflow run ci.yml --repo "$GH_REPO" --ref "$head_branch"

# gh cannot report which run a dispatch created, so poll for one on this branch at this commit.
for _ in $(seq 1 60); do
    run_id=$(gh run list --repo "$GH_REPO" --workflow ci.yml --branch "$head_branch" \
        --event workflow_dispatch --limit 20 --json databaseId,headSha \
        --jq "map(select(.headSha == \"$head_sha\")) | first | .databaseId // empty")
    if [[ -n "$run_id" ]]; then
        echo "CI run $run_id started for #$number"
        exit 0
    fi
    sleep 2
done

echo "Dispatched CI for #$number but no run appeared for $head_sha." >&2
exit 1
