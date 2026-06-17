#!/bin/bash
set -e

VERSION="${1:-0.0.5}"
VERSION=$(echo "$VERSION" | sed 's/^v//')
ARCH="${2:-x86_64}"
BUILD_DIR="build"
INSTALL_DIR="${BUILD_DIR}/install"
APPDIR="${BUILD_DIR}/Winafi.AppDir"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

echo "Building Winafi AppImage v${VERSION}"

# Clean up previous builds
rm -rf "${BUILD_DIR}"

# Build the application
mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}"
cmake .. -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_BUILD_TYPE=Release -DBUILD_GUI=ON \
    -DBUILD_TESTS=OFF -DWINAFI_BUILD_VERSION="${VERSION}"
make -j$(nproc)
make install DESTDIR="${INSTALL_DIR}"

# Create AppDir structure
mkdir -p "${APPDIR}/usr/bin"
mkdir -p "${APPDIR}/usr/lib"
mkdir -p "${APPDIR}/usr/share/applications"
mkdir -p "${APPDIR}/usr/share/icons/hicolor/scalable/apps"

# Copy application files
cp "${INSTALL_DIR}/usr/bin/winafi-gui" "${APPDIR}/usr/bin/"
cp "${INSTALL_DIR}/usr/bin/winafi" "${APPDIR}/usr/bin/"
cp "${PROJECT_ROOT}/packaging/winafi.svg" "${APPDIR}/usr/share/icons/hicolor/scalable/apps/winafi.svg"

# Copy libraries if needed (linuxdeploy will handle this)
if [ -d "${INSTALL_DIR}/usr/lib" ]; then
    cp -r "${INSTALL_DIR}/usr/lib"/* "${APPDIR}/usr/lib/" 2>/dev/null || true
fi

# Create desktop entry
cat > "${APPDIR}/usr/share/applications/winafi.desktop" << 'EOF'
[Desktop Entry]
Type=Application
Name=Winafi
Comment=USB bootable drive utility for Linux
Exec=winafi-gui
Icon=winafi
Categories=Utility;System;
Terminal=false
EOF

# Create AppRun script
cat > "${APPDIR}/AppRun" << 'EOF'
#!/bin/bash
SELF=$(readlink -f "$0")
HERE=${SELF%/*}
export LD_LIBRARY_PATH="${HERE}/usr/lib:${LD_LIBRARY_PATH}"
exec "${HERE}/usr/bin/winafi-gui" "$@"
EOF
chmod +x "${APPDIR}/AppRun"

# Download and use linuxdeploy to build directory
if ! command -v linuxdeploy &> /dev/null; then
    echo "Downloading linuxdeploy..."
    wget -q https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-"${ARCH}".AppImage
    chmod +x linuxdeploy-"${ARCH}".AppImage
    LINUXDEPLOY="$(pwd)/linuxdeploy-${ARCH}.AppImage"
else
    LINUXDEPLOY="linuxdeploy"
fi

# Download linuxdeploy-plugin-qt
if ! command -v linuxdeploy-plugin-qt &> /dev/null; then
    echo "Downloading linuxdeploy-plugin-qt..."
    wget -q https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-"${ARCH}".AppImage
    chmod +x linuxdeploy-plugin-qt-"${ARCH}".AppImage
    export LINUXDEPLOY_PLUGIN_QT_APPIMAGE="$(pwd)/linuxdeploy-plugin-qt-${ARCH}.AppImage"
else
    export LINUXDEPLOY_PLUGIN_QT_APPIMAGE="linuxdeploy-plugin-qt"
fi

# Use linuxdeploy to bundle dependencies
"${LINUXDEPLOY}" --appdir="${APPDIR}" \
    --desktop-file="${APPDIR}/usr/share/applications/winafi.desktop" \
    --icon-file="${APPDIR}/usr/share/icons/hicolor/scalable/apps/winafi.svg" \
    --output=appimage --plugin=qt

# The AppImage is created in the current directory (BUILD_DIR)
# Move it to project root and list it
cd ..
mv "${BUILD_DIR}"/Winafi-*.AppImage* . 2>/dev/null || true
ls -lh Winafi-*.AppImage*
