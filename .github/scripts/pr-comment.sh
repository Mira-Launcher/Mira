#!/bin/bash
# Posts BODY_FILE as a PR comment, or updates the comment an earlier run posted with the same KEY, so each
# workflow keeps one comment of its own. Usage: pr-comment.sh PR KEY BODY_FILE (needs GH_TOKEN).
set -euo pipefail
pr=$1
marker="<!-- mira:$2 -->"
body="$(cat "$3")"$'\n\n'"$marker"
id=$(gh api "repos/{owner}/{repo}/issues/$pr/comments" --paginate \
  --jq ".[] | select(.user.type == \"Bot\" and (.body | contains(\"$marker\"))) | .id" | tail -1)
if [ -n "$id" ]; then
  gh api -X PATCH "repos/{owner}/{repo}/issues/comments/$id" -f body="$body" >/dev/null
else
  gh pr comment "$pr" --body "$body"
fi
