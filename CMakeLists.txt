cmake_minimum_required(VERSION 3.24)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/IBStartVersion.cmake")
if(NOT IBSTART_VERSION MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)(-([0-9A-Za-z]+)(\\.([0-9A-Za-z.-]+))?)?$")
  message(FATAL_ERROR "IBSTART_VERSION must be SemVer without build metadata: ${IBSTART_VERSION}")
endif()
set(IBSTART_VERSION_MAJOR "${CMAKE_MATCH_1}")
set(IBSTART_VERSION_MINOR "${CMAKE_MATCH_2}")
set(IBSTART_VERSION_PATCH "${CMAKE_MATCH_3}")
set(IBSTART_VERSION_PRERELEASE "${CMAKE_MATCH_4}")
set(IBSTART_VERSION_REVISION 0)
if(IBSTART_VERSION_PRERELEASE MATCHES "([0-9]+)$")
  set(IBSTART_VERSION_REVISION "${CMAKE_MATCH_1}")
endif()
set(IBSTART_VERSION_CORE "${IBSTART_VERSION_MAJOR}.${IBSTART_VERSION_MINOR}.${IBSTART_VERSION_PATCH}")
set(IBSTART_FILE_VERSION "${IBSTART_VERSION_CORE}.${IBSTART_VERSION_REVISION}")
set(IBSTART_VERSION_COMMA "${IBSTART_VERSION_MAJOR},${IBSTART_VERSION_MINOR},${IBSTART_VERSION_PATCH},${IBSTART_VERSION_REVISION}")
set(IBSTART_GITHUB_LATEST_VERSION_ASSET_PATH "/${IBSTART_GITHUB_OWNER}/${IBSTART_GITHUB_REPOSITORY}/releases/latest/download/IBStart.version")
set(IBSTART_GITHUB_RELEASE_PAGE_PREFIX "https://github.com/${IBSTART_GITHUB_OWNER}/${IBSTART_GITHUB_REPOSITORY}/releases/")

project(IBStart VERSION "${IBSTART_VERSION_CORE}" LANGUAGES CXX)

if(NOT WIN32)
  message(FATAL_ERROR "IBStart is a native Windows application and must be built on Windows.")
