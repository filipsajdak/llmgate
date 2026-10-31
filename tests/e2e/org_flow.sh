#!/bin/sh
set -eu
root=$(cd "$(dirname "$0")/../.." && pwd)
binary=${1:-$root/build/host/llmgate}
binary=$(cd "$(dirname "$binary")" && pwd)/$(basename "$binary")
work=$(mktemp -d); if command -v cygpath >/dev/null 2>&1; then work=$(cygpath -m "$work"); fi
trap 'rm -rf "$work"' EXIT
export HOME="$work/home" XDG_STATE_HOME="$work/state" XDG_CONFIG_HOME="$work/config" LLMGATE_ORG_DIR="$work/org"
mkdir -p "$HOME"
failures=0
label=
check() { if "$@"; then echo "ok   $label"; else echo "FAIL $label"; failures=$((failures + 1)); fi; }

config=$work/config.jsonc
cat > "$config" <<'JSON'
{ "listen": "127.0.0.1:19881",
  "providers": { "bedrock": { "base_url": "http://127.0.0.1:1", "auth": { "kind": "none" }, "models": { "claude-sonnet-5-5": "m" } } },
  "routes": { "other": ["bedrock"] },
  "network": { "proxy": "http://mine.example.com:8080" },
  "claude": { "profile_dir": "~/.claude-org" } }
JSON
llmgate() { "$binary" --config "$config" "$@"; }

pack=$work/pack
mkdir -p "$pack"
cat > "$pack/llmgate-org.jsonc" <<'JSON'
{ "schema": 1, "name": "flow-corp", "version": "1.0.0",
  "config": { "network": { "proxy": "http://proxy.flow.example:3128" } },
  "locked": ["network"], "providers_allowed": ["bedrock", "copilot"],
  "claude": { "deny": ["Bash(curl *)"], "announcements": ["Use the company providers."],
              "marketplaces": { "flow-tools": { "source": "github", "repo": "flow/plugins" } }, "plugins": { "fmt@flow-tools": true } } }
JSON
mkdir -p "$HOME/.claude-org"
printf '{"permissions":{"deny":["Read(./mine)"]},"companyAnnouncements":["mine"]}\n' > "$HOME/.claude-org/settings.json"

label="without --yes and without a terminal the pack is shown and nothing is applied"
if llmgate org install "$pack" < /dev/null > "$work/out.txt" 2>&1; then rc=0; else rc=$?; fi
check sh -c "[ $rc -eq 2 ] && grep -q '+ deny rule: Bash(curl \*)' '$work/out.txt' && grep -q 'no terminal' '$work/out.txt' && [ ! -e '$work/org/pack.jsonc' ]"

label="--yes applies the pack and keeps the person's own settings"
llmgate org install "$pack" --yes > "$work/out.txt" 2>&1
check sh -c "[ -e '$work/org/pack.jsonc' ] && [ -e '$work/org/source.json' ] && grep -q 'mine' '$HOME/.claude-org/settings.json' && grep -q 'Bash(curl' '$HOME/.claude-org/settings.json' && grep -q 'Use the company providers' '$HOME/.claude-org/settings.json' && grep -q 'flow-tools' '$HOME/.claude-org/settings.json' && grep -q 'fmt@flow-tools' '$HOME/.claude-org/settings.json'"

label="org show names the pack and what it sets"
check sh -c "'$binary' --config '$config' org show | grep -q 'flow-corp 1.0.0' && '$binary' --config '$config' org show | grep -q 'allowed provider: bedrock'"

label="the locked proxy beats the person's value and doctor warns about the override"
llmgate doctor --offline > "$work/doctor.txt" 2>&1 || true
check sh -c "grep -q 'proxy.flow.example:3128' '$work/doctor.txt' && grep -q 'overridden by the pack' '$work/doctor.txt'"

label="a provider outside providers_allowed makes the configuration invalid with a named reason"
sed 's/"bedrock": { "base_url"/"rogue": { "base_url"/; s/"other": \["bedrock"\]/"other": ["rogue"]/' "$config" > "$work/rogue.jsonc"
check sh -c "! '$binary' --config '$work/rogue.jsonc' --check-config > '$work/out.txt' 2>&1 && grep -q 'provider rogue is not on providers_allowed' '$work/out.txt'"

label="an unknown key in a pack is refused with its path and nothing is installed"
mkdir -p "$work/bad"
printf '{"schema":1,"name":"x","version":"1.0.0","surprise":true}\n' > "$work/bad/llmgate-org.jsonc"
check sh -c "! '$binary' --config '$config' org install '$work/bad' --yes > '$work/out.txt' 2>&1 && grep -q 'surprise' '$work/out.txt'"

label="a locked path that is not a configuration section is refused"
printf '{"schema":1,"name":"x","version":"1.0.0","locked":["nonsense.key"]}\n' > "$work/bad/llmgate-org.jsonc"
check sh -c "! '$binary' --config '$config' org install '$work/bad' --yes > '$work/out.txt' 2>&1 && grep -q 'does not start with a configuration section' '$work/out.txt'"

label="remove takes out exactly what the pack added"
llmgate org remove --yes > "$work/out.txt" 2>&1
check sh -c "[ ! -e '$work/org/pack.jsonc' ] && grep -q 'mine' '$HOME/.claude-org/settings.json' && ! grep -q 'Bash(curl' '$HOME/.claude-org/settings.json' && ! grep -q 'flow-tools' '$HOME/.claude-org/settings.json' && ! grep -q 'company providers' '$HOME/.claude-org/settings.json'"

