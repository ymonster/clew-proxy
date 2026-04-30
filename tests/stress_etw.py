# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
"""ETW process churn stress test for Clew.

Maintains a target population of short-lived child processes so that
Windows continuously fires ProcessStart / ProcessStop ETW events. Run
this in one terminal and observe the Clew UI in another to gauge how
the strand + projection + WebView2 push pipeline behaves under load.

This is the realistic test for the "remove coalescing" decision: the
e2e suite is intentionally low-noise and won't tell you whether the
UI feels good when 1000 processes churn in/out per second.

Usage (PEP 723 inline deps, no virtualenv setup needed):

    uv run --script tests/stress_etw.py
    uv run --script tests/stress_etw.py --target 1000 --min-life 1 --max-life 20
    uv run --script tests/stress_etw.py --target 500 --duration 60

Knobs
    --target     Maintain this many alive children at all times. Default 200.
    --min-life   Each child sleeps this many seconds minimum before exiting.
    --max-life   Each child sleeps this many seconds maximum before exiting.
                 Combined with --target, sets the steady-state churn rate
                 (≈ target / mean_life events/sec for both Start and Stop).
    --duration   Stop after this many seconds. Default: run until Ctrl-C.

Output
    One line per second: "alive=N   spawned=M   exited=K   churn~R/s"

Cleanup
    On Ctrl-C, terminates all surviving children before exiting.
    Exit status 0.
"""
from __future__ import annotations

import argparse
import random
import signal
import subprocess
import sys
import time

# Each child is a quiet ping that swallows output. ping is more reliable
# than `timeout /nobreak` when launched without a real console (timeout
# requires interactive input handling, ping does not). One ping is ~1s,
# so life_secs maps directly to ping count.
def make_child(life_secs: float) -> subprocess.Popen[bytes]:
    n = max(1, int(round(life_secs)))
    cmd = ["cmd.exe", "/c", f"ping -n {n} 127.0.0.1 >nul"]
    return subprocess.Popen(
        cmd,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        creationflags=subprocess.CREATE_NEW_PROCESS_GROUP,
    )


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__.splitlines()[0],
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--target",   type=int,   default=200, help="alive child count")
    parser.add_argument("--min-life", type=float, default=2.0, help="child lifespan min (sec)")
    parser.add_argument("--max-life", type=float, default=15.0, help="child lifespan max (sec)")
    parser.add_argument("--duration", type=float, default=0.0,
                        help="stop after N sec (0 = forever, Ctrl-C to stop)")
    args = parser.parse_args()

    if args.min_life <= 0 or args.max_life < args.min_life:
        parser.error("require 0 < min-life <= max-life")
    if args.target <= 0:
        parser.error("--target must be positive")

    children: list[subprocess.Popen[bytes]] = []
    spawned_total = 0
    exited_total = 0
    last_report_at = time.monotonic()
    last_report_spawn = 0
    last_report_exit = 0
    started_at = time.monotonic()

    def cleanup_and_exit(_signum: int = 0, _frame: object = None) -> None:
        # Best-effort terminate. Outliers may already be dead by the time
        # we get here; ignore those.
        for c in children:
            if c.poll() is None:
                try:
                    c.terminate()
                except Exception:
                    pass
        sys.stdout.write("\nstress_etw: terminated, exiting\n")
        sys.exit(0)

    signal.signal(signal.SIGINT, cleanup_and_exit)
    if hasattr(signal, "SIGBREAK"):
        signal.signal(signal.SIGBREAK, cleanup_and_exit)  # type: ignore[attr-defined]

    mean_life = (args.min_life + args.max_life) / 2.0
    sys.stdout.write(
        f"stress_etw: target={args.target} life={args.min_life:.1f}-{args.max_life:.1f}s "
        f"(expected churn ~{args.target/mean_life:.0f} ProcessStart/sec, same for Stop)\n"
    )
    sys.stdout.flush()

    try:
        while True:
            # Reap dead children.
            alive: list[subprocess.Popen[bytes]] = []
            for c in children:
                if c.poll() is None:
                    alive.append(c)
                else:
                    exited_total += 1
            children = alive

            # Spawn up to target. Each iteration spawns a small batch so a
            # single tick doesn't burst-create N processes (which would
            # spike CreateProcess thrash and skew measurements).
            need = args.target - len(children)
            spawn_this_tick = min(need, max(8, args.target // 20))
            for _ in range(spawn_this_tick):
                life = random.uniform(args.min_life, args.max_life)
                try:
                    children.append(make_child(life))
                    spawned_total += 1
                except Exception as e:
                    sys.stdout.write(f"stress_etw: spawn failed: {e}\n")
                    break

            # Once-a-second report.
            now = time.monotonic()
            if now - last_report_at >= 1.0:
                window = now - last_report_at
                churn_in  = (spawned_total - last_report_spawn) / window
                churn_out = (exited_total - last_report_exit) / window
                sys.stdout.write(
                    f"  alive={len(children):4d}  spawned_total={spawned_total:6d}  "
                    f"exited_total={exited_total:6d}  "
                    f"start~{churn_in:5.1f}/s  stop~{churn_out:5.1f}/s\n"
                )
                sys.stdout.flush()
                last_report_at = now
                last_report_spawn = spawned_total
                last_report_exit = exited_total

            if args.duration > 0 and now - started_at >= args.duration:
                cleanup_and_exit()

            time.sleep(0.1)
    except Exception as e:
        sys.stdout.write(f"stress_etw: {e}\n")
        cleanup_and_exit()
        return 1


if __name__ == "__main__":
    sys.exit(main())
