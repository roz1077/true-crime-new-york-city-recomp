#!/bin/bash
# Build the native Linux/SteamOS test package from a native build directory.
#   scripts/package-linux.sh [build-dir] [output-dir]
set -euo pipefail
REPO=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${1:-/root/tcnyc-linux-build}
OUT=${2:-$REPO/dist}
NAME=TrueCrimeNYC-linux-x86_64
STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT

PKG="$STAGE/$NAME"
mkdir -p "$PKG/lib" "$PKG/assets" "$PKG/LICENSES"

# the release binary: no debug tables, the rpath already points at $ORIGIN/lib
install -m 0755 "$BUILD/TrueCrimeNYC" "$PKG/TrueCrimeNYC"
strip --strip-unneeded "$PKG/TrueCrimeNYC"

SHADERC=$(ldconfig -p | awk '/libshaderc\.so\.1 /{print $NF; exit}')
if [ -z "$SHADERC" ]; then
    echo "libshaderc.so.1 was not found; install libshaderc1" >&2
    exit 1
fi
cp -L "$SHADERC" "$PKG/lib/libshaderc.so.1"
# the shaderc package's own dependencies must exist on the target
if ldd "$PKG/lib/libshaderc.so.1" 2>/dev/null | grep -q 'not found'; then
    echo "libshaderc.so.1 has unresolved dependencies:" >&2
    ldd "$PKG/lib/libshaderc.so.1" | grep 'not found' >&2
    exit 1
fi

cp "$REPO/README-LINUX.md" "$PKG/README-LINUX.md"
cp "$REPO/README.md" "$PKG/README.md"
cp "$REPO/sdk/THIRD_PARTY.txt" "$PKG/THIRD_PARTY.txt"
for file in /usr/share/doc/libshaderc1/copyright /usr/share/doc/glslang-dev/copyright; do
    [ -f "$file" ] && cp "$file" "$PKG/LICENSES/$(basename "$(dirname "$file")")-copyright.txt"
done
[ -f "$BUILD/_deps/imgui-src/LICENSE.txt" ] && cp "$BUILD/_deps/imgui-src/LICENSE.txt" "$PKG/LICENSES/imgui-LICENSE.txt"

cat > "$PKG/assets/PUT-THE-GAME-HERE.txt" <<'EOF'
Put the game in this folder, one of:

  - the extracted files: default.xbe with the game's folders beside it, or
  - a disc image: any .iso file.

The extracted files are used when both are present. No game assets are included
in this package.
EOF

{
    echo "True Crime: New York City - native Linux build"
    echo "Built: $(date -u +'%Y-%m-%dT%H:%M:%SZ')"
    echo "Git: $(git -C "$REPO" rev-parse --short HEAD 2>/dev/null || echo unknown)"
    echo "GLIBC: $(objdump -T "$PKG/TrueCrimeNYC" | grep -oE 'GLIBC_[0-9.]+' | sort -V | tail -1)"
} > "$PKG/BUILD.txt"

chmod 0755 "$PKG/TrueCrimeNYC"
chmod 0644 "$PKG/README.md" "$PKG/README-LINUX.md" "$PKG/THIRD_PARTY.txt" "$PKG/BUILD.txt" \
    "$PKG/assets/PUT-THE-GAME-HERE.txt" "$PKG/lib/libshaderc.so.1"
for licence in "$PKG"/LICENSES/*; do
    [ -e "$licence" ] && chmod 0644 "$licence"
done
chmod 0755 "$PKG" "$PKG/lib" "$PKG/assets" "$PKG/LICENSES"

mkdir -p "$OUT"
tar -C "$STAGE" -czf "$OUT/$NAME.tar.gz" "$NAME"
(cd "$OUT" && sha256sum "$NAME.tar.gz" > "$NAME.tar.gz.sha256")
ls -l "$OUT/$NAME.tar.gz" "$OUT/$NAME.tar.gz.sha256"
