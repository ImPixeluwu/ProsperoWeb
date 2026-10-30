# The HTML chrome is plain data: installed as-is and loaded by the frontend.
include(GNUInstallDirs)

set(PS5BROWSER_CHROME_INSTALL_DIR "${CMAKE_INSTALL_DATADIR}/ps5browser/chrome-ui"
    CACHE STRING "Install location of the HTML chrome, relative to the prefix")

install(FILES chrome.html chrome.css chrome.js
        DESTINATION "${PS5BROWSER_CHROME_INSTALL_DIR}")

# Frontends link this to learn where the chrome lives: the installed copy
# first, the source tree as a fallback for running from the build tree.
add_library(ps5browser_chrome_ui INTERFACE)
add_library(ps5browser::chrome_ui ALIAS ps5browser_chrome_ui)
target_compile_definitions(ps5browser_chrome_ui INTERFACE
  PS5BROWSER_CHROME_DIR="${CMAKE_INSTALL_PREFIX}/${PS5BROWSER_CHROME_INSTALL_DIR}"
  PS5BROWSER_CHROME_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
