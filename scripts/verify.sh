#!/usr/bin/env bash
# Clew refactor verification script.
#
# One shot:
#   1. frontend build (Vite — picks up notify.ts and Vue changes)
#   2. cmake build (incremental — assumes build/ is already configured)
#   3. seven layering grep guards that assert the three-layer boundaries
#   4. run tests/run_all.py (launches clew.exe, runs HTTP e2e, teardown)
#   5. run tests/playwright_e2e/run_pw.py (launches clew.exe with CDP,
#      runs Playwright + WebView2 e2e, teardown)
#
# Each stage is wrapped with a per-stage timeout via GNU `timeout` so a
# single hung stage (infinite loop, deadlock, frozen webview) aborts the
# pipeline with a clear message instead of silently hanging the dev's
# terminal. Budgets are generous (full rebuild + admin shell prompt all
# fit) but bounded.
#
# Exit 0 on full green. Any step failure exits non-zero with a clear
# "FAIL: ..." prefix. Safe to re-run; idempotent.
#
# Requires administrator shell (clew.exe needs elevation), `uv` on PATH
# (used by the Playwright suite via PEP 723 inline-deps), and GNU
# coreutils `timeout` (ships with Git for Windows).

set -euo pipefail

cd "$(dirname "$0")/.."

fail() {
    local msg=$1
    echo "FAIL: $msg" >&2
    exit 1
}

# Wrap a command with a wall-clock timeout. Exit 124 from `timeout` means
# the budget was exceeded; treat it specially so the user sees which stage
# hung instead of just a generic non-zero rc.
run_with_timeout() {
    local label=$1
    local secs=$2
    shift 2
    set +e
    timeout --preserve-status --kill-after=10 "$secs" "$@"
    local rc=$?
    set -e
    if [[ $rc -eq 124 ]]; then
        fail "$label exceeded ${secs}s timeout"
    elif [[ $rc -ne 0 ]]; then
        fail "$label exited $rc"
    fi
}

if ! command -v timeout >/dev/null 2>&1; then
    fail "GNU timeout not found on PATH (Git for Windows ships it under /usr/bin)"
fi

# ---------------------------------------------------------------------------
# 1/5 frontend build (npm run build)
# ---------------------------------------------------------------------------
echo "==> 1/5 frontend build"
if [[ ! -d frontend ]]; then
    fail "frontend/ directory missing"
fi
run_with_timeout 'frontend build' 240 bash -c 'cd frontend && npm run build'

# ---------------------------------------------------------------------------
# 2/5 cpp build (cmake --build)
# ---------------------------------------------------------------------------
echo
echo "==> 2/5 cpp build"
if [[ ! -d build ]]; then
    fail "build/ directory missing. Run 'cmake --preset windows-vcpkg' first."
fi
run_with_timeout 'cpp build' 600 cmake --build build --config Release --target clew

# ---------------------------------------------------------------------------
# 3/5 layering grep
# ---------------------------------------------------------------------------
echo
echo "==> 3/5 layering grep"

grep_must_be_empty() {
    local pattern=$1
    local search_path=$2
    local message=$3
    local hits
    hits=$(grep -rn "$pattern" "$search_path" 2>/dev/null || true)
    if [[ -n "$hits" ]]; then
        echo "$hits" | head -5 >&2
        fail "$message"
    fi
    return 0
}

run_with_timeout 'layering grep' 30 bash <<'GREPS'
set -euo pipefail
grep_must_be_empty() {
    local pattern=$1
    local search_path=$2
    local message=$3
    local hits
    hits=$(grep -rn "$pattern" "$search_path" 2>/dev/null || true)
    if [[ -n "$hits" ]]; then
        echo "$hits" | head -5 >&2
        echo "FAIL: $message" >&2
        exit 1
    fi
}

grep_must_be_empty 'asio\.hpp\|asio::'     'src/services/'          'services must not touch asio (H1 layering)'
grep_must_be_empty 'httplib\.h\|httplib::' 'src/services/'          'services must not touch httplib (H1 layering)'
grep_must_be_empty 'sse_hub'               'src/services/'          'services must not reference sse_hub (H5)'
grep_must_be_empty 'strand_bound'          'src/transport/handlers/' 'handlers must not use strand_bound (H1 layering)'

leaks=$(grep -rln 'config_manager\.hpp' \
    src/services src/projection src/transport src/common src/domain \
    2>/dev/null | grep -v '/config_store\.hpp$' || true)
if [[ -n "$leaks" ]]; then
    echo "$leaks" >&2
    echo 'FAIL: config_manager.hpp leaked into new-arch code (only config_store.hpp may include it)' >&2
    exit 1
fi

for cpp in src/services/*.cpp; do
    self="$(basename "$cpp" .cpp)"
    bad=$(grep -E '#include "services/' "$cpp" | grep -v "services/${self}\\.hpp" || true)
    if [[ -n "$bad" ]]; then
        echo "$cpp:" >&2
        echo "$bad" >&2
        echo 'FAIL: service-to-service include detected (services must not depend on each other)' >&2
        exit 1
    fi
done

echo 'layering OK'
GREPS

# ---------------------------------------------------------------------------
# 4/5 HTTP e2e
# ---------------------------------------------------------------------------
echo
echo "==> 4/5 HTTP e2e (tests/run_all.py)"
run_with_timeout 'HTTP e2e' 240 python tests/run_all.py

# ---------------------------------------------------------------------------
# 5/5 Playwright e2e
# ---------------------------------------------------------------------------
echo
echo "==> 5/5 Playwright e2e (tests/playwright_e2e/run_pw.py)"
if ! command -v uv >/dev/null 2>&1; then
    fail "uv not found on PATH (required for PEP 723 inline-deps script)"
fi
run_with_timeout 'Playwright e2e' 300 uv run --script tests/playwright_e2e/run_pw.py

echo
echo 'ALL GREEN'
