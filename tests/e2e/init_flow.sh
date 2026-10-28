#!/bin/sh
set -eu
root=$(cd "$(dirname "$0")/../.." && pwd)
binary=${1:-$root/build/host/llmgate}
work=$(mktemp -d)
export XDG_STATE_HOME="$work/state"
trap 'kill $upstream_pid 2>/dev/null || true; wait 2>/dev/null || true; rm -rf "$work"' EXIT
failures=0
check() { if "$@"; then echo "ok   $label"; else echo "FAIL $label"; failures=$((failures + 1)); fi; }

python3 "$root/tests/e2e/fake_models.py" "$work/up.port" "$work/up.log" e2e-token &
upstream_pid=$!
while [ ! -s "$work/up.port" ]; do sleep 0.1; done
address=http://127.0.0.1:$(cat "$work/up.port")

fresh_home() { export HOME="$work/$1"; mkdir -p "$HOME"; config="$HOME/.config/llmgate/config.jsonc"; }

fresh_home one
label="init on a terminal adds a key-protected gateway and a local server, offers models, and writes the profile"
printf '4,5\npartner\n%s\n1\ne2e-token\ny\n\n%s\ny\n0\n' "$address" "$address" | "$binary" init > "$work/one.out" 2>&1 || true
check sh -c "grep -q '\"partner\"' '$config' && grep -q 'secrets/partner.key' '$config' && grep -q '\"local\"' '$config'"

label="the key is stored private and never printed or put in the configuration"
check sh -c "[ \"\$(cat '$HOME/.config/llmgate/secrets/partner.key')\" = e2e-token ] && [ \"\$(ls -l '$HOME/.config/llmgate/secrets/partner.key' | cut -c1-10)\" = '-rw-------' ] && ! grep -q e2e-token '$config' '$work/one.out'"

label="the recommended models of the provider that answered are offered, and a provider that cannot list them is reported without failing init"
check sh -c "grep -q 'up-on' '$config' && grep -q 'openai_chat' '$config' && ! grep -q 'up-embed' '$config' && grep -q 'models could not be listed now; later run: llmgate models offer local all' '$work/one.out'"

label="the profile is written and onboarding is marked done because the credential mode is gateway"
check sh -c "[ -e '$HOME/.claude-llmgate/settings.json' ] && grep -q hasCompletedOnboarding '$HOME/.claude-llmgate/.claude.json' && grep -q 'Ready. Start Claude Code with: llmgate claude' '$work/one.out'"

label="running init again skips a provider name that exists and leaves the configuration unchanged"
before=$(cksum < "$config")
printf '4\npartner\n0\n' | "$binary" init > "$work/two.out" 2>&1 || true
check sh -c "grep -q 'A provider named partner already exists; skipping it.' '$work/two.out' && [ \"\$(cksum < '$config')\" = '$before' ]"

label="input that ends early stops init with a message and a failure status"
if "$binary" init < /dev/null > "$work/eof.out" 2>&1; then rc=0; else rc=$?; fi
check sh -c "[ $rc -eq 1 ] && grep -q 'the input ended before init finished' '$work/eof.out'"

label="a choice that is not a number is asked again, with the reason"
printf 'two\n9\n4\n\n%s\n3\n\n0\n' "$address" | "$binary" init > "$work/bad.out" 2>&1 || true
check sh -c "grep -q \"'two' is not a number from 1 to 5\" '$work/bad.out'"

fresh_home two
label="init --yes creates the configuration and the profile without a question and says no provider exists yet"
"$binary" init --yes > "$work/yes.out" 2>&1 < /dev/null || true
check sh -c "[ -e '$config' ] && [ -e '$HOME/.claude-llmgate/settings.json' ] && grep -q 'No providers yet' '$work/yes.out' && grep -q 'Ready.' '$work/yes.out'"

label="init --yes with a recipe that does not exist is refused by name"
if "$binary" init --yes --recipe nonesuch < /dev/null > "$work/recipe.out" 2>&1; then rc=0; else rc=$?; fi
check sh -c "[ $rc -ne 0 ] && grep -q 'no recipe named nonesuch' '$work/recipe.out'"

fresh_home three
label="a Claude login sets the credential mode to login, leaves onboarding alone and says to type /login"
printf '1\n\n0\n' | "$binary" init > "$work/claude.out" 2>&1 || true
check sh -c "grep -q 'forward_credentials' '$config' && grep -q '\"credential\": \"login\"' '$config' && [ ! -e '$HOME/.claude-llmgate/.claude.json' ] && grep -q 'type /login once' '$work/claude.out'"

fresh_home four
label="a Copilot enterprise host is asked for, the sign-in that cannot finish is reported with the retry command, and init carries on"
printf '2\n\n127.0.0.1:1\nn\n0\n' | "$binary" init > "$work/copilot.out" 2>&1 || true
check sh -c "grep -q 'https://127.0.0.1:1' '$config' && grep -q 'Sign-in did not finish; run: llmgate login --provider copilot' '$work/copilot.out' && grep -q 'Ready.' '$work/copilot.out'"

[ "$failures" -eq 0 ] || { echo "$failures check(s) failed"; exit 1; }
echo "all init checks passed"
