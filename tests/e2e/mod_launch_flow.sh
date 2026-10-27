#!/bin/sh
set -eu
root=$(cd "$(dirname "$0")/../.." && pwd)
binary=${1:?usage: mod_launch_flow.sh <llmgate binary>}
work=$(mktemp -d)
export HOME="$work/home" XDG_STATE_HOME="$work/state" XDG_CONFIG_HOME="$work/config" XDG_DATA_HOME="$work/data"
mkdir -p "$HOME"
failures=0
check() { if "$@"; then echo "ok   $label"; else echo "FAIL $label"; failures=$((failures + 1)); fi; }
port=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])')
trap '"$binary" --config "$work/on.jsonc" stop >/dev/null 2>&1 || true; rm -rf "$work"' EXIT

cat > "$work/claude" <<'STUB'
#!/bin/sh
{ echo "ARGS: $*"; env | grep -E '^LLMGATE_(BIN|CONFIG|MOD_DIR)=' | sort; } > "$STUB_OUTPUT"
STUB
chmod +x "$work/claude"
export STUB_OUTPUT="$work/claude.out" LLMGATE_CLAUDE="$work/claude"

write_config() {
  cat > "$work/$1.jsonc" <<JSON
{
  "listen": "127.0.0.1:$port",
  "upstream": { "base_url": "http://127.0.0.1:1" },
  "static_token_env": "E2E_UPSTREAM_TOKEN",
  "claude": { "profile_dir": "$work/profile"$2 }
}
JSON
}
export E2E_UPSTREAM_TOKEN=test-token
write_config on ""
write_config off ', "mod": false'

label="llmgate claude loads the mod from a digest directory and puts it before the person's arguments"
"$binary" --config "$work/on.jsonc" claude -p hello >/dev/null
dir=$(sed -n 's/^ARGS: --plugin-dir \([^ ]*\) -p hello$/\1/p' "$work/claude.out")
check sh -c "case '$dir' in '$work'/data/llmgate/mod/????????????????) true ;; *) false ;; esac"

label="every runtime file of the mod is written there, byte for byte, private to the user"
check sh -c "cmp -s '$root/mods/llmgate/.claude-plugin/plugin.json' '$dir/.claude-plugin/plugin.json' && cmp -s '$root/mods/llmgate/hooks/register.tsx' '$dir/hooks/register.tsx' && cmp -s '$root/mods/llmgate/hooks/hooks.json' '$dir/hooks/hooks.json' && [ \"\$(ls -l '$dir/hooks/register.tsx' | cut -c1-10)\" = '-rw-------' ] && [ ! -e '$dir/tests' ] && [ ! -e '$dir/.claude-plugin/types' ]"

label="the binary and the configuration are exported for the mod, as absolute paths"
check sh -c "grep -q '^LLMGATE_BIN=/' '$work/claude.out' && grep -q \"^LLMGATE_CONFIG=$work/on.jsonc\$\" '$work/claude.out'"

label="llmgate mod installs the mod and prints its directory, the one llmgate claude passes"
printed=$("$binary" --config "$work/on.jsonc" mod)
check sh -c "[ '$printed' = '$dir' ] && cmp -s '$root/mods/llmgate/hooks/setup.ts' '$printed/hooks/setup.ts'"

label="a second launch writes nothing again"
before=$(ls -l -T "$dir/hooks/register.tsx" 2>/dev/null || ls -l --time-style=full-iso "$dir/hooks/register.tsx")
sleep 1
"$binary" --config "$work/on.jsonc" claude >/dev/null
after=$(ls -l -T "$dir/hooks/register.tsx" 2>/dev/null || ls -l --time-style=full-iso "$dir/hooks/register.tsx")
check test "$before" = "$after"

label="a changed file is put right on the next launch"
echo tampered > "$dir/hooks/model.ts"
"$binary" --config "$work/on.jsonc" claude >/dev/null
check cmp -s "$root/mods/llmgate/hooks/model.ts" "$dir/hooks/model.ts"

label="claude.mod false writes nothing new, adds no flag and exports nothing"
rm -rf "$work/data"
"$binary" --config "$work/off.jsonc" claude -p hi >/dev/null
check sh -c "grep -q '^ARGS: -p hi\$' '$work/claude.out' && ! grep -q LLMGATE_ '$work/claude.out' && [ ! -e '$work/data' ]"

label="llmgate mod says so and fails when the mod is turned off"
if "$binary" --config "$work/off.jsonc" mod >"$work/mod.out" 2>"$work/mod.err"; then rc=0; else rc=$?; fi
check sh -c "[ $rc -ne 0 ] && [ ! -s '$work/mod.out' ] && grep -q 'claude.mod is false' '$work/mod.err'"

label="LLMGATE_MOD_DIR loads a directory of the person's choice and writes nothing"
LLMGATE_MOD_DIR=/somewhere/else "$binary" --config "$work/on.jsonc" claude -p hi >/dev/null
check sh -c "grep -q '^ARGS: --plugin-dir /somewhere/else -p hi\$' '$work/claude.out' && [ ! -e '$work/data' ]"

label="when the files cannot be written, Claude Code still starts, without the mod, with a warning"
mkdir -p "$work/blocked"
chmod 500 "$work/blocked"
XDG_DATA_HOME="$work/blocked" "$binary" --config "$work/on.jsonc" claude -p hi >"$work/out.txt" 2>"$work/err.txt" || true
chmod 700 "$work/blocked"
check sh -c "grep -q '^ARGS: -p hi\$' '$work/claude.out' && grep -q 'starting Claude Code without it' '$work/err.txt'"

[ "$failures" -eq 0 ] || { echo "$failures check(s) failed"; exit 1; }
echo "all mod launch checks passed"
