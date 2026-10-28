#!/usr/bin/env bash
# Refresh third_party/npp/plugin.h from a Nextpad++ Linux checkout, or from
# the commit recorded in third_party/npp/SOURCE.txt when no path is given.
# scintilla_min.h is maintained by hand; this script prints the host's
# sci_messages.h path so the constants can be compared.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
dest="$root/third_party/npp"
commit=8dec39c9f0f53aefc662520717fb7c0b6107fcdb
url=https://github.com/nextpad-plus-plus/nextpad-plus-plus-linux

if [[ $# -ge 1 ]]; then
    src=$1
    [[ -f "$src/src/plugin.h" ]] || { echo "no src/plugin.h under $src" >&2; exit 1; }
    cp "$src/src/plugin.h" "$dest/plugin.h"
    echo "Copied plugin.h from $src"
    if [[ -f "$src/src/sci_messages.h" ]]; then
        echo "Compare scintilla constants with $src/src/sci_messages.h"
    fi
    exit 0
fi

tmp=$(mktemp)
curl -fLsS "$url/raw/$commit/src/plugin.h" -o "$tmp"
mv "$tmp" "$dest/plugin.h"
echo "Downloaded plugin.h at $commit"
echo "sci_messages.h: $url/raw/$commit/src/sci_messages.h"
