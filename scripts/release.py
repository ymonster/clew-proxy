#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
"""Cut a new Clew release — tag → push → configure → build → zip → GitHub release.

Usage:

    uv run scripts/release.py                  # auto-bump patch from latest tag
    uv run scripts/release.py v0.8.0           # explicit version
    uv run scripts/release.py --dry-run        # preview the plan, change nothing
    uv run scripts/release.py --skip-build     # trust build/ and frontend/dist/ are already current

Safety:
* Aborts if the working tree is dirty.
* Aborts if the target tag already exists.
* With --dry-run, only read-only commands run; destructive steps are printed.
"""
from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from zipfile import ZipFile, ZIP_DEFLATED

ROOT = Path(__file__).resolve().parent.parent

# Files copied verbatim into the release zip root.
RELEASE_FILES = [
    "build/Release/clew.exe",
    "build/Release/WinDivert.dll",
    "build/Release/WinDivert64.sys",
    "build/Release/brotlicommon.dll",
    "build/Release/brotlidec.dll",
    "build/Release/brotlienc.dll",
    "LICENSE",
    "README.md",
    "README.en.md",
]

# (source dir, dest rel path inside staging) — copied recursively.
RELEASE_DIRS = [
    ("frontend/dist", "frontend/dist"),
]

GH_REPO = "ymonster/clew-proxy"


def sh(cmd: list[str], *, cwd: Path = ROOT, check: bool = True) -> str:
    r = subprocess.run(cmd, cwd=cwd, text=True, capture_output=True)
    if check and r.returncode != 0:
        sys.stderr.write(f"\n[FAIL] {' '.join(cmd)}\n{r.stderr}\n")
        sys.exit(r.returncode)
    return r.stdout.strip()


def run_step(label: str, cmd: list[str], *, cwd: Path = ROOT, dry: bool = False) -> None:
    pretty_cwd = cwd.relative_to(ROOT) if cwd != ROOT else Path(".")
    print(f"  {label}: ({pretty_cwd}) {' '.join(cmd)}")
    if dry:
        return
    r = subprocess.run(cmd, cwd=cwd)
    if r.returncode != 0:
        sys.exit(r.returncode)


def bump_patch(tag: str) -> str:
    m = re.match(r"^v(\d+)\.(\d+)\.(\d+)$", tag)
    if not m:
        raise SystemExit(f"Cannot auto-bump '{tag}' — expected vMAJOR.MINOR.PATCH")
    major, minor, patch = map(int, m.groups())
    return f"v{major}.{minor}.{patch + 1}"


def build_notes(prev_tag: str, new_tag: str) -> str:
    log = sh(["git", "log", "--pretty=format:* %s", f"{prev_tag}..HEAD"])
    if not log:
        log = "* (no new commits since previous tag)"
    return (
        f"Changes since [{prev_tag}](https://github.com/{GH_REPO}/releases/tag/{prev_tag}):\n\n"
        f"{log}\n\n"
        f"Full diff: https://github.com/{GH_REPO}/compare/{prev_tag}...{new_tag}"
    )


