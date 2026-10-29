#!/bin/sh
set -eu

repo=filipsajdak/llmgate
prefix=$HOME/.local/bin
version=latest
base_url=
dry_run=0
yes=0
force=0
uninstall=0
purge=0
run_init=1
with_claude=0
org=
org_ref=
log=
tty=${LLMGATE_INSTALL_TTY:-/dev/tty}

usage() {
  cat <<'EOF'
Install llmgate from a release.

  sh install.sh [options]
  curl -fsSL https://raw.githubusercontent.com/filipsajdak/llmgate/main/install/install.sh | sh -s -- [options]

Options:
  --version X.Y.Z   install this version (default: the latest release)
  --prefix DIR      install into DIR (default: ~/.local/bin)
  --repo OWNER/NAME take releases from another repository (a fork)
  --org SOURCE      apply an organisation pack (a directory or git URL) before init
  --org-ref REF     the tag or full commit of a git URL pack
  --no-init         do not run llmgate init (or llmgate setup) afterwards
  --with-claude     install Claude Code with its official installer if it is missing
  --force           reinstall even when the version is already installed
  --dry-run         print every step and change nothing
  --yes             answer yes to questions about removing files
  --uninstall       stop llmgate and remove the binary
  --purge           with --uninstall: also remove llmgate's configuration, state and profiles
  --base-url URL    download from URL instead of GitHub (for tests); needs --version
  --help            show this text
EOF
}

die() {
  printf 'install: %s\n' "$*" >&2
  exit 1
}

say() {
  printf '%s\n' "$*"
  if [ -n "$log" ]; then
    printf '%s\n' "$*" >> "$log"
  fi
}

step() {
  if [ "$dry_run" -eq 1 ]; then
    say "would: $*"
    return 1
  fi
  return 0
}

have_tty() {
  ( : < "$tty" ) 2>/dev/null
}

ask() {
  if [ "$yes" -eq 1 ]; then
    return 0
  fi
  if ! have_tty; then
    return 1
  fi
  printf '%s [y/N] ' "$1" > "$tty"
  answer=
  read -r answer < "$tty" || answer=
  case "$answer" in
    y|Y|yes|YES) return 0 ;;
    *) return 1 ;;
  esac
}

while [ $# -gt 0 ]; do
  case "$1" in
    --version) [ $# -ge 2 ] || die "--version needs a value"; version=$2; shift ;;
    --prefix) [ $# -ge 2 ] || die "--prefix needs a value"; prefix=$2; shift ;;
    --repo) [ $# -ge 2 ] || die "--repo needs a value"; repo=$2; shift ;;
    --base-url) [ $# -ge 2 ] || die "--base-url needs a value"; base_url=$2; shift ;;
    --org) [ $# -ge 2 ] || die "--org needs a value"; org=$2; shift ;;
    --org-ref) [ $# -ge 2 ] || die "--org-ref needs a value"; org_ref=$2; shift ;;
    --no-init) run_init=0 ;;
    --with-claude) with_claude=1 ;;
    --force) force=1 ;;
    --dry-run) dry_run=1 ;;
    --yes) yes=1 ;;
    --uninstall) uninstall=1 ;;
    --purge) uninstall=1; purge=1 ;;
    --help|-h) usage; exit 0 ;;
    *) usage >&2; die "unknown option $1" ;;
  esac
  shift
done

state_dir=${XDG_STATE_HOME:-$HOME/.local/state}/llmgate
config_dir=${XDG_CONFIG_HOME:-$HOME/.config}/llmgate

case "$repo" in
  *[!A-Za-z0-9._/-]*|*..*|/*|*/|*//*) die "--repo must look like OWNER/NAME" ;;
  */*) ;;
  *) die "--repo must look like OWNER/NAME" ;;
esac

if [ "$uninstall" -eq 1 ]; then
  bin=$prefix/llmgate
  if [ -x "$bin" ] && step "$bin stop"; then
    "$bin" stop >/dev/null 2>&1 || true
  fi
  for file in "$bin" "$bin.previous"; do
    if [ -e "$file" ] && step "remove $file"; then
      rm -f "$file"
      say "removed $file"
    fi
  done
  if [ "$purge" -eq 1 ]; then
    found=
    for dir in "$config_dir" "$state_dir" "$HOME"/.claude-llmgate*; do
      if [ -e "$dir" ]; then
        found="$found $dir"
      fi
    done
    if [ -z "$found" ]; then
      say "nothing to purge"
    else
      say "purge would remove:"
      for dir in $found; do
        say "  $dir"
      done
      if step "remove the directories above" && ask "Remove them (this deletes secrets and the usage ledger)?"; then
        for dir in $found; do
          rm -rf "$dir"
        done
        say "purged"
      fi
    fi
  fi
  exit 0
fi

case "$version" in
  latest) ;;
  [0-9]*.[0-9]*.[0-9]*)
    case "$version" in
      *[!0-9.]*) die "version $version is not MAJOR.MINOR.PATCH" ;;
    esac ;;
  *) die "version $version is not MAJOR.MINOR.PATCH" ;;
esac

system=${LLMGATE_UNAME_S:-$(uname -s)}
machine=${LLMGATE_UNAME_M:-$(uname -m)}
case "$system" in
  Darwin) os=darwin ;;
  Linux) os=linux ;;
  *) die "unsupported system $system (on Windows, install inside WSL)" ;;
esac
case "$machine" in
  arm64|aarch64) arch=arm64 ;;
  x86_64|amd64) arch=x86_64 ;;
  *) die "unsupported architecture $machine" ;;
esac

if [ "$dry_run" -eq 0 ]; then
  mkdir -p "$state_dir"
  log=$state_dir/install-$(date +%Y%m%d-%H%M%S).log
  : > "$log"
  chmod 600 "$log"
fi

fetch() {
  curl -fsSL --retry 3 --connect-timeout 15 -o "$2" "$1"
}

if [ "$version" = latest ]; then
  [ -z "$base_url" ] || die "--base-url needs --version"
  final=$(curl -fsSIL --retry 3 --connect-timeout 15 -o /dev/null -w '%{url_effective}' "https://github.com/$repo/releases/latest") \
    || die "cannot reach github.com"
  tag=${final##*/}
  version=${tag#v}
  case "$version" in
    [0-9]*.[0-9]*.[0-9]*) ;;
    *) die "the latest release of $repo has no version tag (got $tag)" ;;
  esac
