#!/bin/sh
set -eu

bundle=${1:?usage: claude_publish.sh <bundle> <base sha> <branch> [dry]}
base=${2:?usage: claude_publish.sh <bundle> <base sha> <branch> [dry]}
branch=${3:?usage: claude_publish.sh <bundle> <base sha> <branch> [dry]}
mode=${4:-push}

max_commits=20
max_bundle_bytes=5242880
incoming=refs/claude/incoming
allowed='^((src|app|docs|examples|mods)/|tests/(unit|e2e|constexpr|fuzz|network|support|compat)/|README\.md$|config\.example\.jsonc$)'
secret='gh[a-z]_[A-Za-z0-9]{20,}|github_pat_[A-Za-z0-9_]{20,}|sk-ant-[A-Za-z0-9_-]{20,}|x-access-token'

refuse() { echo "refusing: $*" >&2; exit 1; }

printf '%s' "$base" | grep -Eq '^[0-9a-f]{40}$' || refuse "the base is not a commit id"
printf '%s' "$branch" | grep -Eq '^claude/run-[0-9]+(-[0-9]+)?$' || refuse "the branch name is not claude/run-N"
size=$(wc -c < "$bundle")
[ "$size" -le "$max_bundle_bytes" ] || refuse "the bundle is $size bytes"

git bundle verify "$bundle" >/dev/null 2>&1 || refuse "the bundle does not verify against this repository"
git -c fetch.fsckObjects=true fetch --quiet --no-tags "$bundle" "HEAD:$incoming"

git merge-base --is-ancestor "$base" "$incoming" || refuse "the commits do not build on the base"
count=$(git rev-list --count "$base..$incoming")
[ "$count" -ge 1 ] && [ "$count" -le "$max_commits" ] || refuse "$count commits"

names=$(mktemp)
trap 'rm -f "$names"' EXIT
git -c core.quotePath=false log --name-only -z --format= --no-renames "$base..$incoming" > "$names"
if LC_ALL=C grep -zEq '[^A-Za-z0-9._/-]' "$names" || [ "$(tr -d '\0' < "$names" | wc -l)" -ne 0 ]; then
  refuse "a path uses a character outside A-Z a-z 0-9 . _ / -"
fi
if tr '\0' '\n' < "$names" | grep -Evq "$allowed"; then
  tr '\0' '\n' < "$names" | grep -Ev "$allowed" | sed 's/^/  outside the allowed directories: /' >&2
  refuse "the commits touch a path outside the directories Claude may change"
fi
if tr '\0' '\n' < "$names" | grep -Eq '(^|/)\.'; then
  tr '\0' '\n' < "$names" | grep -E '(^|/)\.' | sed 's/^/  dot-prefixed component: /' >&2
  refuse "a path has a dot-prefixed component"
fi
if [ "$(git rev-list --merges --count "$base..$incoming")" -ne 0 ]; then
  refuse "the commits include a merge commit"
fi
if git log --raw --no-renames --format= "$base..$incoming" | grep -Eq '^:[0-7]{6} 160000|^:160000'; then
  refuse "the commits add or change a submodule"
fi
if git log --raw --no-renames --format= "$base..$incoming" | awk '{print $2}' | grep -Evq '^(100644|100755|000000)$'; then
  refuse "the commits add a symlink or a file with an unusual mode"
fi
if git log -p --text --no-ext-diff --no-textconv --format='%H%n%an%n%ae%n%cn%n%ce%n%B' "$base..$incoming" | LC_ALL=C grep -aEq "$secret"; then
  refuse "a commit, its author fields or its change look like they carry a token"
fi

echo "verified $count commit(s) from ${base} for $branch"
[ "$mode" = dry ] && exit 0
git push origin "$(git rev-parse --verify "$incoming^{commit}"):refs/heads/$branch"
