diagnose_gateway() {
  echo "== diagnostics (work directory: $work)"
  echo "-- gateway log, last 40 lines"
  tail -n 40 "$work/state/llmgate/gateway.log" 2>/dev/null || echo "(no gateway log)"
  echo "-- state directory"
  ls -la "$work/state/llmgate" 2>&1 | head -20
  echo "-- pid file: $(cat "$work/state/llmgate/gateway.pid" 2>/dev/null || echo none)"
  echo "-- llmgate and python processes"
  ps -ef 2>/dev/null | grep -E "llmgate|fake_|upstream.py|github.py" | grep -v grep | cut -c1-180 | head -10
  if [ -n "${port:-}" ]; then
    echo "-- health probe on 127.0.0.1:$port"
    curl -sS -m 5 -i "http://127.0.0.1:$port/healthz?challenge=00ff" 2>&1 | head -12
  fi
}
