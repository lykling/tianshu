#!/usr/bin/env bash
# =============================================================================
# TIANSHU clang-tidy + clang-format check
# =============================================================================
#
# Per ADR-0018 (C++ style guide), all C/C++ source must pass clang-tidy.
# Per ADR-0004, this is an AUXILIARY tool, not a build entry point.
#
# Usage:
#   tools/tidy.sh                # check all source files
#   tools/tidy.sh --fix          # auto-fix where possible
#   tools/tidy.sh --format-check # also check clang-format compliance

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${REPO_ROOT}"

FIX=0
CHECK_FORMAT=0
for arg in "$@"; do
    case "${arg}" in
        --fix) FIX=1 ;;
        --format-check) CHECK_FORMAT=1 ;;
        *) echo "Unknown arg: ${arg}"; exit 2 ;;
    esac
done

# Required tools
if ! command -v clang-tidy >/dev/null 2>&1; then
    echo "[tidy] clang-tidy not installed"
    exit 1
fi

# Find compile_commands.json
CC_JSON=""
for path in compile_commands.json build/desktop/compile_commands.json; do
    if [[ -f "${path}" ]]; then
        CC_JSON="$(pwd)/${path}"
        break
    fi
done
if [[ -z "${CC_JSON}" ]]; then
    echo "[tidy] ERROR: compile_commands.json not found."
    echo "[tidy] Generate via one of:"
    echo "[tidy]   cmake --preset=desktop               (CMake, auto-generates)"
    echo "[tidy]   bazel run @hedron_compile_commands_extractor//:refresh_all  (Bazel)"
    exit 1
fi
echo "[tidy] using compile database: ${CC_JSON}"

# Source files to check (only TIANSHU source, not examples tests phase 1+ separately)
SOURCE_FILES=$(find tianshu tests examples \
    -type f \( -name '*.cc' -o -name '*.cpp' -o -name '*.h' -o -name '*.hpp' \) \
    2>/dev/null || true)
if [[ -z "${SOURCE_FILES}" ]]; then
    echo "[tidy] no source files found"
    exit 0
fi

# -----------------------------------------------------------------------------
# clang-format check (run before clang-tidy)
# -----------------------------------------------------------------------------
if [[ ${CHECK_FORMAT} -eq 1 ]]; then
    echo "[tidy] checking clang-format..."
    if ! command -v clang-format >/dev/null 2>&1; then
        echo "[tidy] clang-format not installed; skipping format check"
    else
        FAILED=0
        while IFS= read -r f; do
            if ! diff -q <(clang-format "${f}") "${f}" >/dev/null 2>&1; then
                echo "[tidy] format FAIL: ${f}"
                FAILED=1
            fi
        done <<< "${SOURCE_FILES}"
        if [[ ${FAILED} -ne 0 ]]; then
            echo "[tidy] FAIL: files above need formatting; run 'tools/format.sh' to fix"
            exit 1
        fi
        echo "[tidy] format OK"
    fi
fi

# -----------------------------------------------------------------------------
# clang-tidy
# -----------------------------------------------------------------------------
TIDY_ARGS=(-p "${CC_JSON}")
if [[ ${FIX} -eq 1 ]]; then
    TIDY_ARGS+=(--fix --fix-errors)
else
    # Treat warnings as errors (per ADR-0018 zero-warning policy)
    TIDY_ARGS+=(--warnings-as-errors=*)
fi

echo "[tidy] running clang-tidy (${#TIDY_ARGS[@]} args)..."
FAILED=0
while IFS= read -r f; do
    if ! clang-tidy "${TIDY_ARGS[@]}" "${f}" >/tmp/tidy.log 2>&1; then
        echo "[tidy] FAIL: ${f}"
        cat /tmp/tidy.log
        FAILED=1
    else
        # Even with exit 0, warnings may have been printed
        if grep -E "^/.*: warning:" /tmp/tidy.log >/dev/null 2>&1; then
            echo "[tidy] FAIL: warnings in ${f}"
            cat /tmp/tidy.log
            FAILED=1
        fi
    fi
done <<< "${SOURCE_FILES}"

if [[ ${FAILED} -ne 0 ]]; then
    echo "[tidy] FAILED"
    exit 1
fi
echo "[tidy] PASS: all clean"
