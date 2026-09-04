#!/usr/bin/env bash
# =============================================================================
# TIANSHU code format helper
# =============================================================================
#
# Per ADR-0004, this is an AUXILIARY tool, not a build entry point.
# Run clang-format on staged C/C++ files.
#
# Usage:
#   tools/format.sh            # format all files
#   tools/format.sh --check    # check only, no modification (CI mode)

set -euo pipefail

# Resolve repo root
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${REPO_ROOT}"

CHECK_ONLY=0
if [[ "${1:-}" == "--check" ]]; then
	CHECK_ONLY=1
fi

# Files to format
FILES=$(find tianshu tests examples \
	-type f \( -name '*.cc' -o -name '*.cpp' -o -name '*.h' -o -name '*.hpp' \) 2>/dev/null || true)

if [[ -z "${FILES}" ]]; then
	echo "[format] no source files found"
	exit 0
fi

if [[ ${CHECK_ONLY} -eq 1 ]]; then
	echo "[format] checking (no modification)"
	if ! clang-format --version >/dev/null 2>&1; then
		echo "[format] clang-format not installed; skipping"
		exit 0
	fi
	FAILED=0
	while IFS= read -r f; do
		if ! diff -q <(clang-format "${f}") "${f}" >/dev/null 2>&1; then
			echo "[format] needs formatting: ${f}"
			FAILED=1
		fi
	done <<<"${FILES}"
	if [[ ${FAILED} -ne 0 ]]; then
		echo "[format] FAIL: files above need formatting; run 'tools/format.sh' to fix"
		exit 1
	fi
	echo "[format] PASS"
else
	echo "[format] formatting in-place"
	if ! clang-format --version >/dev/null 2>&1; then
		echo "[format] clang-format not installed; aborting"
		exit 1
	fi
	echo "${FILES}" | xargs clang-format -i --verbose
	echo "[format] done"
fi
