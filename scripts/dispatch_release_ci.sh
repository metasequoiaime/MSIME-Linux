#!/usr/bin/env bash
# Give the release-please branch a CI check.
#
# GITHUB_TOKEN may not start workflows, so the pull_request run for that branch is created in
# action_required and waits for someone to press Approve, every release. A push to the branch does
# not create a run at all. workflow_dispatch is the exception GitHub allows, so dispatch CI on the
# release branch: the checks land on its head commit, which is both what a reviewer needs before
# merging and what required_status_checks reads (MSIME-Windows#121).
#
# The branch is found by ref rather than from a pull request payload. release-please only reports a
# pull request as created once, but it rewrites that branch on every later push to main, so keying
# off creation would leave every subsequent head commit without a check.
#
# Only confirms the run started. Nobody waits on the result here, because a human merges the release
# pull request in this repository; the point is that the check exists for them to look at.
#
# Requires GH_TOKEN and GH_REPO.
set -euo pipefail

branch=$(gh api "repos/$GH_REPO/git/matching-refs/heads/release-please--" \
    --jq '.[0].ref // empty' | sed 's|^refs/heads/||')
if [[ -z "$branch" ]]; then
    echo "No release branch exists yet; nothing to dispatch."
    exit 0
fi

head_sha=$(gh api "repos/$GH_REPO/git/ref/heads/$branch" --jq .object.sha)
echo "Dispatching CI for $branch at $head_sha"
gh workflow run ci.yml --repo "$GH_REPO" --ref "$branch"

# gh cannot report which run a dispatch created, so poll for one on this branch at this commit.
# Without this check a broken dispatch looks exactly like a working one, which is how the previous
# attempt at this shipped dead configuration.
for _ in $(seq 1 60); do
    run_id=$(gh run list --repo "$GH_REPO" --workflow ci.yml --branch "$branch" \
        --event workflow_dispatch --limit 20 --json databaseId,headSha \
        --jq "map(select(.headSha == \"$head_sha\")) | first | .databaseId // empty")
    if [[ -n "$run_id" ]]; then
        echo "CI run $run_id started for $branch at $head_sha"
        exit 0
    fi
    sleep 2
done

echo "Dispatched CI for $branch but no run appeared for $head_sha." >&2
exit 1
