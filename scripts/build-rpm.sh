#!/bin/bash
set -e

VERSION="${1:-0.0.5}"
VERSION=$(echo "$VERSION" | sed 's/^v//' | sed 's/-/_/g')
BUILD_DIR="build"
INSTALL_DIR="${BUILD_DIR}/rpm-install"

echo "Building Winafi RPM package v${VERSION}"

# Build application
mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}"
cmake .. -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_TESTS=OFF -DWINAFI_BUILD_VERSION="${VERSION}"
make -j$(nproc)

# Install to staging directory
rm -rf "${INSTALL_DIR}"
make install DESTDIR="${INSTALL_DIR}"
cd ..

# Create desktop entry in staging
mkdir -p "${INSTALL_DIR}/usr/share/applications"
cat > "${INSTALL_DIR}/usr/share/applications/winafi.desktop" << 'EOF'
[Desktop Entry]
Type=Application
Name=Winafi
Comment=USB bootable drive utility for Linux
Exec=winafi-gui
Icon=application-x-executable
Categories=Utility;System;
Terminal=false
EOF

# Use FPM to build RPM package
fpm -s dir \
  -t rpm \
  -n winafi \
  -v "${VERSION}" \
  -C "${INSTALL_DIR}" \
  --maintainer "AlphaGlider25 <12aa44edsta@gmail.com>" \
  --url "https://github.com/AlphaGlider25/Winafi" \
  --description "USB bootable drive utility for Linux" \
  --license "GPL-3.0" \
  --depends "qt5-qtbase" \
  --depends "libudev" \
  --depends "libparted" \
  --depends "libarchive" \
  --depends "libcdio" \
  --depends "openssl-libs" \
  --depends "libcurl" \
  -p "winafi-${VERSION}-1.x86_64.rpm"

ls -lh "winafi-${VERSION}-1.x86_64.rpm"
