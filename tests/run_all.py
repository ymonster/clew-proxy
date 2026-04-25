"""
Clew end-to-end test harness.

Runs tests/e2e_api_test.py with the full lifecycle handled automatically:
  - admin sanity check
  - launch build/Release/clew.exe (if not already running)
  - wait for /api/stats to become ready
  - invoke the e2e test module as a subprocess
  - restore clew.json if modified during the run
  - taskkill the spawned clew.exe on the way out

Requires an administrator shell. Windows does NOT fork a UAC prompt when
CreateProcess is issued from an already-elevated token, so this works
unattended inside CI or developer shells that launched Claude Code /
PowerShell with "Run as administrator".

Usage:
    python tests/run_all.py              # launch our own clew.exe
    python tests/run_all.py --no-launch  # use an already-running one
"""

from __future__ import annotations

import argparse
import ctypes
import os
import pathlib
import shutil
import subprocess
import sys
import time

import requests


REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
EXE = REPO_ROOT / "build" / "Release" / "clew.exe"
CONFIG = REPO_ROOT / "clew.json"
CONFIG_BACKUP = REPO_ROOT / "clew.json.bak-e2e"
READY_URL = "http://127.0.0.1:18080/api/stats"
READY_TIMEOUT_S = 30.0


def is_elevated() -> bool:
    try:
        return bool(ctypes.windll.shell32.IsUserAnAdmin())
    except Exception:
        return False


def is_clew_up() -> bool:
    try:
        r = requests.get(READY_URL, timeout=1.5)
        return r.status_code == 200
    except Exception:
        return False


def wait_ready(timeout: float = READY_TIMEOUT_S) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if is_clew_up():
            return True
        time.sleep(0.5)
    return False


def backup_config() -> None:
    if CONFIG.exists():
        shutil.copy2(CONFIG, CONFIG_BACKUP)


def restore_config() -> None:
    if CONFIG_BACKUP.exists():
        # If clew still has the file open we may get denied; retry briefly.
        for _ in range(10):
            try:
                shutil.move(str(CONFIG_BACKUP), str(CONFIG))
                return
            except PermissionError:
                time.sleep(0.2)


def kill_clew(pid: int) -> None:
    subprocess.run(
        ["taskkill", "/F", "/PID", str(pid)],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )


def main() -> int:
    parser = argparse.ArgumentParser(description="Run Clew e2e tests end-to-end.")
    parser.add_argument("--no-launch", action="store_true",
                        help="Use an already-running clew.exe; do not spawn one.")
    args = parser.parse_args()

    if not is_elevated():
        print("ERROR: run_all.py needs an administrator shell "
              "(clew.exe requires elevation via its embedded manifest).",
              file=sys.stderr)
        return 2

    own_pid: int | None = None

    if args.no_launch:
        if not is_clew_up():
            print("ERROR: --no-launch set but clew.exe is not reachable at :18080",
                  file=sys.stderr)
            return 1
        print("Using already-running clew.exe.")
    else:
        if is_clew_up():
            print("clew.exe already running; reusing existing instance.")
        else:
            if not EXE.exists():
                print(f"ERROR: {EXE} not found. Build first:", file=sys.stderr)
                print("       cmake --build build --config Release --target clew",
                      file=sys.stderr)
                return 1
            print(f"Launching {EXE} ...")
            # DETACHED_PROCESS so the child doesn't share our console state
            p = subprocess.Popen(
                [str(EXE)],
                cwd=str(REPO_ROOT),
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                creationflags=subprocess.CREATE_NEW_PROCESS_GROUP,
            )
            own_pid = p.pid
            if not wait_ready():
                print("ERROR: clew.exe did not become ready within "
                      f"{READY_TIMEOUT_S}s", file=sys.stderr)
                kill_clew(own_pid)
                return 1
            print(f"clew.exe is ready (pid={own_pid}).")

    exit_code = 1
    try:
        backup_config()
        result = subprocess.run(
            [sys.executable, str(REPO_ROOT / "tests" / "e2e_api_test.py")],
            cwd=str(REPO_ROOT),
        )
        exit_code = result.returncode
    finally:
        # clew first (so quill releases the log file), then restore config.
        if own_pid is not None:
            print("Stopping clew.exe ...")
            kill_clew(own_pid)
            time.sleep(0.7)
        restore_config()

    return exit_code


if __name__ == "__main__":
    sys.exit(main())
