#!/usr/bin/env bash
# Reject commit messages containing CJK characters (ADR-0009: commit
# messages must be English). Installed as a commit-msg hook via
# `pre-commit install --hook-type commit-msg`; CI re-checks the whole
# history in the commitlint job.

set -euo pipefail

msg_file="$1"
if grep -qP '[\x{4e00}-\x{9fff}\x{3400}-\x{4dbf}]' "$msg_file"; then
	echo "commit message contains CJK characters (ADR-0009 requires English):" >&2
	grep -nP '[\x{4e00}-\x{9fff}\x{3400}-\x{4dbf}]' "$msg_file" | head -3 >&2
	exit 1
fi
exit 0
