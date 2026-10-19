#!/bin/sh
set -eu
root=$(cd "$(dirname "$0")/../.." && pwd)
work=$(mktemp -d)
trap 'kill $upstream_pid $github_pid 2>/dev/null || true; wait 2>/dev/null || true; rm -rf "$work"' EXIT

FAKE_TOOL_COMMAND="touch $work/denied-marker" python3 "$root/tests/e2e/fake_anthropic.py" "$work/upstream.port" "$work/upstream.log" tool &
upstream_pid=$!
while [ ! -s "$work/upstream.port" ]; do sleep 0.1; done
upstream_port=$(cat "$work/upstream.port")
python3 "$root/tests/e2e/fake_github.py" "$work/github.port" "http://127.0.0.1:$upstream_port" &
github_pid=$!
while [ ! -s "$work/github.port" ]; do sleep 0.1; done
github_url="http://127.0.0.1:$(cat "$work/github.port")"

LLMGATE_REAL=1 LLMGATE_GHE_HOST=rehearsal.invalid LLMGATE_GITHUB_URL="$github_url" LLMGATE_GITHUB_API_URL="$github_url" \
  LLMGATE_COPILOT_BASE_URL="http://127.0.0.1:$upstream_port" LLMGATE_REPORT_DIR="$work/report" LLMGATE_SAMPLE_MUST_PASS=0 \
  sh "$root/tests/e2e/real_run.sh"
grep -q "result: PASS" "$work/report/report.md"
if [ -e "$work/denied-marker" ]; then
  echo "FAIL the permission scope let a command outside the sample run" >&2
  exit 1
fi
grep -q "refused by the permission scope: [1-9]" "$work/report/report.md"
echo "ok   a write outside the sample was refused and counted"
echo "rehearsal: the real-run script passes against fakes"
