# The HTML chrome is plain data: installed as-is and loaded by the frontend.
include(GNUInstallDirs)

set(PROSPEROWEB_CHROME_INSTALL_DIR "${CMAKE_INSTALL_DATADIR}/prosperoweb/chrome-ui"
    CACHE STRING "Install location of the HTML chrome, relative to the prefix")

install(FILES chrome.html chrome.css chrome.js diag.html
        DESTINATION "${PROSPEROWEB_CHROME_INSTALL_DIR}")

# Frontends link this to learn where the chrome lives: the installed copy
# first, the source tree as a fallback for running from the build tree.
add_library(prosperoweb_chrome_ui INTERFACE)
add_library(prosperoweb::chrome_ui ALIAS prosperoweb_chrome_ui)
target_compile_definitions(prosperoweb_chrome_ui INTERFACE
  PROSPEROWEB_CHROME_DIR="${CMAKE_INSTALL_PREFIX}/${PROSPEROWEB_CHROME_INSTALL_DIR}"
  PROSPEROWEB_CHROME_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
