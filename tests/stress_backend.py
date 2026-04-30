# /// script
# requires-python = ">=3.10"
# dependencies = [
#     "requests>=2.31",
# ]
# ///
"""Pure-Python backend stress + measurement driver.

No Playwright. No frontend interaction. Spawns the ETW churn driver
(stress_etw.py), exercises hijack/unhijack via HTTP, tails clew.log,
and reports per-stage backend pipeline latencies.

Setup
    1. start clew.exe in an admin shell with log_level=debug
       (edit clew.json or use the Settings UI)
    2. in another shell, run this script (no admin required for the
       script itself; only clew.exe needs admin)

Usage
    uv run --script tests/stress_backend.py --scenario churn
    uv run --script tests/stress_backend.py --scenario steady --target 500
    uv run --script tests/stress_backend.py --scenario churn --visibility-cycle

Scenarios
    churn   — high event rate, moderate tree (target=200, life 2-5s)
    steady  — low event rate, large tree (target=500, life 30s)

Output
    Per-metric summary: n, min, p50, p95, p99, max (microseconds).
    ETW event rate, tree size range, strand utilization estimate.
    Hijack/unhijack RTT distribution.
    With --visibility-cycle: separate buckets for visible vs hidden.
"""
from __future__ import annotations

import argparse
import contextlib
import ctypes
import ctypes.wintypes as wintypes
import os
import pathlib
import re
import signal
import subprocess
import sys
import time
from collections import defaultdict
from datetime import datetime, timezone

import requests


# ---------------------------------------------------------------- paths/const

REPO_ROOT     = pathlib.Path(__file__).resolve().parents[1]
STRESS_SCRIPT = REPO_ROOT / "tests" / "stress_etw.py"
LOG_PATH      = REPO_ROOT / "clew.log"
EXE           = REPO_ROOT / "build" / "Release" / "clew.exe"
BASE          = "http://127.0.0.1:18080/api"

# Win32 — minimal, only what we need to drive visibility transitions.
WINDOW_CLASS  = "ClewWebViewClass"
WM_SYSCOMMAND = 0x0112
SC_MINIMIZE   = 0xF020
SC_RESTORE    = 0xF120


# ----------------------------------------------------------------- win32 hwnd

def find_clew_hwnd() -> int | None:
    user32 = ctypes.windll.user32
    EnumWindowsProc = ctypes.WINFUNCTYPE(
        wintypes.BOOL, wintypes.HWND, wintypes.LPARAM,
    )
    found: list[int] = []
    def cb(hwnd, _):
        buf = ctypes.create_unicode_buffer(256)
        user32.GetClassNameW(hwnd, buf, 256)
        if buf.value == WINDOW_CLASS:
            found.append(int(hwnd))
            return False
        return True
    user32.EnumWindows(EnumWindowsProc(cb), 0)
    return found[0] if found else None


def post_wm(hwnd: int, msg: int, wparam: int, lparam: int) -> bool:
    return bool(ctypes.windll.user32.PostMessageW(hwnd, msg, wparam, lparam))


# ------------------------------------------------------------- clew.log parse

# Quill format: "%(time) [%(log_level_short_code)] %(message)"
# Time format: "%Y-%m-%d %H:%M:%S.%Qus" (microseconds).
# We log at INFO ([I]); accept [D] too in case the macro is changed back.
TRACE_LINE_RE = re.compile(
    r'^(?P<ts>\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{6})\s+'
    r'\[[IDW]\]\s+'
    r'\[trace\]\s+'
    r'(?P<kind>\w+)'
    r'(?P<rest>.*)$'
)


def parse_ts(s: str) -> float:
    # quill timestamps are local time, naive. Match against time.time().
    return datetime.strptime(s, "%Y-%m-%d %H:%M:%S.%f").timestamp()


def parse_kv(s: str) -> dict[str, int | str]:
    out: dict[str, int | str] = {}
    for tok in s.strip().split():
        if "=" not in tok:
            continue
        k, v = tok.split("=", 1)
        try:
            out[k] = int(v)
        except ValueError:
            out[k] = v
    return out


# ------------------------------------------------------- log slice reader
#
# Real-time tailing via Python text-mode + quill backend buffering on Windows
# proved unreliable (lines that are clearly in the file at end-of-test were
# not seen by an open-and-seek-to-EOF FD). Switched to: read the whole file
# at end of phase, filter to events whose timestamp falls inside the phase
# wall-clock window. log is a few MB at most for our test durations, so
# scanning the whole file is fine.

def collect_trace_events(path: pathlib.Path,
                         ts_start: float,
                         ts_end: float) -> list[dict]:
    out: list[dict] = []
    if not path.exists():
        return out
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            m = TRACE_LINE_RE.match(line.rstrip("\n"))
            if not m:
                continue
            ts = parse_ts(m.group("ts"))
            if ts < ts_start or ts > ts_end:
                continue
            d: dict = parse_kv(m.group("rest"))
            d["_ts"]   = ts
            d["_kind"] = m.group("kind")
            out.append(d)
    return out


