target_sources(kwin PRIVATE
    hwcomposer_backend.cpp
    hwcomposer_egl_backend.cpp
    hwcomposer_logging.cpp  
)

find_package(PkgConfig)
pkg_check_modules(EGL REQUIRED egl)
pkg_check_modules(HWC_libhardware REQUIRED libhardware)
pkg_check_modules(HWC_androidheaders REQUIRED android-headers)
pkg_check_modules(HWC_hwcomposerwindow REQUIRED hwcomposer-egl)
pkg_check_modules(HWC_hybriseglplatform REQUIRED hybris-egl-platform)
pkg_check_modules(PKG_sync REQUIRED libsync)
pkg_check_modules(PKG_udev REQUIRED libudev)

include_directories(${CMAKE_SOURCE_DIR}/platformsupport/scenes/opengl)

target_include_directories(kwin PUBLIC ${EGL_INCLUDE_DIRS}
    ${HWC_libhardware_INCLUDE_DIRS}
    ${HWC_androidheaders_INCLUDE_DIRS}
    ${HWC_hwcomposerwindow_INCLUDE_DIRS}
    ${HWC_hybriseglplatform_INCLUDE_DIRS}
    ${PKG_sync_INCLUDE_DIRS}
    ${PKG_udev_INCLUDE_DIRS}
    )

target_link_libraries(kwin PRIVATE
    ${EGL_LIBRARIES}
    ${PKG_sync_LIBRARIES}
    ${PKG_udev_LIBRARIES}
    ${HWC_libhardware_LIBRARIES}
    ${HWC_androidheaders_LIBRARIES}
    ${HWC_hwcomposerwindow_LIBRARIES}
    ${HWC_hybriseglplatform_LIBRARIES}
    -lhwc2 -lsync
    dl
)
