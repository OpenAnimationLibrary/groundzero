#!/usr/bin/env bash
set -euo pipefail
trap 'echo "::error title=Check published build::Could not check release tags for $GITHUB_SHA" >&2' ERR

# Match the actual tag commit, including peeled annotated tags.
tags=$(git ls-remote --tags "$GITHUB_SERVER_URL/$GITHUB_REPOSITORY.git" 'refs/tags/build-*' |
  awk -v sha="$GITHUB_SHA" '$1 == sha && $2 ~ /^refs\/tags\/build-[0-9]+(\^\{\})?$/ {
    sub(/^refs\/tags\//, "", $2)
    sub(/\^\{\}$/, "", $2)
    print $2
  }')

if [[ -n "$tags" ]]; then
  gh api "repos/$GITHUB_REPOSITORY/releases?per_page=100" --paginate --slurp |
    jq -r --arg tags "$tags" '
      ($tags | split("\n")) as $matching_tags
      | first(.[][] | select(.draft == false and .prerelease == true and .published_at != null)
          | .tag_name | select(IN($matching_tags[]))) // empty'
fi