# --------------------------------------------------------------- stat helpers

def stats(values: list[int]) -> str:
    if not values:
        return "n=0"
    s = sorted(values)
    n = len(s)
    def pct(p: float) -> int:
        idx = min(n - 1, max(0, int(round(p * (n - 1)))))
        return s[idx]
    return (f"n={n} min={s[0]} p50={pct(0.50)} p95={pct(0.95)} "
            f"p99={pct(0.99)} max={s[-1]}")


# ----------------------------------------------------------- helper subprocess

def spawn_long_lived_helper(seconds: int = 600) -> subprocess.Popen[bytes]:
    """Stable target for hijack/unhijack measurement. ping for `seconds`."""
    return subprocess.Popen(
        ["cmd.exe", "/c", "ping", "127.0.0.1", "-n", str(seconds)],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )


def wait_pid_in_clew(pid: int, timeout: float = 5.0) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            r = requests.get(f"{BASE}/processes/{pid}", timeout=1.0)
            if r.status_code == 200:
                return True
        except Exception:
            pass
        time.sleep(0.1)
    return False


def hijack(pid: int) -> tuple[int, float]:
    t0 = time.monotonic()
    r = requests.post(f"{BASE}/hijack/{pid}",
                      json={"tree": False, "group_id": 0}, timeout=30.0)
    return r.status_code, (time.monotonic() - t0) * 1e6  # microseconds


def unhijack(pid: int) -> tuple[int, float]:
    t0 = time.monotonic()
    r = requests.delete(f"{BASE}/hijack/{pid}", data="", timeout=30.0)
    return r.status_code, (time.monotonic() - t0) * 1e6


# ----------------------------------------------------------- measurement loop

def run_phase(label: str, secs: float,
              target_pid: int, hijack_interval: float) -> dict:
    """Run a phase: alternate hijack/unhijack at fixed cadence, return
    raw timing data. Trace events are collected from clew.log AFTER the
    phase ends (post-hoc filter by wall-clock window)."""
    print(f"\n=== phase: {label} ({secs:.1f}s) ===")
    rtts_hijack:   list[float] = []
    rtts_unhijack: list[float] = []
    next_action = time.monotonic()
    is_hijacked = False

    ts_start = time.time()
    end_at = time.monotonic() + secs
    while time.monotonic() < end_at:
        if time.monotonic() >= next_action:
            if not is_hijacked:
                code, rtt_us = hijack(target_pid)
                if code == 200:
                    rtts_hijack.append(rtt_us)
                    is_hijacked = True
            else:
                code, rtt_us = unhijack(target_pid)
                if code == 200:
                    rtts_unhijack.append(rtt_us)
                    is_hijacked = False
            next_action = time.monotonic() + hijack_interval
        time.sleep(0.05)

    # Restore unhijacked at phase end so the next phase starts clean.
    if is_hijacked:
        unhijack(target_pid)
    ts_end = time.time() + 1.0  # +1s headroom for in-flight log writes

    # Wait for quill to flush trailing writes.
    time.sleep(1.5)

    events = collect_trace_events(LOG_PATH, ts_start, ts_end)
    print(f"    collected {len(events)} trace events from log")

    return {
        "label": label,
        "secs":  secs,
        "events": events,
        "rtts_hijack":   rtts_hijack,
        "rtts_unhijack": rtts_unhijack,
    }


# ----------------------------------------------------------------- reporting