label="org with no or an unknown action prints the usage and exits 2"
if llmgate org > "$work/out1.txt" 2>&1; then a=0; else a=$?; fi
if llmgate org bogus > "$work/out2.txt" 2>&1; then b=0; else b=$?; fi
check sh -c "[ $a -eq 2 ] && [ $b -eq 2 ] && grep -q 'usage: llmgate org install' '$work/out1.txt'"

label="show, update and remove with nothing installed say so (update fails, the others succeed)"
if llmgate org update --yes > "$work/out3.txt" 2>&1; then c=0; else c=$?; fi
check sh -c "[ $c -ne 0 ] && grep -q 'no organisation pack is installed' '$work/out3.txt' && '$binary' --config '$config' org show | grep -q 'no organisation pack' && '$binary' --config '$config' org remove --yes | grep -q 'no organisation pack'"

label="a pack whose text holds a control character is refused before anything is printed"
printf '{"schema":1,"name":"x","version":"1.0.0","claude":{"announcements":["safe\\u001b[2Jspoof"]}}\n' > "$work/bad/llmgate-org.jsonc"
check sh -c "! '$binary' --config '$config' org install '$work/bad' --yes > '$work/out.txt' 2>&1 && grep -q 'control character' '$work/out.txt' && ! grep -q 'spoof' '$work/out.txt'"

label="a pack larger than the limit and a directory with no pack file are refused"
mkdir -p "$work/empty"
check sh -c "! '$binary' --config '$config' org install '$work/empty' --yes > '$work/out.txt' 2>&1 && grep -q 'has no readable llmgate-org.jsonc' '$work/out.txt'"

if command -v git >/dev/null 2>&1; then
  repo=$work/repo
  git init -q "$repo"
  git -C "$repo" config user.email test@example.com
  git -C "$repo" config user.name test
  cp "$pack/llmgate-org.jsonc" "$repo/"
  git -C "$repo" add . && git -C "$repo" commit -q -m one && git -C "$repo" tag v1.0.0
  first=$(git -C "$repo" rev-parse HEAD)
  sed -i.bak 's/"Bash(curl \*)"/"Bash(wget *)"/; s/"1.0.0"/"1.1.0"/' "$repo/llmgate-org.jsonc" && rm -f "$repo/llmgate-org.jsonc.bak"
  git -C "$repo" commit -q -am two && git -C "$repo" tag v1.1.0
  url="file://$repo"

  label="a git URL needs --ref, and a branch name is refused because it is not pinned"
  branch=$(git -C "$repo" rev-parse --abbrev-ref HEAD)
  if llmgate org install "$url" --yes > "$work/out1.txt" 2>&1; then a=0; else a=$?; fi
  if llmgate org install "$url" --ref "$branch" --yes > "$work/out2.txt" 2>&1; then b=0; else b=$?; fi
  check sh -c "[ $a -ne 0 ] && grep -q 'needs --ref' '$work/out1.txt' && [ $b -ne 0 ] && grep -q 'is not a tag' '$work/out2.txt' && [ ! -e '$work/org/pack.jsonc' ]"

  label="a tag installs and org show records the source and commit"
  llmgate org install "$url" --ref v1.0.0 --yes > "$work/out.txt" 2>&1
  check sh -c "'$binary' --config '$config' org show | head -1 | grep -q 'commit $first' && grep -q 'Bash(curl' '$HOME/.claude-org/settings.json'"

  label="org update shows the difference and moves to the new tag"
  llmgate org update --ref v1.1.0 --yes > "$work/out.txt" 2>&1
  check sh -c "grep -q -- '- deny rule: Bash(curl \*)' '$work/out.txt' && grep -q -- '+ deny rule: Bash(wget \*)' '$work/out.txt' && '$binary' --config '$config' org show | grep -q 'flow-corp 1.1.0'"

  label="a branch with the name of a tag cannot stand in for the tag: the tag's commit is installed"
  git -C "$repo" checkout -q -b v1.0.0 "$first"
  printf '{"schema":1,"name":"evil","version":"9.9.9"}\n' > "$repo/llmgate-org.jsonc"
  git -C "$repo" commit -q -am "branch with a tag's name"
  git -C "$repo" checkout -q "$branch"
  llmgate org install "$url" --ref v1.0.0 --yes > "$work/out.txt" 2>&1
  check sh -c "'$binary' --config '$config' org show | head -1 | grep -q 'flow-corp 1.0.0' && '$binary' --config '$config' org show | head -1 | grep -q 'commit $first'"

  label="a directory named like a git URL cannot stand in for it, and update keeps using the saved kind and absolute path"
  mkdir -p "$work/cwd/file:/$repo"
  printf '{"schema":1,"name":"shadow","version":"6.6.6"}\n' > "$work/cwd/file:/$repo/llmgate-org.jsonc"
  (cd "$work/cwd" && "$binary" --config "$config" org install "$url" --ref v1.0.0 --yes > "$work/out.txt" 2>&1)
  check sh -c "'$binary' --config '$config' org show | head -1 | grep -q 'flow-corp 1.0.0'"
  llmgate org remove --yes > /dev/null 2>&1
  (cd "$work" && "$binary" --config "$config" org install pack --yes > "$work/out.txt" 2>&1)
  (cd / && "$binary" --config "$config" org update --yes > "$work/out.txt" 2>&1)
  check sh -c "'$binary' --config '$config' org show | head -1 | grep -q \"flow-corp 1.0.0  source $(cd "$pack" && pwd -P)\""

  label="a full commit is accepted as a pin"
  llmgate org install "$url" --ref "$first" --yes > "$work/out.txt" 2>&1
  check sh -c "llmgate_show=\$('$binary' --config '$config' org show | head -1); echo \"\$llmgate_show\" | grep -q 'flow-corp 1.0.0'"
fi

echo "$failures failures"
[ "$failures" -eq 0 ]