endif()
enable_language(RC)
if(NOT MSVC)
  message(FATAL_ERROR "IBStart is supported with MSVC only.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
string(REPLACE "/O2" "/O1" CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}")

set(IBSTART_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(IBSTART_RESOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/app")
file(MAKE_DIRECTORY "${IBSTART_GENERATED_DIR}/core/domain")
configure_file(src/core/domain/version.hpp.in "${IBSTART_GENERATED_DIR}/core/domain/version.hpp" @ONLY)
configure_file(src/app/IBStart.rc.in "${IBSTART_GENERATED_DIR}/IBStart.rc" @ONLY)
configure_file(src/app/app.manifest.in "${IBSTART_GENERATED_DIR}/app.manifest" @ONLY)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/ibstart-version.txt" "${IBSTART_VERSION}\n")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/IBStart.portable" "")
message(STATUS "IBStart product version: ${IBSTART_VERSION} (file version ${IBSTART_FILE_VERSION})")

add_library(ibstart_core STATIC
  src/core/connection/connection_string.cpp
  src/core/catalog/catalog.cpp
  src/core/catalog/catalog_metadata_service.cpp
  src/core/v8i/v8i_document.cpp
  src/core/v8i/v8i_file_store.cpp
  src/core/launcher/command_builder.cpp
  src/core/platform/platform_discovery.cpp
  src/core/cache/cache_service.cpp
  src/core/scanner/file_base_scanner.cpp
  src/core/storage/storage.cpp
  src/core/logging/logging.cpp
  src/core/shell/shortcut.cpp
  src/core/update/update_service.cpp
)
target_include_directories(ibstart_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src" "${IBSTART_GENERATED_DIR}")
target_compile_definitions(ibstart_core PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX WINVER=0x0A00 _WIN32_WINNT=0x0A00)
target_compile_options(ibstart_core PRIVATE /W4 /WX /permissive- /EHsc /Zc:__cplusplus /utf-8)
target_link_libraries(ibstart_core PUBLIC Advapi32 Shell32 Ole32 Winhttp)

add_executable(IBStart WIN32
  src/app/main.cpp
  src/ui/database_editor_dialog.cpp
  src/ui/dialog_support.cpp
  src/ui/folder_picker.cpp
  src/ui/input_box.cpp
  src/ui/main_window.cpp
  src/ui/owner_draw_menu.cpp
  src/ui/tag_assignment_dialog.cpp
  src/ui/tag_manager_dialog.cpp
  src/ui/tree_presentation.cpp
  "${IBSTART_GENERATED_DIR}/IBStart.rc"
  "${IBSTART_GENERATED_DIR}/app.manifest"
)
set(IBSTART_ICON_FILES
  "${IBSTART_RESOURCE_DIR}/IBStart.ico"
  "${IBSTART_RESOURCE_DIR}/icons/enterprise.ico"
  "${IBSTART_RESOURCE_DIR}/icons/designer.ico"
  "${IBSTART_RESOURCE_DIR}/icons/edit.ico"
  "${IBSTART_RESOURCE_DIR}/icons/cache.ico"
  "${IBSTART_RESOURCE_DIR}/icons/shortcut.ico"
  "${IBSTART_RESOURCE_DIR}/icons/delete.ico"
  "${IBSTART_RESOURCE_DIR}/icons/add.ico"
  "${IBSTART_RESOURCE_DIR}/icons/folder.ico"
  "${IBSTART_RESOURCE_DIR}/icons/database.ico"
  "${IBSTART_RESOURCE_DIR}/icons/favorite.ico"
  "${IBSTART_RESOURCE_DIR}/icons/refresh.ico"
  "${IBSTART_RESOURCE_DIR}/icons/file_database.ico"
  "${IBSTART_RESOURCE_DIR}/icons/server_database.ico"
  "${IBSTART_RESOURCE_DIR}/icons/update.ico"
  "${IBSTART_RESOURCE_DIR}/icons/exit.ico"
)
set_property(SOURCE "${IBSTART_GENERATED_DIR}/IBStart.rc" PROPERTY OBJECT_DEPENDS "${IBSTART_ICON_FILES}")
target_link_libraries(IBStart PRIVATE ibstart_core Comctl32 Comdlg32)
target_compile_definitions(IBStart PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX WINVER=0x0A00 _WIN32_WINNT=0x0A00)
target_compile_options(IBStart PRIVATE /W4 /WX /permissive- /EHsc /Zc:__cplusplus /utf-8)

if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_CONFIGURATION_TYPES)
  target_compile_options(ibstart_core PRIVATE $<$<CONFIG:Release>:/Gy>)
  target_compile_options(IBStart PRIVATE $<$<CONFIG:Release>:/Gy>)
  target_link_options(IBStart PRIVATE $<$<CONFIG:Release>:/OPT:REF /OPT:ICF /INCREMENTAL:NO>)
endif()

add_custom_command(TARGET IBStart POST_BUILD
  COMMAND "${CMAKE_COMMAND}" "-DIBSTART_FILE=$<TARGET_FILE:IBStart>" "-DMAX_SIZE=8388608"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_binary_size.cmake"
  VERBATIM
  COMMENT "Checking IBStart.exe size limit (8 MiB)")

include(CTest)
if(BUILD_TESTING)
  add_executable(ibstart_tests tests/unit/test_main.cpp src/ui/tree_presentation.cpp)
  target_link_libraries(ibstart_tests PRIVATE ibstart_core Comctl32)
  target_compile_definitions(ibstart_tests PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX WINVER=0x0A00 _WIN32_WINNT=0x0A00)
  target_compile_options(ibstart_tests PRIVATE /W4 /WX /permissive- /EHsc /Zc:__cplusplus /utf-8)
  add_test(NAME ibstart.unit COMMAND ibstart_tests)
  set_tests_properties(ibstart.unit PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
endif()
