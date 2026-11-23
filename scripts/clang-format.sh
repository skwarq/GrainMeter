#!/bin/bash
# Formats all own C/C++ sources with the repository .clang-format (Google).
#   scripts/clang-format.sh            format in place
#   scripts/clang-format.sh --dry-run  check only (non-zero exit on diff), CI
# Uses $CLANG_FORMAT or clang-format-18 / clang-format from PATH.
set -euo pipefail
cd "$(dirname "$0")/.."

CF="${CLANG_FORMAT:-}"
if [ -z "$CF" ]; then
    if command -v clang-format-18 >/dev/null; then CF=clang-format-18; else CF=clang-format; fi
fi
VERSION="$("$CF" --version | sed -n 's/.*version \([0-9]*\).*/\1/p')"
if [ "$VERSION" != "18" ]; then
    echo "warning: $CF is version $VERSION, CI uses 18 (results may differ)" >&2
fi

mapfile -t FILES < <(git ls-files --cached --others --exclude-standard -- \
    'core/*.h' 'core/*.cpp' 'pc/*.h' 'pc/*.cpp' 'tests/*.h' 'tests/*.cpp' \
    'firmware/main/*.h' 'firmware/main/*.cpp' 2>/dev/null || true)
if [ ${#FILES[@]} -eq 0 ]; then
    mapfile -t FILES < <(find core pc tests firmware/main -type f \( -name '*.h' -o -name '*.cpp' \) | sort)
fi

if [ "${1:-}" = "--dry-run" ]; then
    "$CF" --dry-run -Werror "${FILES[@]}"
    echo "clang-format: ${#FILES[@]} files OK"
elif [ $# -eq 0 ]; then
    "$CF" -i "${FILES[@]}"
    echo "clang-format: ${#FILES[@]} files formatted"
else
    echo "usage: $0 [--dry-run]" >&2
    exit 2
fi