def package_zip(new_tag: str, zip_path: Path, staging: Path) -> None:
    if staging.exists():
        shutil.rmtree(staging)
    staging.mkdir(parents=True)

    for rel in RELEASE_FILES:
        src = ROOT / rel
        if not src.exists():
            sys.exit(f"Missing release file: {src}")
        shutil.copy2(src, staging / src.name)

    for src_rel, dst_rel in RELEASE_DIRS:
        src = ROOT / src_rel
        if not src.exists():
            sys.exit(f"Missing release dir: {src}")
        shutil.copytree(src, staging / dst_rel)

    if zip_path.exists():
        zip_path.unlink()
    with ZipFile(zip_path, "w", ZIP_DEFLATED) as zf:
        for entry in staging.rglob("*"):
            if entry.is_file():
                zf.write(entry, entry.relative_to(staging.parent))
    print(f"    wrote {zip_path} ({zip_path.stat().st_size:,} bytes)")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("version", nargs="?", help="Target tag, e.g. v0.8.0 (default: bump patch)")
    parser.add_argument("--dry-run", action="store_true", help="Preview the plan without any destructive action")
    parser.add_argument("--skip-build", action="store_true", help="Assume build outputs are already current")
    parser.add_argument("--notes", help="Override auto-generated release notes")
    args = parser.parse_args()

    dry = args.dry_run

    # --- Preflight -----------------------------------------------------------
    print("== Preflight ==")
    status = sh(["git", "status", "--porcelain"])
    if status:
        print("  ERROR: working tree is not clean:")
        print("  " + "\n  ".join(status.splitlines()))
        sys.exit(2)
    print("  working tree clean [OK]")

    prev_tag = sh(["git", "describe", "--tags", "--abbrev=0"])
    print(f"  previous tag: {prev_tag}")

    new_tag = args.version or bump_patch(prev_tag)
    if not new_tag.startswith("v"):
        new_tag = "v" + new_tag
    print(f"  target tag:   {new_tag}")

    if sh(["git", "tag", "--list", new_tag]):
        sys.exit(f"  ERROR: tag {new_tag} already exists locally")

    remote_tag = sh(["git", "ls-remote", "--tags", "origin", new_tag])
    if remote_tag:
        sys.exit(f"  ERROR: tag {new_tag} already exists on origin")

    zip_name = f"clew-{new_tag}-windows-x64.zip"
    staging = Path(tempfile.gettempdir()) / f"clew-{new_tag}-windows-x64"
    zip_path = Path(tempfile.gettempdir()) / zip_name

    notes = args.notes or build_notes(prev_tag, new_tag)
    print("\n  release notes preview:")
    for line in notes.splitlines():
        print(f"    {line}")

    # --- Tag + push ----------------------------------------------------------
    print(f"\n== Tag {new_tag} ==")
    run_step("tag", ["git", "tag", "-a", new_tag, "-m", f"Clew {new_tag}"], dry=dry)

    print("\n== Push ==")
    run_step("push commits", ["git", "push", "origin", "HEAD"], dry=dry)
    run_step("push tag", ["git", "push", "origin", new_tag], dry=dry)

    # --- Build ---------------------------------------------------------------
    if args.skip_build:
        print("\n== Build ==\n  skipped (--skip-build)")
    else:
        print("\n== Configure CMake (refreshes git-describe version) ==")
        run_step("cmake configure", ["cmake", "--preset", "windows-vcpkg"], dry=dry)

        print("\n== Build backend (Release) ==")
        run_step("cmake build", ["cmake", "--build", "build", "--config", "Release"], dry=dry)

        print("\n== Build frontend ==")
        run_step("npm install", ["npm", "install"], cwd=ROOT / "frontend", dry=dry)
        run_step("npm run build", ["npm", "run", "build"], cwd=ROOT / "frontend", dry=dry)

    # --- Package -------------------------------------------------------------
    print("\n== Package zip ==")
    print(f"  staging: {staging}")
    print(f"  output:  {zip_path}")
    if dry:
        print("  [dry] skipped")
    else:
        package_zip(new_tag, zip_path, staging)

    # --- GitHub release ------------------------------------------------------
    print("\n== Create GitHub release ==")
    cmd = [
        "gh", "release", "create", new_tag,
        str(zip_path),
        "--repo", GH_REPO,
        "--title", f"Clew {new_tag}",
        "--notes", notes,
    ]
    run_step("gh release", cmd, dry=dry)

    if dry:
        print("\n(dry run -- nothing was actually changed)")
    else:
        print(f"\n[OK] Released {new_tag}: https://github.com/{GH_REPO}/releases/tag/{new_tag}")


if __name__ == "__main__":
    main()
