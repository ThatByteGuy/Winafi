cmake_minimum_required(VERSION 3.20)
project(Winafi
    VERSION 0.0.5
    DESCRIPTION "Winafi - USB bootable drive utility for Linux"
    LANGUAGES C CXX
)
set(WINAFI_BUILD_VERSION "${PROJECT_VERSION}" CACHE STRING "Version string embedded in Winafi binaries")

include(GNUInstallDirs)

# C17 standard required
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# C++17 for GUI
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find required packages
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBUDEV REQUIRED libudev>=200)
pkg_check_modules(GTK4 OPTIONAL gtk4>=4.0)
pkg_check_modules(LIBPARTED REQUIRED libparted>=3.0)
pkg_check_modules(LIBARCHIVE REQUIRED libarchive)
pkg_check_modules(LIBMOUNT OPTIONAL libmount)
pkg_check_modules(LIBBLKID OPTIONAL blkid)
pkg_check_modules(LIBCDIO REQUIRED libcdio)
pkg_check_modules(OPENSSL REQUIRED openssl>=1.1)
find_package(CURL REQUIRED)

# Optional packages
pkg_check_modules(NTFS3G OPTIONAL ntfs-3g)
pkg_check_modules(LIBEXFAT OPTIONAL libexfat)
pkg_check_modules(LIBATASMART libatasmart)

# Qt5 for GUI (optional)
find_package(Qt5 COMPONENTS Widgets Concurrent QUIET)

# Build options
option(BUILD_TESTS "Build unit tests" ON)
option(BUILD_GUI "Build Qt5 GUI" ${Qt5_FOUND})
option(ENABLE_NTFS "Enable NTFS support" ${NTFS3G_FOUND})
option(ENABLE_EXFAT "Enable exFAT support" ${LIBEXFAT_FOUND})
option(ENABLE_DOCS "Build documentation" OFF)

# Qt5 automoc/autouic/autorcc
if(Qt5_FOUND AND BUILD_GUI)
    set(CMAKE_AUTOMOC ON)
    set(CMAKE_AUTOUIC ON)
    set(CMAKE_AUTORCC ON)
endif()

# Version info
add_compile_definitions(
    WINAFI_VERSION="${WINAFI_BUILD_VERSION}"
    WINAFI_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    WINAFI_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    WINAFI_VERSION_PATCH=${PROJECT_VERSION_PATCH}
)

# Compiler flags
add_compile_options(
    -Wall
    -Wextra
    -Wpedantic
    -Wconversion
    -Wshadow
    -Wwrite-strings
    $<$<COMPILE_LANGUAGE:C>:-Wstrict-prototypes>
)

# Debug vs Release
if(CMAKE_BUILD_TYPE MATCHES Debug)
    add_compile_definitions(DEBUG_BUILD)
    add_compile_options(-g -O0)
else()
    add_compile_options(-O2)
    add_compile_definitions(NDEBUG)
endif()

# Include directories
include_directories(
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_CURRENT_BINARY_DIR}
)

# Add subdirectories
add_subdirectory(src)

if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(ENABLE_DOCS)
    add_subdirectory(docs)
endif()

# Desktop integration — shared by every package format (deb/rpm/AUR/AppImage/Flatpak/Snap).
# Installing these from CMake keeps a single source of truth instead of inlining a .desktop
# entry in each packaging script.
if(BUILD_GUI)
    install(FILES packaging/winafi.desktop DESTINATION share/applications)
    install(FILES packaging/winafi.svg
            DESTINATION share/icons/hicolor/scalable/apps
            RENAME winafi.svg)
endif()

# Convert build type to uppercase for compiler flags
string(TOUPPER "${CMAKE_BUILD_TYPE}" CMAKE_BUILD_TYPE_UPPER)

# Print configuration summary
message(STATUS "")
message(STATUS "=== Winafi Configuration ===")
message(STATUS "Version: ${WINAFI_BUILD_VERSION}")
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "C Compiler: ${CMAKE_C_COMPILER}")
message(STATUS "C Flags: ${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_${CMAKE_BUILD_TYPE_UPPER}}")
message(STATUS "")
message(STATUS "Dependencies:")
message(STATUS "  libudev: ${LIBUDEV_FOUND}")
message(STATUS "  libparted: ${LIBPARTED_FOUND}")
message(STATUS "  libblkid: ${LIBBLKID_FOUND}")
message(STATUS "  libcdio: ${LIBCDIO_FOUND}")
message(STATUS "  OpenSSL: ${OPENSSL_FOUND}")
message(STATUS "  libcurl: ${CURL_FOUND}")
message(STATUS "  Qt5: ${Qt5_FOUND}")
message(STATUS "")
message(STATUS "Optional Features:")
message(STATUS "  Qt5 GUI: ${BUILD_GUI} (${Qt5_FOUND})")
message(STATUS "  NTFS support: ${ENABLE_NTFS}")
message(STATUS "  exFAT support: ${ENABLE_EXFAT}")
message(STATUS "  libatasmart: ${LIBATASMART_FOUND}")
message(STATUS "  Build tests: ${BUILD_TESTS}")
message(STATUS "  Build docs: ${ENABLE_DOCS}")
message(STATUS "")
message(STATUS "To build:")
message(STATUS "  make -j\$(nproc)")
message(STATUS "")