def report(phase: dict) -> None:
    label  = phase["label"]
    secs   = phase["secs"]
    events = phase["events"]

    print(f"\n--- {label}: {secs:.1f}s, {len(events)} trace events ---")

    by_kind: dict[str, list[dict]] = defaultdict(list)
    for e in events:
        by_kind[e["_kind"]].append(e)

    # ETW arrival rate (off-strand, before any queueing).
    etw_cb = by_kind.get("etw_cb", [])
    starts = sum(1 for e in etw_cb if e.get("id") == "START")
    stops  = sum(1 for e in etw_cb if e.get("id") == "STOP")
    print(f"ETW: {len(etw_cb)} events ({starts} START, {stops} STOP) = "
          f"{len(etw_cb)/max(secs,1e-9):.1f}/sec")

    # Strand: per-event work breakdown (apply + notify) from etw_strand_in.
    etw_strand = by_kind.get("etw_strand_in", [])
    apply_us  = [int(e["apply_us"])  for e in etw_strand if "apply_us"  in e]
    notify_us = [int(e["notify_us"]) for e in etw_strand if "notify_us" in e]
    trees     = [int(e["tree"])      for e in etw_strand if "tree"      in e]
    if etw_strand:
        print(f"strand etw apply_us:  {stats(apply_us)}")
        print(f"strand etw notify_us: {stats(notify_us)}")
        if trees:
            print(f"tree size: min={min(trees)} max={max(trees)} "
                  f"median={sorted(trees)[len(trees)//2]}")

    # Projection (refresh + push), only fires when visible.
    # Three trace kinds depending on build / urgency:
    #   - proj urgency=immediate refresh_us=N push_us=N tree=N   (immediate path)
    #   - proj urgency=batched scheduled tree=N                  (new coalesce: refresh deferred)
    #   - proj urgency=batched refresh_us=N push_us=coalesced... (old coalesce: refresh per-event)
    #   - proj urgency=n/a refresh_us=N push_us=N mode=no-coalesce (CLEW_NO_COALESCE build)
    #   - proj_timer_flush refresh_us=N push_us=N tree=N         (new coalesce timer fired)
    #   - proj urgency=X skipped=hidden                          (visibility gate)
    proj          = by_kind.get("proj", [])
    proj_flush    = by_kind.get("proj_timer_flush", [])
    proj_skipped  = sum(1 for e in proj if e.get("skipped") == "hidden")
    proj_inline   = [e for e in proj if "refresh_us" in e]      # has refresh inline
    proj_deferred = [e for e in proj if e.get("scheduled") == 1 or
                     "scheduled" in e]                           # new coalesce batched
    refresh_us_inline = [int(e["refresh_us"]) for e in proj_inline]
    push_us_inline    = [int(e["push_us"]) for e in proj_inline
                         if isinstance(e.get("push_us"), int)]
    refresh_us_flush  = [int(e["refresh_us"]) for e in proj_flush]
    push_us_flush     = [int(e["push_us"])    for e in proj_flush]

    print(f"projection: {len(proj)+len(proj_flush)} total "
          f"({len(proj_inline)} inline-refresh, "
          f"{len(proj_deferred)} batched-scheduled, "
          f"{len(proj_flush)} timer-flushes, "
          f"{proj_skipped} skipped-hidden)")
    if proj_inline:
        print(f"  inline refresh_us: {stats(refresh_us_inline)}")
        if push_us_inline:
            print(f"  inline push_us:    {stats(push_us_inline)}")
    if proj_flush:
        print(f"  flush  refresh_us: {stats(refresh_us_flush)}")
        print(f"  flush  push_us:    {stats(push_us_flush)}")

    # Hijack roundtrip from log (svc_post_command total_us) + strand wait.
    svc_post = by_kind.get("svc_post_command", [])
    hijack_total_us = [int(e["total_us"]) for e in svc_post
                       if e.get("op") == "hijack"]
    unhijack_total_us = [int(e["total_us"]) for e in svc_post
                         if e.get("op") == "unhijack"]
    strand_exec = by_kind.get("strand_exec", [])
    hijack_wait_us = [int(e["wait_us"]) for e in strand_exec
                      if e.get("op") == "hijack"]
    unhijack_wait_us = [int(e["wait_us"]) for e in strand_exec
                        if e.get("op") == "unhijack"]
    if hijack_total_us or unhijack_total_us:
        print("hijack/unhijack (server-side, from log):")
        print(f"  hijack   total_us: {stats(hijack_total_us)}")
        print(f"  hijack   wait_us:  {stats(hijack_wait_us)}")
        print(f"  unhijack total_us: {stats(unhijack_total_us)}")
        print(f"  unhijack wait_us:  {stats(unhijack_wait_us)}")

    # Wall-clock RTT measured from the requesting Python process.
    rtts_h = [int(x) for x in phase["rtts_hijack"]]
    rtts_u = [int(x) for x in phase["rtts_unhijack"]]
    if rtts_h or rtts_u:
        print("hijack/unhijack RTT (wall-clock, from python):")
        if rtts_h: print(f"  hijack:   {stats(rtts_h)}")
        if rtts_u: print(f"  unhijack: {stats(rtts_u)}")

    # Strand utilization estimate: sum of strand-side work / phase duration.
    # Includes ETW strand work (apply + notify) AND timer-flush work (in
    # coalesce-with-refresh build, refresh+push runs in timer callback on
    # the strand, so we need to count it explicitly).
    flush_work_us = sum(refresh_us_flush) + sum(push_us_flush)
    strand_work_us = sum(apply_us) + sum(notify_us) + flush_work_us
    util_pct = 100.0 * strand_work_us / (secs * 1e6)
    print(f"strand busy estimate: {strand_work_us/1e3:.1f}ms over {secs:.1f}s "
          f"= {util_pct:.1f}% "
          f"(etw {(sum(apply_us)+sum(notify_us))/1e3:.0f}ms + "
          f"flush {flush_work_us/1e3:.0f}ms)")


# -------------------------------------------------------------------- main

