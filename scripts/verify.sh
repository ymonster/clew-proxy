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
# Exit 0 on full green. Any step failure exits non-zero with a clear
# "FAIL: ..." prefix. Safe to re-run; idempotent.
#
# Requires administrator shell (clew.exe needs elevation) and `uv` on PATH
# (used by the Playwright suite via PEP 723 inline-deps).

set -euo pipefail

cd "$(dirname "$0")/.."

fail() {
    local msg=$1
    echo "FAIL: $msg" >&2
    exit 1
}

# ---------------------------------------------------------------------------
# 1/5 frontend build
# ---------------------------------------------------------------------------
echo "==> 1/5 frontend build"
if [[ ! -d frontend ]]; then
    fail "frontend/ directory missing"
fi
(cd frontend && npm run build)

# ---------------------------------------------------------------------------
# 2/5 cpp build
# ---------------------------------------------------------------------------
echo
echo "==> 2/5 cpp build"
if [[ ! -d build ]]; then
    fail "build/ directory missing. Run 'cmake --preset windows-vcpkg' first."
fi
cmake --build build --config Release --target clew

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

grep_must_be_empty 'asio\.hpp\|asio::'     'src/services/'          'services must not touch asio (H1 layering)'
grep_must_be_empty 'httplib\.h\|httplib::' 'src/services/'          'services must not touch httplib (H1 layering)'
grep_must_be_empty 'sse_hub'               'src/services/'          'services must not reference sse_hub (H5)'
grep_must_be_empty 'strand_bound'          'src/transport/handlers/' 'handlers must not use strand_bound (H1 layering)'

# config_manager.hpp must only be included from config_store.hpp across the
# new-architecture directories.
leaks=$(grep -rln 'config_manager\.hpp' \
    src/services src/projection src/transport src/common src/domain \
    2>/dev/null | grep -v '/config_store\.hpp$' || true)
if [[ -n "$leaks" ]]; then
    echo "$leaks" >&2
    fail 'config_manager.hpp leaked into new-arch code (only config_store.hpp may include it)'
fi

# service .cpp files must not include other services/ headers.
for cpp in src/services/*.cpp; do
    self="$(basename "$cpp" .cpp)"
    bad=$(grep -E '#include "services/' "$cpp" | grep -v "services/${self}\\.hpp" || true)
    if [[ -n "$bad" ]]; then
        echo "$cpp:" >&2
        echo "$bad" >&2
        fail 'service-to-service include detected (services must not depend on each other)'
    fi
done

echo 'layering OK'

# ---------------------------------------------------------------------------
# 4/5 HTTP e2e
# ---------------------------------------------------------------------------
echo
echo "==> 4/5 HTTP e2e (tests/run_all.py)"
python tests/run_all.py

# ---------------------------------------------------------------------------
# 5/5 Playwright e2e
# ---------------------------------------------------------------------------
echo
echo "==> 5/5 Playwright e2e (tests/playwright_e2e/run_pw.py)"
if ! command -v uv >/dev/null 2>&1; then
    fail "uv not found on PATH (required for PEP 723 inline-deps script)"
fi
uv run --script tests/playwright_e2e/run_pw.py

echo
echo 'ALL GREEN'
