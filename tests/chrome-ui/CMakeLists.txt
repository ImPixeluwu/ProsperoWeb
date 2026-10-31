# chrome.js under node with a minimal DOM shim; skipped without node.
find_program(PROSPEROWEB_NODE node)
if(PROSPEROWEB_NODE)
  add_test(NAME chrome_ui.render
           COMMAND "${PROSPEROWEB_NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_chrome.js"
                   "${PROJECT_SOURCE_DIR}/src/chrome-ui/chrome.js")
else()
  message(STATUS "node not found: the chrome_ui.render test is skipped")
endif()
