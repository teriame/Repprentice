#!/usr/bin/env bash
# Builds the single self-contained Linux & SteamOS (Steam Deck) AppImage:
#
#   dist/Apprentice-<version>-x86_64.AppImage
#   dist/SHA256SUMS.txt
#
# Neither the disc image nor any derived 68000 game code is included in the
# release AppImage.  The single binary is built with `tools/gen.py --stub` and
# embeds the recompiler/CHD tools inside itself so 68000 -> C derivation and
# .chd conversion happen locally in CWD (./saves/) or /tmp.
#
# Usage: ./package.sh [--no-build]
set -euo pipefail
cd "$(dirname "$0")"

NO_BUILD=0
for arg in "$@"; do
    case "$arg" in
        --no-build|-NoBuild) NO_BUILD=1 ;;
    esac
done

VER=$(head -n 1 VERSION | tr -d '[:space:]')
NAME="Apprentice-${VER}"
DIST="dist"
APPDIR="${DIST}/AppDir"
BIN_PATH="build/cmake/apprentice"

if [ "$NO_BUILD" -eq 0 ]; then
    echo "Building zero-derived-code release ${VER} (--stub)..."
    ./build.sh --stub --compile-only
fi

if [ ! -x "$BIN_PATH" ]; then
    echo "Error: $BIN_PATH is missing; run ./build.sh --stub first" >&2
    exit 1
fi

rm -rf "$APPDIR" "${DIST}/${NAME}-linux-x86_64" "${DIST}/${NAME}-linux-x86_64-portable.tar.gz"
rm -f apprentice apprentice_hd

mkdir -p "$DIST" \
         "$APPDIR/usr/bin" \
         "$APPDIR/usr/share/apprentice/tools" \
         "$APPDIR/usr/share/apprentice/runtime" \
         "$APPDIR/usr/share/applications" \
         "$APPDIR/usr/share/icons/hicolor/scalable/apps"

cp -f "$BIN_PATH" "$APPDIR/usr/bin/apprentice"
cp -f tools/chd.py tools/cdi_extract.py tools/m68k.py tools/os9mod.py tools/analyze.py tools/config.py tools/gen.py tools/steam_register.py "$APPDIR/usr/share/apprentice/tools/"
cp -f runtime/recomp.h "$APPDIR/usr/share/apprentice/runtime/"
cp -f README.md VERSION game.json "$APPDIR/usr/share/apprentice/"

cat > "$APPDIR/AppRun" << 'EOF'
#!/usr/bin/env bash
HERE="$(dirname "$(readlink -f "$0")")"
export APPDIR="$HERE"
exec "$HERE/usr/bin/apprentice" "$@"
EOF
chmod +x "$APPDIR/AppRun"

cat > "$APPDIR/apprentice.desktop" << 'EOF'
[Desktop Entry]
Type=Application
Name=The Apprentice
GenericName=CD-i Game
Comment=The Apprentice (1994, Philips CD-i) static recompilation runtime with GPU Cartoon HD upscaling
Exec=AppRun
Icon=apprentice
Terminal=false
Categories=Game;ActionGame;ArcadeGame;
Keywords=CD-i;Apprentice;Platformer;SteamDeck;
StartupWMClass=apprentice
EOF
cp -f "$APPDIR/apprentice.desktop" "$APPDIR/usr/share/applications/apprentice.desktop"

cat > "$APPDIR/apprentice.svg" << 'EOF'
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 256 256" width="256" height="256">
  <defs>
    <linearGradient id="bg" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0%" stop-color="#1c2d4f"/>
      <stop offset="100%" stop-color="#0d1424"/>
    </linearGradient>
    <linearGradient id="hat" x1="0" y1="0" x2="1" y2="1">
      <stop offset="0%" stop-color="#4a78d0"/>
      <stop offset="100%" stop-color="#224088"/>
    </linearGradient>
  </defs>
  <rect width="256" height="256" rx="40" fill="url(#bg)" stroke="#ffe070" stroke-width="6"/>
  <path d="M128 32 L188 168 L68 168 Z" fill="url(#hat)" stroke="#ffe070" stroke-width="5" stroke-linejoin="round"/>
  <ellipse cx="128" cy="170" rx="84" ry="20" fill="url(#hat)" stroke="#ffe070" stroke-width="5"/>
  <polygon points="128,75 133,88 147,88 136,96 140,109 128,101 116,109 120,96 109,88 123,88" fill="#ffe070"/>
  <circle cx="105" cy="135" r="5" fill="#ffe070"/>
  <circle cx="152" cy="130" r="6" fill="#ffe070"/>
  <rect x="32" y="198" width="192" height="34" rx="8" fill="#141e30" stroke="#ffe070" stroke-width="3"/>
  <text x="128" y="221" font-family="sans-serif" font-size="20" font-weight="bold" fill="#ffe070" text-anchor="middle">APPRENTICE</text>
</svg>
EOF
cp -f "$APPDIR/apprentice.svg" "$APPDIR/usr/share/icons/hicolor/scalable/apps/apprentice.svg"
ln -sf apprentice.svg "$APPDIR/.DirIcon"

# Compile Type-2 AppImage runtime stub
RUNTIME_BIN="${DIST}/appimage_runtime"
OFFSET=65536
gcc -O2 -DAPPIMAGE_OFFSET=${OFFSET} tools/appimage_runtime.c -o "$RUNTIME_BIN"
strip -s "$RUNTIME_BIN" 2>/dev/null || true

RT_SIZE=$(stat -c%s "$RUNTIME_BIN")
if [ "$RT_SIZE" -gt "$OFFSET" ]; then
    echo "Error: AppImage runtime ($RT_SIZE bytes) exceeds offset $OFFSET" >&2
    exit 1
fi

# Stamp Type-2 AppImage magic bytes "AI\x02" at ELF header e_ident[8..10]
python3 -c "
with open('${RUNTIME_BIN}', 'r+b') as f:
    f.seek(8)
    f.write(b'AI\x02')
    f.seek(0, 2)
    pad = ${OFFSET} - f.tell()
    if pad > 0:
        f.write(b'\x00' * pad)
"

SQUASHFS_IMG="${DIST}/appdir.squashfs"
rm -f "$SQUASHFS_IMG"
mksquashfs "$APPDIR" "$SQUASHFS_IMG" -root-owned -noappend >/dev/null

APPIMAGE="${DIST}/${NAME}-x86_64.AppImage"
cat "$RUNTIME_BIN" "$SQUASHFS_IMG" > "$APPIMAGE"
chmod +x "$APPIMAGE"
rm -f "$RUNTIME_BIN" "$SQUASHFS_IMG"
rm -rf "$APPDIR" gen extracted
echo "  ${APPIMAGE}"

(
    cd "$DIST"
    sha256sum "${NAME}-x86_64.AppImage" > SHA256SUMS.txt
)
echo "Done: ${APPIMAGE} (single self-contained AppImage, zero derived game code)"
