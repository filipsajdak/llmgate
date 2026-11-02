#!/bin/sh
set -eu
here=$(cd "$(dirname "$0")" && pwd)
installer=$here/../../install/install.sh
work=$(mktemp -d); if command -v cygpath >/dev/null 2>&1; then work=$(cygpath -m "$work"); fi
export HOME="$work/home" XDG_STATE_HOME="$work/state" XDG_CONFIG_HOME="$work/config" LLMGATE_INSTALL_TTY=/dev/null
mkdir -p "$HOME" "$work/stubs"
failures=0
started=$(date +%s)
check() { elapsed=$(($(date +%s) - started)); if "$@"; then echo "ok   [${elapsed}s] $label"; else echo "FAIL [${elapsed}s] $label"; failures=$((failures + 1)); fi; }
server=
trap '[ -z "$server" ] || kill "$server" 2>/dev/null || true; rm -rf "$work"' EXIT

os=$(uname -s | tr 'A-Z' 'a-z')
case "$(uname -m)" in arm64|aarch64) arch=arm64 ;; *) arch=x86_64 ;; esac
prefix=$work/bin
site=$work/site
printf '#!/bin/sh\nexit 0\n' > "$work/stubs/claude"
chmod +x "$work/stubs/claude"
PATH="$work/stubs:$PATH"
export PATH

release() {
  v=$1
  usage_line="usage: llmgate [setup]"
  if [ "${2:-}" = init ]; then usage_line="usage: llmgate [setup | init [--yes]]"; fi
  dir=$site/v$v
  stage=$work/stage-$v/llmgate-$v-$os-$arch
  mkdir -p "$dir" "$stage"
  cat > "$stage/llmgate" <<BIN
#!/bin/sh
case "\$1" in
  --version) echo "llmgate $v" ;;
  stop) echo stopped >> "$work/stop.marker" ;;
  setup) echo "setup $v" >> "$work/setup.marker" ;;
  init) echo "init $v" >> "$work/init.marker" ;;
  org) shift; echo "\$*" >> "$work/org.marker" ;;
  *) echo "$usage_line" >&2; exit 2 ;;
esac
BIN
  chmod +x "$stage/llmgate"
  tar -C "$work/stage-$v" -czf "$dir/llmgate-$v-$os-$arch.tar.gz" "llmgate-$v-$os-$arch"
  if command -v sha256sum >/dev/null 2>&1; then
    (cd "$dir" && sha256sum "llmgate-$v-$os-$arch.tar.gz" > SHA256SUMS.txt)
  else
    (cd "$dir" && shasum -a 256 "llmgate-$v-$os-$arch.tar.gz" > SHA256SUMS.txt)
  fi
}
release 1.0.0
release 1.1.0 init

port=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])')
python3 -I -m http.server "$port" --bind 127.0.0.1 --directory "$site" >/dev/null 2>&1 &
server=$!
base=http://127.0.0.1:$port
up=0
for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
  if curl -fsS -o /dev/null "$base/v1.0.0/SHA256SUMS.txt" 2>/dev/null; then up=1; break; fi
  sleep 0.25
done
[ "$up" -eq 1 ] || { echo "FAIL the test server did not start"; exit 1; }

run() { sh "$installer" --prefix "$prefix" --base-url "$base/v$version" --version "$version" "$@"; }

label="a dry run prints the steps and writes nothing, not even a log"
version=1.0.0
out=$(run --dry-run 2>&1)
check sh -c "echo '$out' | grep -q 'would: download' && [ ! -e '$prefix/llmgate' ] && [ ! -e '$work/state' ]"

label="an install puts the verified binary in place, logs it, and runs setup when init is missing and a terminal exists"
version=1.0.0
out=$(run 2>&1)
check sh -c "[ \"\$('$prefix/llmgate' --version)\" = 'llmgate 1.0.0' ] && echo '$out' | grep -q 'checksum ok' && ls '$work'/state/llmgate/install-*.log >/dev/null && grep -q 'setup 1.0.0' '$work/setup.marker'"

label="the log is private to the user"
check sh -c "[ \"\$(ls -l '$work'/state/llmgate/install-*.log | cut -c1-10)\" = '-rw-------' ]"

label="a second run with the same version changes nothing and keeps no previous binary"
version=1.0.0
out=$(run 2>&1)
check sh -c "echo '$out' | grep -q 'already installed' && [ ! -e '$prefix/llmgate.previous' ]"

label="an upgrade keeps the old binary as llmgate.previous and runs init when the new binary has it"
version=1.1.0
run >/dev/null 2>&1
check sh -c "[ \"\$('$prefix/llmgate' --version)\" = 'llmgate 1.1.0' ] && [ \"\$('$prefix/llmgate.previous' --version)\" = 'llmgate 1.0.0' ] && grep -q 'init 1.1.0' '$work/init.marker'"

