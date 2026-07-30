# Same reasoning as Findx264.cmake: no upstream CMake package config, only a
# vcpkg-generated pkg-config (.pc) file.

find_package(x265 CONFIG)
if (NOT x265_FOUND)
    find_package(PkgConfig)
    if (PKG_CONFIG_FOUND)
        pkg_search_module(x265 IMPORTED_TARGET GLOBAL x265)
        if (x265_FOUND)
            add_library(x265::x265 ALIAS PkgConfig::x265)
        endif ()
    endif ()
endif ()

find_package_handle_standard_args(x265
        REQUIRED_VARS
        x265_LINK_LIBRARIES
        x265_FOUND
        VERSION_VAR x265_VERSION
)
