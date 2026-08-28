cmake_minimum_required(VERSION 3.24)

file(READ "${CMAKE_CURRENT_LIST_DIR}/IBStartSemVer.regex" IBSTART_SEMVER_REGEX)
string(STRIP "${IBSTART_SEMVER_REGEX}" IBSTART_SEMVER_REGEX)
if(IBSTART_SEMVER_REGEX STREQUAL "")
  message(FATAL_ERROR "The shared SemVer regular expression is empty.")
endif()

set(valid_versions
  "0.0.0"
  "1.2.3"
  "1.2.3-0"
  "1.2.3-alpha"
  "1.2.3-alpha.1"
  "1.2.3-0alpha"
  "1.2.3-alpha-1"
  "1.2.3-alpha.0"
)
set(invalid_versions
  "00.1.2"
  "1.02.3"
  "1.2.03"
  "1.2.3-"
  "1.2.3-alpha."
  "1.2.3-.alpha"
  "1.2.3-alpha..1"
  "1.2.3-alpha...1"
  "1.2.3-01"
  "1.2.3-alpha.01"
  "1.2.3+build"
  "1.2.3-alpha_1"
)

foreach(version IN LISTS valid_versions)
  if(NOT "${version}" MATCHES "${IBSTART_SEMVER_REGEX}")
    message(FATAL_ERROR "CMake rejected valid SemVer '${version}'.")
  endif()
endforeach()

foreach(version IN LISTS invalid_versions)
  if("${version}" MATCHES "${IBSTART_SEMVER_REGEX}")
    message(FATAL_ERROR "CMake accepted invalid SemVer '${version}'.")
  endif()
endforeach()

set(prerelease_version "1.2.3-alpha.1")
if(NOT "${prerelease_version}" MATCHES "${IBSTART_SEMVER_REGEX}" OR
    NOT CMAKE_MATCH_4 STREQUAL "-alpha.1")
  message(FATAL_ERROR "CMake did not capture the complete prerelease component.")
endif()

message(STATUS "CMake SemVer validation tests passed.")
