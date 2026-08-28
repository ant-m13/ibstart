cmake_minimum_required(VERSION 3.24)

get_filename_component(PROJECT_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
file(GLOB_RECURSE DOCUMENTATION_FILES LIST_DIRECTORIES false
    "${PROJECT_ROOT}/docs/*.md"
    "${PROJECT_ROOT}/README.md"
    "${PROJECT_ROOT}/CONTRIBUTING.md")

set(DOCUMENTATION_LINK_ERRORS)
set(SOURCE_LINK_COUNT 0)

foreach(DOCUMENTATION_FILE IN LISTS DOCUMENTATION_FILES)
  file(READ "${DOCUMENTATION_FILE}" DOCUMENTATION_CONTENT)
  string(REGEX MATCHALL "\\[[^]]+\\]\\((\\.\\./)?src/[^)]+\\)"
      SOURCE_LINKS "${DOCUMENTATION_CONTENT}")
  get_filename_component(DOCUMENTATION_DIRECTORY "${DOCUMENTATION_FILE}" DIRECTORY)

  foreach(SOURCE_LINK IN LISTS SOURCE_LINKS)
    math(EXPR SOURCE_LINK_COUNT "${SOURCE_LINK_COUNT} + 1")
    string(REGEX MATCH "^\\[([^]]+)\\]\\(([^)]+)\\)$" UNUSED_MATCH "${SOURCE_LINK}")
    set(LINK_LABEL "${CMAKE_MATCH_1}")
    set(LINK_TARGET "${CMAKE_MATCH_2}")

    if(LINK_TARGET MATCHES ":[0-9]+$" OR LINK_TARGET MATCHES "#L[0-9]+")
      list(APPEND DOCUMENTATION_LINK_ERRORS
          "${DOCUMENTATION_FILE}: line-based source link is not stable: ${SOURCE_LINK}")
      continue()
    endif()

    if(NOT LINK_LABEL MATCHES "^`[^`]+`$")
      list(APPEND DOCUMENTATION_LINK_ERRORS
          "${DOCUMENTATION_FILE}: source links must name a backtick-quoted symbol: ${SOURCE_LINK}")
      continue()
    endif()

    string(REGEX REPLACE "^`" "" SYMBOL "${LINK_LABEL}")
    string(REGEX REPLACE "`$" "" SYMBOL "${SYMBOL}")
    get_filename_component(SOURCE_FILE "${DOCUMENTATION_DIRECTORY}/${LINK_TARGET}" ABSOLUTE)
    if(NOT EXISTS "${SOURCE_FILE}" OR IS_DIRECTORY "${SOURCE_FILE}")
      list(APPEND DOCUMENTATION_LINK_ERRORS
          "${DOCUMENTATION_FILE}: source link target does not exist: ${LINK_TARGET}")
      continue()
    endif()

    file(READ "${SOURCE_FILE}" SOURCE_CONTENT)
    string(FIND "${SOURCE_CONTENT}" "${SYMBOL}" SYMBOL_POSITION)
    if(SYMBOL_POSITION EQUAL -1 AND SYMBOL MATCHES "::")
      string(REGEX REPLACE "^.*::" "" UNQUALIFIED_SYMBOL "${SYMBOL}")
      string(FIND "${SOURCE_CONTENT}" "${UNQUALIFIED_SYMBOL}" SYMBOL_POSITION)
    endif()
    if(SYMBOL_POSITION EQUAL -1)
      list(APPEND DOCUMENTATION_LINK_ERRORS
          "${DOCUMENTATION_FILE}: symbol '${SYMBOL}' was not found in ${LINK_TARGET}")
    endif()
  endforeach()
endforeach()

if(DOCUMENTATION_LINK_ERRORS)
  string(REPLACE ";" "\n  - " DOCUMENTATION_LINK_ERRORS_TEXT "${DOCUMENTATION_LINK_ERRORS}")
  message(FATAL_ERROR "Documentation source-link check failed:\n  - ${DOCUMENTATION_LINK_ERRORS_TEXT}")
endif()

message(STATUS "Documentation source-link check passed (${SOURCE_LINK_COUNT} source links).")
