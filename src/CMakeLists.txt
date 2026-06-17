# Core library - only include available files
add_library(winafi-core STATIC
    core/error.c
    core/progress.c
    core/log.c
    platform/linux/device.c
    platform/linux/device_validate.c
    platform/linux/partition.c
    platform/linux/mount_ops.c
    platform/linux/fs_ops.c
    platform/linux/iso.c
    platform/linux/iso_verify.c
    platform/linux/iso_extract.c
    platform/linux/bootloader.c
    platform/linux/badblocks_check.c
    platform/linux/windows_boot.c
    platform/linux/linux_boot.c
    platform/linux/session.c
    platform/linux/settings.c
    platform/linux/net.c
    platform/linux/process.c
    platform/linux/proc_utils.c
    platform/linux/wue.c
    platform/linux/wimboot.c
    platform/linux/smart.c
    platform/linux/sbat.c
    platform/linux/pe.c
    platform/linux/assets.c
    platform/linux/pki.c
    platform/linux/update.c
    platform/linux/localization_linux.c
)

target_include_directories(winafi-core PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${LIBUDEV_INCLUDE_DIRS}
    ${LIBPARTED_INCLUDE_DIRS}
    ${LIBARCHIVE_INCLUDE_DIRS}
    ${LIBMOUNT_INCLUDE_DIRS}
)

target_link_libraries(winafi-core PRIVATE
    ${LIBUDEV_LIBRARIES}
    ${LIBPARTED_LIBRARIES}
    ${LIBARCHIVE_LIBRARIES}
    ${LIBMOUNT_LIBRARIES}
    ${LIBBLKID_LIBRARIES}
    ${OPENSSL_LIBRARIES}
    CURL::libcurl
)

# SMART monitoring (optional libatasmart backend)
if(LIBATASMART_FOUND)
    target_compile_definitions(winafi-core PRIVATE HAVE_LIBATASMART)
    target_include_directories(winafi-core PRIVATE ${LIBATASMART_INCLUDE_DIRS})
    target_link_libraries(winafi-core PRIVATE ${LIBATASMART_LIBRARIES})
endif()

# Shared CLI logic library
add_library(cli_logic STATIC cli/cli.c)
target_include_directories(cli_logic PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(cli_logic PRIVATE winafi-core)

# CLI executable
add_executable(winafi cli/main.c)
target_link_libraries(winafi PRIVATE cli_logic winafi-core)
target_include_directories(winafi PRIVATE ${CMAKE_SOURCE_DIR}/include)

# Qt5 GUI library (for sharing between executable and tests)
if(Qt5_FOUND AND BUILD_GUI)
    add_library(winafi_gui STATIC
        gui/MainWindow.cpp
        gui/MainWindow.h
        gui/theme/Theme.cpp
        gui/theme/Theme.h
        gui/sections/SourceSection.cpp
        gui/sections/SourceSection.h
        gui/sections/TargetSection.cpp
        gui/sections/TargetSection.h
        gui/sections/WindowsCustomizeSection.cpp
        gui/sections/WindowsCustomizeSection.h
        gui/sections/AdvancedSection.cpp
        gui/sections/AdvancedSection.h
        gui/WorkerThread.cpp
        gui/WorkerThread.h
        gui/AdvancedDriveDialog.cpp
        gui/AdvancedDriveDialog.h
        gui/AdvancedFormatDialog.cpp
        gui/AdvancedFormatDialog.h
        gui/ISOHashDialog.cpp
        gui/ISOHashDialog.h
        gui/ISOHashWorkerThread.cpp
        gui/ISOHashWorkerThread.h
        gui/dark_mode.cpp
        gui/dark_mode.h
        gui/resources.qrc
    )
    target_link_libraries(winafi_gui PUBLIC cli_logic winafi-core Qt5::Widgets Qt5::Concurrent)
    target_include_directories(winafi_gui PUBLIC ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/src/gui)

    # Qt5 GUI executable (optional)
    add_executable(winafi-gui
        gui/main.cpp
    )
    target_link_libraries(winafi-gui PRIVATE winafi_gui winafi-core)
    target_include_directories(winafi-gui PRIVATE ${CMAKE_SOURCE_DIR}/include)
    install(TARGETS winafi-gui
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
endif()

# Installation
install(TARGETS winafi
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(TARGETS winafi-core
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(FILES ${CMAKE_SOURCE_DIR}/include/winafi.h DESTINATION include)
install(FILES errors.txt DESTINATION share/winafi)
install(FILES assets/uefi-ntfs/bootx64.efi DESTINATION share/winafi/assets/uefi-ntfs)
