# x264 has no upstream CMake package config -- vcpkg only generates a
# pkg-config (.pc) file for it (vcpkg_fixup_pkgconfig() in its portfile), so
# fall back to that the same way Findlibusb.cmake already does for libusb.

find_package(x264 CONFIG)
if (NOT x264_FOUND)
    find_package(PkgConfig)
    if (PKG_CONFIG_FOUND)
        pkg_search_module(x264 IMPORTED_TARGET GLOBAL x264)
        if (x264_FOUND)
            add_library(x264::x264 ALIAS PkgConfig::x264)
        endif ()
    endif ()
endif ()

find_package_handle_standard_args(x264
        REQUIRED_VARS
        x264_LINK_LIBRARIES
        x264_FOUND
        VERSION_VAR x264_VERSION
)
