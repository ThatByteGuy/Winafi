#!/usr/bin/env bash
# Build a portable, relocatable Winafi tarball (Feature 6: "Portable binary release").
# Produces dist/winafi-<version>-portable-<arch>.tar.gz containing the CLI + GUI
# binaries, the signed UEFI:NTFS asset, error strings, a desktop file, and a
# launcher that sets WINAFI_DATADIR so assets resolve without a system install.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

VERSION="${1:-$(grep -oP '^\s*VERSION\s+\K[0-9.]+' CMakeLists.txt | head -1 || echo 0.0.0)}"
ARCH="$(uname -m)"
STAGE="dist/winafi-${VERSION}-portable-${ARCH}"

echo "Building Winafi portable ${VERSION} (${ARCH})"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_GUI=ON -DBUILD_TESTS=OFF \
    -DWINAFI_BUILD_VERSION="${VERSION}" >/dev/null
cmake --build build -j"$(nproc)"

rm -rf "$STAGE"
mkdir -p "$STAGE/bin" "$STAGE/share/winafi/assets/uefi-ntfs"

# Binaries
cp "build/src/winafi"     "$STAGE/bin/" 2>/dev/null || true
cp "build/src/winafi-gui" "$STAGE/bin/" 2>/dev/null || true

# Runtime data
cp src/assets/uefi-ntfs/bootx64.efi "$STAGE/share/winafi/assets/uefi-ntfs/"
cp src/errors.txt "$STAGE/share/winafi/" 2>/dev/null || true
cp packaging/winafi.desktop "$STAGE/share/winafi/" 2>/dev/null || true

# Launcher: point WINAFI_DATADIR at the bundled assets so the loader resolves.
cat > "$STAGE/winafi" <<'EOF'
#!/usr/bin/env bash
here="$(cd "$(dirname "$0")" && pwd)"
export WINAFI_DATADIR="$here/share/winafi/assets"
exec "$here/bin/winafi-gui" "$@"
EOF
chmod +x "$STAGE/winafi"

cat > "$STAGE/README.txt" <<EOF
Winafi ${VERSION} (portable, ${ARCH})

Run ./winafi to start the GUI, or ./bin/winafi <args> for the CLI.
This build is dynamically linked. Install the runtime libraries first:
  Qt5 Widgets, libudev, libparted, libarchive, libcdio, libmount,
  libblkid, OpenSSL, libcurl.
Writing to a USB device requires root (run with sudo).
EOF

# Deterministic tarball.
mkdir -p dist
tar --sort=name --owner=0 --group=0 --numeric-owner \
    --mtime="@${SOURCE_DATE_EPOCH:-0}" \
    -czf "dist/winafi-${VERSION}-portable-${ARCH}.tar.gz" -C dist "$(basename "$STAGE")"

echo "Wrote dist/winafi-${VERSION}-portable-${ARCH}.tar.gz"
