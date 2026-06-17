#!/bin/bash
set -e

VERSION="${1:-0.0.5}"
VERSION=$(echo "$VERSION" | sed 's/^v//' | sed 's/-/~/g')
BUILD_DIR="build"
INSTALL_DIR="${BUILD_DIR}/deb-install"

echo "Building Winafi Debian package v${VERSION}"

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

# Use FPM to build deb package
fpm -s dir \
  -t deb \
  -n winafi \
  -v "${VERSION}" \
  -C "${INSTALL_DIR}" \
  --maintainer "AlphaGlider25 <12aa44edsta@gmail.com>" \
  --url "https://github.com/AlphaGlider25/Winafi" \
  --description "USB bootable drive utility for Linux" \
  --license "GPL-3.0" \
  --depends "libqt5widgets5" \
  --depends "libudev1" \
  --depends "libparted2" \
  --depends "libarchive13" \
  --depends "libcdio19" \
  --depends "libssl3" \
  --depends "libcurl4" \
  -p "winafi_${VERSION}_amd64.deb"

ls -lh "winafi_${VERSION}_amd64.deb"
