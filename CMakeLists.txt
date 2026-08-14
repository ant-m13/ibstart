cmake_minimum_required(VERSION 3.24)

project(IBStart VERSION 0.1.0 LANGUAGES CXX)

if(NOT WIN32)
  message(FATAL_ERROR "IBStart is a native Windows application and must be built on Windows.")
endif()
if(NOT MSVC)
  message(FATAL_ERROR "IBStart is supported with MSVC only.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_library(ibstart_core STATIC
  src/core/catalog/catalog.cpp
  src/core/v8i/v8i_document.cpp
  src/core/v8i/v8i_file_store.cpp
  src/core/launcher/command_builder.cpp
  src/core/platform/platform_discovery.cpp
  src/core/cache/cache_service.cpp
  src/core/scanner/file_base_scanner.cpp
  src/core/storage/storage.cpp
  src/core/logging/logging.cpp
  src/core/shell/shortcut.cpp
)
target_include_directories(ibstart_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_definitions(ibstart_core PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(ibstart_core PRIVATE /W4 /permissive- /EHsc /Zc:__cplusplus)
target_link_libraries(ibstart_core PUBLIC Advapi32 Shell32 Ole32)

add_executable(IBStart WIN32
  src/app/main.cpp
  src/ui/main_window.cpp
  src/app/IBStart.rc
)
target_link_libraries(IBStart PRIVATE ibstart_core Comctl32)
target_compile_definitions(IBStart PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(IBStart PRIVATE /W4 /permissive- /EHsc /Zc:__cplusplus)

if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_CONFIGURATION_TYPES)
  target_compile_options(ibstart_core PRIVATE $<$<CONFIG:Release>:/O1 /Gy>)
  target_compile_options(IBStart PRIVATE $<$<CONFIG:Release>:/O1 /Gy>)
  target_link_options(IBStart PRIVATE $<$<CONFIG:Release>:/OPT:REF /OPT:ICF /INCREMENTAL:NO>)
endif()

add_custom_command(TARGET IBStart POST_BUILD
  COMMAND "${CMAKE_COMMAND}" "-DIBSTART_FILE=$<TARGET_FILE:IBStart>" "-DMAX_SIZE=8388608"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_binary_size.cmake"
  VERBATIM
  COMMENT "Checking IBStart.exe size limit (8 MiB)")

include(CTest)
if(BUILD_TESTING)
  add_executable(ibstart_tests tests/unit/test_main.cpp)
  target_link_libraries(ibstart_tests PRIVATE ibstart_core)
  target_compile_definitions(ibstart_tests PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_compile_options(ibstart_tests PRIVATE /W4 /permissive- /EHsc /Zc:__cplusplus)
  add_test(NAME ibstart.unit COMMAND ibstart_tests)
  set_tests_properties(ibstart.unit PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
endif()