label="a checksum mismatch installs nothing and says so"
release 1.2.0
echo "$(printf '0%.0s' $(seq 1 64))  llmgate-1.2.0-$os-$arch.tar.gz" > "$site/v1.2.0/SHA256SUMS.txt"
version=1.2.0
if run >"$work/out.txt" 2>&1; then rc=0; else rc=$?; fi
check sh -c "[ $rc -ne 0 ] && grep -q 'checksum mismatch' '$work/out.txt' && [ \"\$('$prefix/llmgate' --version)\" = 'llmgate 1.1.0' ]"

label="a binary that reports another version is refused"
release 1.3.0
sed -i.bak 's/llmgate 1.3.0/llmgate 9.9.9/' "$work/stage-1.3.0/llmgate-1.3.0-$os-$arch/llmgate"
tar -C "$work/stage-1.3.0" -czf "$site/v1.3.0/llmgate-1.3.0-$os-$arch.tar.gz" "llmgate-1.3.0-$os-$arch"
if command -v sha256sum >/dev/null 2>&1; then (cd "$site/v1.3.0" && sha256sum "llmgate-1.3.0-$os-$arch.tar.gz" > SHA256SUMS.txt); else (cd "$site/v1.3.0" && shasum -a 256 "llmgate-1.3.0-$os-$arch.tar.gz" > SHA256SUMS.txt); fi
version=1.3.0
if run >"$work/out.txt" 2>&1; then rc=0; else rc=$?; fi
check sh -c "[ $rc -ne 0 ] && grep -q 'reports' '$work/out.txt'"

label="a version that is not MAJOR.MINOR.PATCH, a bad repository and an unknown system are refused"
bad=0
sh "$installer" --prefix "$prefix" --version '1.0/../x' >/dev/null 2>&1 && bad=1
sh "$installer" --prefix "$prefix" --version 1.0.0 --repo 'a/../b' >/dev/null 2>&1 && bad=1
LLMGATE_UNAME_S=Plan9 sh "$installer" --prefix "$prefix" --version 1.0.0 --base-url "$base/v1.0.0" >"$work/out.txt" 2>&1 && bad=1
check sh -c "[ $bad -eq 0 ] && grep -q 'unsupported system' '$work/out.txt'"

label="a prefix that is not on PATH gets the line to add and no profile is edited"
version=1.1.0
out=$(run --force --no-init 2>&1)
check sh -c "echo '$out' | grep -q 'export PATH=' && [ ! -e '$HOME/.zshrc' ] && [ ! -e '$HOME/.bashrc' ] && [ ! -e '$HOME/.profile' ]"

label="--org passes the source, the ref and the yes on to llmgate org install, before init"
rm -f "$work/org.marker"
version=1.1.0
run --force --no-init --yes --org https://git.example/pack.git --org-ref v1.2.0 >/dev/null 2>&1 || true
check sh -c "grep -qx 'install https://git.example/pack.git --ref v1.2.0 --yes' '$work/org.marker'"

label="--org with --dry-run only says what it would do"
rm -f "$work/org.marker"
out=$(run --force --no-init --dry-run --org ./pack 2>&1 || true)
check sh -c "echo '$out' | grep -q 'would: run llmgate org install ./pack' && [ ! -e '$work/org.marker' ]"

label="uninstall stops the gateway and removes the binaries but keeps configuration"
mkdir -p "$XDG_CONFIG_HOME/llmgate" "$HOME/.claude" "$HOME/.claude-llmgate"
echo keep > "$XDG_CONFIG_HOME/llmgate/config.jsonc"
echo mine > "$HOME/.claude/settings.json"
sh "$installer" --prefix "$prefix" --uninstall >/dev/null 2>&1
check sh -c "[ ! -e '$prefix/llmgate' ] && [ ! -e '$prefix/llmgate.previous' ] && [ -e '$work/stop.marker' ] && [ -e '$XDG_CONFIG_HOME/llmgate/config.jsonc' ]"

label="purge without a yes and without a terminal removes nothing"
sh "$installer" --prefix "$prefix" --purge >/dev/null 2>&1
check sh -c "[ -e '$XDG_CONFIG_HOME/llmgate/config.jsonc' ] && [ -e '$HOME/.claude-llmgate' ]"

label="purge with --yes removes llmgate's directories and never touches ~/.claude"
sh "$installer" --prefix "$prefix" --purge --yes >/dev/null 2>&1
check sh -c "[ ! -e '$XDG_CONFIG_HOME/llmgate' ] && [ ! -e '$XDG_STATE_HOME/llmgate' ] && [ ! -e '$HOME/.claude-llmgate' ] && [ \"\$(cat '$HOME/.claude/settings.json')\" = mine ]"

[ "$failures" -eq 0 ] || { echo "$failures check(s) failed"; exit 1; }
echo "all install checks passed"
