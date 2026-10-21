#!/usr/bin/env python3
"""Run the compatibility scenarios against one backend and compare with the committed expectation."""
import argparse
import datetime
import json
import os
import pathlib
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import urllib.request

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent.parent
PASS, FAIL, UNSUPPORTED, NOT_RUN = "PASS", "FAIL", "UNSUPPORTED", "NOT_RUN"
DEFAULT_TIMEOUT_SECONDS = 300
DETAIL_LIMIT = 600


def load(path):
    return json.loads(path.read_text())


def scenarios(only):
    found = {p.name: load(p / "scenario.json") for p in sorted((HERE / "scenarios").iterdir()) if (p / "scenario.json").exists()}
    return {k: v for k, v in found.items() if not only or k in only}


def tail(text):
    return " ".join(text.split())[-DETAIL_LIMIT:]


def start_fake_upstream(work):
    port_file, log = work / "upstream.port", work / "upstream.log"
    env = dict(os.environ, PYTHONPATH=str(ROOT / "tests/e2e"))
    process = subprocess.Popen([sys.executable, str(ROOT / "tests/e2e/fake_anthropic.py"), str(port_file), str(log)], env=env)
    for _ in range(100):
        if port_file.exists() and port_file.read_text():
            return process, int(port_file.read_text())
        time.sleep(0.1)
    process.kill()
    raise SystemExit("the scripted upstream did not start")


def free_port():
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def write_scripted_config(work, upstream_port):
    config = work / "llmgate.jsonc"
    base = f"http://127.0.0.1:{upstream_port}"
    provider = lambda model: {"base_url": base, "static_token_env": "COMPAT_TOKEN", "models": {"claude-sonnet-5-5": model}}
    config.write_text(json.dumps({
        "listen": f"127.0.0.1:{free_port()}",
        "providers": {"a": provider("m-a"), "b": provider("m-b"), "dead": dict(provider("m-dead"), base_url="http://127.0.0.1:1")},
        "claude": {"profile_dir": str(work / "profile")}}))
    return config


def scripted_variant(config, overrides, work, name):
    merged = dict(json.loads(config.read_text()), **overrides)
    merged["listen"] = f"127.0.0.1:{free_port()}"
    path = work / f"llmgate-{name}.jsonc"
    path.write_text(json.dumps(merged))
    return path


def stats_of(binary, config, env):
    try:
        shown = subprocess.run([binary, "--config", str(config), "env", "--json"], capture_output=True, text=True, env=env, timeout=30)
        base = json.loads(shown.stdout)["ANTHROPIC_BASE_URL"].rsplit("/anthropic", 1)[0]
        return json.load(urllib.request.urlopen(base + "/llmgate/stats", timeout=10))
    except (OSError, ValueError, KeyError, subprocess.SubprocessError):
        return {}


def keep_transcripts(sandbox, out, backend, name):
    for item in sandbox.glob("*.jsonl"):
        shutil.copy(item, out / f"{backend}-{name}-{item.name}")


def run_script(name, spec, sandbox, env, keep):
    try:
        ran = subprocess.run(["sh", "-c", f". {HERE / 'lib.sh'}; . {HERE / 'scenarios' / name / 'run.sh'}"], cwd=sandbox, env=env, stdin=subprocess.DEVNULL,
                             capture_output=True, text=True, timeout=spec.get("timeout", DEFAULT_TIMEOUT_SECONDS))
    except subprocess.TimeoutExpired:
        return FAIL, "timed out"
    finally:
        keep_transcripts(sandbox, *keep)
    return (PASS, "") if ran.returncode == 0 else (FAIL, tail(ran.stdout + ran.stderr))