def main() -> int:
    p = argparse.ArgumentParser(formatter_class=argparse.RawDescriptionHelpFormatter,
                                description=__doc__.splitlines()[0])
    p.add_argument("--scenario",  choices=["churn", "steady"], default="churn")
    p.add_argument("--target",    type=int,   default=None,
                   help="override scenario default target")
    p.add_argument("--min-life",  type=float, default=None,
                   help="override scenario default min-life (seconds per child)")
    p.add_argument("--max-life",  type=float, default=None,
                   help="override scenario default max-life")
    p.add_argument("--measure-secs", type=float, default=20.0,
                   help="duration of each measurement phase")
    p.add_argument("--hijack-interval", type=float, default=2.0)
    p.add_argument("--warmup-secs", type=float, default=5.0,
                   help="time to wait after stress_etw start before measuring")
    p.add_argument("--visibility-cycle", action="store_true",
                   help="run minimize/restore cycle midway and bucket results")
    p.add_argument("--no-stress", action="store_true",
                   help="skip stress_etw (useful for baseline/idle measurement)")
    args = p.parse_args()

    if not LOG_PATH.exists():
        print(f"ERROR: {LOG_PATH} not found. Start clew.exe first.", file=sys.stderr)
        return 1
    try:
        requests.get(f"{BASE}/stats", timeout=2).raise_for_status()
    except Exception as e:
        print(f"ERROR: clew not reachable at {BASE} ({e})", file=sys.stderr)
        return 1

    # Scenario defaults — override-able via --target / --min-life / --max-life.
    if args.scenario == "churn":
        target  = args.target if args.target else 200
        min_life, max_life = 2.0, 5.0
    else:  # steady
        target  = args.target if args.target else 500
        min_life, max_life = 25.0, 35.0
    if args.min_life is not None: min_life = args.min_life
    if args.max_life is not None: max_life = args.max_life

    # Spawn a long-lived helper to hijack against. ping covers entire run.
    helper_secs = int(args.warmup_secs + args.measure_secs * 3 + 30)
    helper = spawn_long_lived_helper(helper_secs)
    target_pid = helper.pid
    print(f"==> helper pid={target_pid} (ping -n {helper_secs})")

    if not wait_pid_in_clew(target_pid):
        helper.kill()
        print(f"ERROR: helper pid {target_pid} did not appear in clew tree",
              file=sys.stderr)
        return 1

    stress = None
    if not args.no_stress:
        cmd = ["uv", "run", "--script", str(STRESS_SCRIPT),
               "--target", str(target),
               "--min-life", str(min_life),
               "--max-life", str(max_life)]
        print(f"==> launching stress_etw: target={target} life={min_life}-{max_life}s")
        stress = subprocess.Popen(
            cmd,
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            creationflags=subprocess.CREATE_NEW_PROCESS_GROUP,
        )
        print(f"==> warmup {args.warmup_secs}s ...")
        time.sleep(args.warmup_secs)

    phases: list[dict] = []
    try:
        if not args.visibility_cycle:
            phases.append(run_phase("steady", args.measure_secs,
                                    target_pid, args.hijack_interval))
        else:
            hwnd = find_clew_hwnd()
            if hwnd is None:
                print("ERROR: ClewWebViewClass hwnd not found "
                      "(need GUI mode + clew.exe to be visible at start)",
                      file=sys.stderr)
                return 1
            print(f"==> hwnd=0x{hwnd:x}")
            phases.append(run_phase("visible-pre",
                                    args.measure_secs,
                                    target_pid, args.hijack_interval))
            print("\n==> minimize -> SC_MINIMIZE")
            post_wm(hwnd, WM_SYSCOMMAND, SC_MINIMIZE, 0)
            time.sleep(0.5)
            phases.append(run_phase("hidden",
                                    args.measure_secs,
                                    target_pid, args.hijack_interval))
            print("\n==> restore -> SC_RESTORE")
            post_wm(hwnd, WM_SYSCOMMAND, SC_RESTORE, 0)
            time.sleep(0.5)
            phases.append(run_phase("visible-post",
                                    args.measure_secs,
                                    target_pid, args.hijack_interval))
    finally:
        if stress is not None:
            try:
                stress.send_signal(signal.CTRL_BREAK_EVENT)
                stress.wait(timeout=5)
            except Exception:
                with contextlib.suppress(Exception):
                    subprocess.run(
                        ["taskkill", "/F", "/T", "/PID", str(stress.pid)],
                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                    )
        with contextlib.suppress(Exception):
            helper.kill()
            helper.wait(timeout=2)

    print()
    print("=" * 60)
    print(f"Backend stress report — scenario={args.scenario} target={target}")
    print("=" * 60)
    for phase in phases:
        report(phase)

    return 0


if __name__ == "__main__":
    sys.exit(main())
