# SPDX-License-Identifier: GPL-3.0-or-later
#
# Stand-in for Sony's proprietary "WebKitRequirements" CMake package used by
# Source/cmake/OptionsPlayStation.cmake and Source/WebCore/PlatformPlayStation.cmake.
#
# Components:
#   WebKitResources  - Sony's resource bundle. Nothing to link: empty target.
#   ProcessLauncher  - Sony's <process-launcher.h> library. The PS5_HOMEBREW
#                      launcher calls ps5_spawn_self() instead, which the
#                      shell provides at final link. Empty target.
#   libwpe           - libwpe from the dependency prefix. Also defines the
#                      WPE::libwpe target that WebCore links directly.
# Optional components (JPEG, LibPSL, LibXml2, SQLite3, WebP, ZLIB) are
# reported as not provided, so OptionsPlayStation finds the real libraries in
# the SDK / prefix with the usual Find modules.

set(WebKitRequirements_FOUND TRUE)

if (NOT TARGET WebKitRequirements::WebKitResources)
    add_library(WebKitRequirements::WebKitResources INTERFACE IMPORTED GLOBAL)
endif ()
set(WebKitRequirements_WebKitResources_FOUND TRUE)

if (NOT TARGET WebKitRequirements::ProcessLauncher)
    add_library(WebKitRequirements::ProcessLauncher INTERFACE IMPORTED GLOBAL)
endif ()
set(WebKitRequirements_ProcessLauncher_FOUND TRUE)

# libwpe installs its headers under include/wpe-1.0/, which the upstream
# FindWPE.cmake does not search without pkg-config (disabled on PlayStation).
find_path(WPE_INCLUDE_DIR NAMES wpe/wpe.h PATH_SUFFIXES wpe-1.0)
find_library(WPE_LIBRARY NAMES wpe-1.0)
find_library(XKBCOMMON_LIBRARY NAMES xkbcommon)
find_package(WPE 1.14.0)
if (WPE_FOUND AND TARGET WPE::libwpe)
    if (XKBCOMMON_LIBRARY)
        set_property(TARGET WPE::libwpe APPEND PROPERTY INTERFACE_LINK_LIBRARIES "${XKBCOMMON_LIBRARY}")
    endif ()
    if (NOT TARGET WebKitRequirements::libwpe)
        add_library(WebKitRequirements::libwpe INTERFACE IMPORTED GLOBAL)
        set_property(TARGET WebKitRequirements::libwpe PROPERTY INTERFACE_LINK_LIBRARIES WPE::libwpe)
    endif ()
    set(WebKitRequirements_libwpe_FOUND TRUE)
else ()
    set(WebKitRequirements_libwpe_FOUND FALSE)
endif ()

foreach (_comp IN LISTS WebKitRequirements_FIND_COMPONENTS)
    if (NOT WebKitRequirements_${_comp}_FOUND)
        set(WebKitRequirements_${_comp}_FOUND FALSE)
        if (WebKitRequirements_FIND_REQUIRED_${_comp})
            set(WebKitRequirements_FOUND FALSE)
            set(WebKitRequirements_NOT_FOUND_MESSAGE "PS5 WebKitRequirements: component ${_comp} not available")
        endif ()
    endif ()
endforeach ()