def run_one(name, spec, backend, ctx):
    missing = sorted(set(spec.get("needs", [])) - set(backend["provides"]))
    if missing:
        return UNSUPPORTED, "backend does not provide: " + ", ".join(missing)
    sandbox = ctx["work"] / "sandbox" / name
    sandbox.mkdir(parents=True)
    files = HERE / "scenarios" / name / "files"
    if files.exists():
        shutil.copytree(files, sandbox, dirs_exist_ok=True)
    transcript = ctx["out"] / f"{ctx['backend']}-{name}.jsonl"
    env = dict(ctx["env"], COMPAT_SCENARIO_DIR=str(HERE / "scenarios" / name), TRANSCRIPT=str(transcript), LLMGATE_BIN=ctx["binary"], LLMGATE_CONFIG=str(ctx["config"]),
               **{f"COMPAT_{k.upper()}": v for k, v in backend.get("switch", {}).items()})
    own_config = None
    if "scripted_config" in spec and ctx["scripted"]:
        own_config = scripted_variant(ctx["config"], spec["scripted_config"], ctx["work"], name)
        env["LLMGATE_CONFIG"] = str(own_config)
    if (HERE / "scenarios" / name / "run.sh").exists():
        try:
            return run_script(name, spec, sandbox, env, (ctx["out"], ctx["backend"], name))
        finally:
            if own_config:
                subprocess.run([ctx["binary"], "--config", str(own_config), "stop"], env=env, capture_output=True)
    command = [ctx["binary"], "--config", str(ctx["config"]), "claude", "-p", spec["prompt"], "--output-format", "stream-json", "--verbose",
               "--no-session-persistence", "--model", spec.get("model", "sonnet")] + spec.get("claude_args", [])
    try:
        ran = subprocess.run(command, cwd=sandbox, env=env, stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=spec.get("timeout", DEFAULT_TIMEOUT_SECONDS))
    except subprocess.TimeoutExpired:
        return FAIL, "timed out"
    transcript.write_text(ran.stdout)
    if ran.returncode != 0:
        return FAIL, f"claude exited {ran.returncode}: {tail(ran.stderr)}"
    checked = subprocess.run(["sh", "-c", f". {HERE / 'lib.sh'}; . {HERE / 'scenarios' / name / 'check.sh'}"], cwd=sandbox, env=env, capture_output=True, text=True)
    return (PASS, "") if checked.returncode == 0 else (FAIL, "the check failed: " + tail(checked.stdout + checked.stderr))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--backend", required=True)
    parser.add_argument("--binary", default=str(ROOT / "build/host/llmgate"))
    parser.add_argument("--config", help="gateway config for a real backend (a scripted backend writes its own)")
    parser.add_argument("--claude", help="claude executable for LLMGATE_CLAUDE (the scripted backend uses a stub)")
    parser.add_argument("--scenario", action="append", default=[])
    parser.add_argument("--out", default=str(ROOT / "build/compat"))
    parser.add_argument("--update-expected", action="store_true")
    parser.add_argument("--idle-stub", action="store_true", help="scripted only: a stub that does nothing, to prove every check can fail")
    args = parser.parse_args()
    backend = load(HERE / "backends" / f"{args.backend}.json")
    out = pathlib.Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    work = pathlib.Path(tempfile.mkdtemp(prefix="compat-"))
    env = dict(os.environ, XDG_STATE_HOME=str(work / "state"), COMPAT_TOKEN="compat-token")
    if backend["kind"] == "scripted":
        env.update(HOME=str(work / "home"), XDG_CONFIG_HOME=str(work / "config"))
        (work / "home").mkdir()
    upstream = None
    try:
        if backend["kind"] == "scripted":
            upstream, port = start_fake_upstream(work)
            config = write_scripted_config(work, port)
            env["LLMGATE_CLAUDE"] = str(HERE / "stub_claude_launcher.sh")
            if args.idle_stub:
                env["COMPAT_STUB_MODE"] = "idle"
        else:
            if not args.config:
                raise SystemExit("a real backend needs --config")
            config = pathlib.Path(args.config)
            if args.claude:
                env["LLMGATE_CLAUDE"] = args.claude
        ctx = {"work": work, "out": out, "env": env, "binary": args.binary, "config": config, "backend": args.backend, "scripted": backend["kind"] == "scripted"}
        chosen = scenarios(set(args.scenario))
        results = {name: run_one(name, spec, backend, ctx) for name, spec in chosen.items() if name in backend.get("only", chosen)}
        stats = stats_of(args.binary, config, env)
    finally:
        subprocess.run([args.binary, "--config", str(locals().get("config", "/dev/null")), "stop"], env=env, capture_output=True)
        if upstream:
            upstream.kill()
        shutil.rmtree(work, ignore_errors=True)
    claude_version = "stub" if backend["kind"] == "scripted" else subprocess.run([args.claude or "claude", "--version"], capture_output=True, text=True).stdout.strip()
    report = {"backend": args.backend, "claude_version": claude_version, "date": datetime.date.today().isoformat(),
              "results": {k: {"status": s, "detail": d} for k, (s, d) in results.items()}, "betas": stats.get("betas", {})}
    (out / f"{args.backend}.json").write_text(json.dumps(report, indent=2) + "\n")
    expected_path = HERE / "expected" / f"{args.backend}.json"
    if args.update_expected:
        merged = load(expected_path) if expected_path.exists() else {}
        merged.update({k: s for k, (s, _) in results.items()})
        expected_path.write_text(json.dumps(dict(sorted(merged.items())), indent=2) + "\n")
    expected = load(expected_path) if expected_path.exists() else {}
    mismatches = 0
    for name, (status, detail) in results.items():
        want = expected.get(name, NOT_RUN)
        flag = "ok  " if want == status else "DIFF"
        mismatches += want != status
        print(f"{flag} {name:14s} {status:11s} (expected {want}){'  ' + detail if detail and status != PASS else ''}")
    sys.exit(1 if mismatches and not args.update_expected else 0)


main()
