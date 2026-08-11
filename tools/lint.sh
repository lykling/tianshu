#!/usr/bin/env bash
# =============================================================================
# TIANSHU lint helper
# =============================================================================
#
# Per ADR-0004, this is an AUXILIARY tool, not a build entry point.
# Runs clang-tidy, cppcheck, and license header check.
#
# Usage:
#   tools/lint.sh                # lint all
#   tools/lint.sh --fix          # auto-fix where possible

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${REPO_ROOT}"

FIX=0
if [[ "${1:-}" == "--fix" ]]; then
    FIX=1
fi

FAILED=0

# -----------------------------------------------------------------------------
# License header check (per ADR-0017, every C++ file must have Apache-2.0 header)
# -----------------------------------------------------------------------------
echo "[lint] checking license headers..."
FILES=$(find tianshu tests examples \
    -type f \( -name '*.cc' -o -name '*.cpp' -o -name '*.h' -o -name '*.hpp' \) 2>/dev/null || true)

for f in ${FILES}; do
    if ! head -3 "${f}" | grep -q "Copyright 2026 The TIANSHU Team"; then
        echo "[lint] FAIL: missing license header: ${f}"
        FAILED=1
    fi
done

# -----------------------------------------------------------------------------
# Non-English comment check (per ADR-0009, comments must be English)
# -----------------------------------------------------------------------------
echo "[lint] checking for non-English comments..."
# Detect CJK characters in comments (rough heuristic: lines starting with // or /* containing CJK)
for f in ${FILES}; do
    # Pattern matches CJK Unicode ranges (rough)
    if grep -nP '^(\s*//|\s*\*).*[\x{4e00}-\x{9fff}\x{3040}-\x{30ff}\x{ac00}-\x{d7af}]' "${f}" >/dev/null 2>&1; then
        echo "[lint] FAIL: non-English comment detected in: ${f}"
        grep -nP '^(\s*//|\s*\*).*[\x{4e00}-\x{9fff}\x{3040}-\x{30ff}\x{ac00}-\x{d7af}]' "${f}" | head -3
        FAILED=1
    fi
done

# -----------------------------------------------------------------------------
# Build entry guard (per ADR-0004, no wrap scripts)
# -----------------------------------------------------------------------------
echo "[lint] checking for forbidden wrap scripts..."
if find . -path ./build -prune -o -path ./.bazel -prune \
    -type f \( -name 'build.sh' -o -name 'build_opt_*' -o -name 'tianshu-build' -o -name 'make.sh' \) \
    -print 2>/dev/null | grep -q .; then
    echo "[lint] FAIL: found forbidden build wrap script(s) (see ADR-0004)"
    find . -path ./build -prune -o -path ./.bazel -prune \
        -type f \( -name 'build.sh' -o -name 'build_opt_*' -o -name 'tianshu-build' -o -name 'make.sh' \) \
        -print
    FAILED=1
fi

# -----------------------------------------------------------------------------
# cppcheck (optional, if installed)
# -----------------------------------------------------------------------------
if command -v cppcheck >/dev/null 2>&1; then
    echo "[lint] running cppcheck..."
    if ! cppcheck --enable=warning,style --suppress=missingIncludeSystem \
        --error-exitcode=1 \
        tianshu tests examples 2>&1; then
        echo "[lint] FAIL: cppcheck reported issues"
        FAILED=1
    fi
else
    echo "[lint] cppcheck not installed, skipping"
fi

# -----------------------------------------------------------------------------
# Summary
# -----------------------------------------------------------------------------
if [[ ${FAILED} -ne 0 ]]; then
    echo "[lint] FAILED"
    exit 1
fi
echo "[lint] PASS"