fi

release_url=${base_url:-https://github.com/$repo/releases/download/v$version}
asset=llmgate-$version-$os-$arch.tar.gz
bin=$prefix/llmgate

say "llmgate $version for $os-$arch into $prefix"

installed=
if [ -x "$bin" ]; then
  installed=$("$bin" --version 2>/dev/null || true)
fi

if [ "$installed" = "llmgate $version" ] && [ "$force" -eq 0 ]; then
  say "llmgate $version is already installed at $bin"
elif step "download $release_url/$asset, check it against SHA256SUMS.txt and install it to $bin"; then
  work=$(mktemp -d)
  trap 'rm -rf "$work"' EXIT
  fetch "$release_url/$asset" "$work/$asset" || die "cannot download $release_url/$asset"
  fetch "$release_url/SHA256SUMS.txt" "$work/SHA256SUMS.txt" || die "cannot download $release_url/SHA256SUMS.txt"
  expected=$(awk -v name="$asset" '$2 == name { print $1 }' "$work/SHA256SUMS.txt")
  [ -n "$expected" ] || die "$asset is not listed in SHA256SUMS.txt"
  if command -v sha256sum >/dev/null 2>&1; then
    actual=$(sha256sum "$work/$asset" | awk '{ print $1 }')
  else
    actual=$(shasum -a 256 "$work/$asset" | awk '{ print $1 }')
  fi
  [ "$expected" = "$actual" ] || die "checksum mismatch for $asset; nothing was installed"
  say "checksum ok"
  tar -xzf "$work/$asset" -C "$work"
  fresh=$work/llmgate-$version-$os-$arch/llmgate
  [ -x "$fresh" ] || die "$asset does not contain llmgate"
  reported=$("$fresh" --version) || die "the downloaded binary does not run on this machine"
  [ "$reported" = "llmgate $version" ] || die "the downloaded binary reports '$reported', expected 'llmgate $version'"
  mkdir -p "$prefix"
  if [ -e "$bin" ]; then
    cp -p "$bin" "$bin.previous"
    say "kept the old binary as $bin.previous"
  fi
  cp "$fresh" "$bin.new"
  chmod 0755 "$bin.new"
  mv -f "$bin.new" "$bin"
  say "installed $bin"
fi

case ":$PATH:" in
  *":$prefix:"*) ;;
  *) say "$prefix is not on your PATH; add this line to your shell profile:"
     say "  export PATH=\"$prefix:\$PATH\"" ;;
esac

if ! command -v claude >/dev/null 2>&1; then
  say "Claude Code is not installed."
  if [ "$with_claude" -eq 1 ] || { [ "$yes" -eq 0 ] && ask "Install Claude Code with its official installer (https://claude.ai/install.sh)?"; }; then
    if step "curl -fsSL https://claude.ai/install.sh | bash"; then
      curl -fsSL https://claude.ai/install.sh | bash || say "Claude Code's installer failed; install it yourself and run this script again"
    fi
  else
    say "Install it from https://claude.ai/install.sh, then run llmgate again."
  fi
fi

if [ -n "$org" ]; then
  set -- org install "$org"
  [ -z "$org_ref" ] || set -- "$@" --ref "$org_ref"
  [ "$yes" -eq 0 ] || set -- "$@" --yes
  if [ "$dry_run" -eq 1 ]; then
    say "would: run llmgate $*"
  elif [ "$yes" -eq 1 ]; then
    "$bin" "$@" < /dev/null || die "llmgate org install failed; nothing else was changed"
  elif have_tty; then
    "$bin" "$@" < "$tty" || die "llmgate org install did not finish; run it again: $bin $*"
  else
    say "no terminal to confirm the organisation pack; run: $bin $*"
  fi
fi

if [ "$run_init" -eq 1 ]; then
  if [ "$dry_run" -eq 1 ]; then
    say "would: run llmgate init (or llmgate setup)"
  elif have_tty; then
    if "$bin" --no-such-flag 2>&1 < /dev/null | grep -q '| init '; then
      say "running llmgate init"
      "$bin" init < "$tty" || say "llmgate init did not finish; run it again when you are ready"
    else
      say "running llmgate setup"
      "$bin" setup < "$tty" || say "llmgate setup did not finish; run it again when you are ready"
    fi
  else
    say "no terminal for questions; run: $bin setup"
  fi
fi

say "done; start Claude Code through the gateway with: llmgate claude"
