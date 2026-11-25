#!/usr/bin/env bash
# Build The Apprentice CD-i static recompilation on Linux / SteamOS as a single
# self-contained AppImage in dist/Apprentice-<version>-x86_64.AppImage.
#
# Everything written during the build stays strictly inside CWD (build/, gen/,
# dist/, saves/) or /tmp.  No loose apprentice / apprentice_hd binaries are
# left in the root directory.
#
# By default, if --stub is passed (used by package.sh for distributable
# releases) or no disc image is in rom/, it builds with zero derived game code
# (--stub), and the runtime derives C code on the user's machine into
# ./saves/derived/ on first launch.
set -euo pipefail
cd "$(dirname "$0")"

STUB_MODE=0
COMPILE_ONLY=0
for arg in "$@"; do
    case "$arg" in
        --stub) STUB_MODE=1 ;;
        --compile-only) COMPILE_ONLY=1 ;;
    esac
done

EXE=$(python3 -c "import json;print(json.load(open('game.json')).get('exe_name','cdigame'))")
if [ -z "$EXE" ]; then
    echo "Error: could not read exe_name from game.json" >&2
    exit 1
fi

if [ "$STUB_MODE" -eq 1 ]; then
    echo "[1/3] Generating zero-derived-code stub tables (code will be derived at the user)..."
    python3 tools/gen.py --stub
else
    # Convert a .chd in rom/ into ./saves/ (inside CWD) if no .bin exists yet
    if ! compgen -G "rom/*.bin" >/dev/null && ! compgen -G "saves/*.bin" >/dev/null && compgen -G "rom/*.chd" >/dev/null; then
        for chd in rom/*.chd; do
            mkdir -p saves
            base=$(basename "${chd%.*}")
            if [ ! -f "saves/${base}.bin" ]; then
                echo "Converting $chd to saves/${base}.bin/.cue..."
                python3 tools/chd.py "$chd" "saves/${base}"
            fi
            break
        done
    fi

    if compgen -G "rom/*.bin" >/dev/null || compgen -G "saves/*.bin" >/dev/null; then
        echo "[1/3] Deriving 68000 code to C from your disc image..."
        python3 tools/gen.py
    else
        echo "[1/3] No disc image in rom/ yet; building with zero-derived-code stub (derives at first run)..."
        python3 tools/gen.py --stub
    fi
fi

echo "[2/3] Configuring..."
python3 tools/embed_tools.py build/cmake/embedded_tools.h
cmake -S . -B build/cmake -DCMAKE_BUILD_TYPE=Release >/dev/null

echo "[3/3] Compiling single unified binary..."
cmake --build build/cmake --config Release --target game --parallel

# Remove any legacy loose binaries in root
rm -f "${EXE}" "${EXE}_hd"

if [ "$COMPILE_ONLY" -eq 0 ]; then
    ./package.sh --no-build
fi
