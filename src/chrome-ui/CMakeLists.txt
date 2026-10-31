# The HTML chrome is plain data: installed as-is and loaded by the frontend.
include(GNUInstallDirs)

set(PROSPEROBROWSER_CHROME_INSTALL_DIR "${CMAKE_INSTALL_DATADIR}/prosperobrowser/chrome-ui"
    CACHE STRING "Install location of the HTML chrome, relative to the prefix")

install(FILES chrome.html chrome.css chrome.js diag.html
        DESTINATION "${PROSPEROBROWSER_CHROME_INSTALL_DIR}")

# Frontends link this to learn where the chrome lives: the installed copy
# first, the source tree as a fallback for running from the build tree.
add_library(prosperobrowser_chrome_ui INTERFACE)
add_library(prosperobrowser::chrome_ui ALIAS prosperobrowser_chrome_ui)
target_compile_definitions(prosperobrowser_chrome_ui INTERFACE
  PROSPEROBROWSER_CHROME_DIR="${CMAKE_INSTALL_PREFIX}/${PROSPEROBROWSER_CHROME_INSTALL_DIR}"
  PROSPEROBROWSER_CHROME_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
