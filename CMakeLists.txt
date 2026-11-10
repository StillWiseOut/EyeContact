cmake_minimum_required(VERSION 3.21)
project(EyeContact LANGUAGES CXX)

if(NOT WIN32 OR NOT MSVC)
  message(FATAL_ERROR "EyeContact builds only on Windows with MSVC (Maxine AR SDK is Windows/Linux, Premiere is Windows/macOS).")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "Configure for x64 (-A x64).")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(AE_SDK_DIR "" CACHE PATH "After Effects SDK folder (the one containing Examples/, or Examples/ itself)")
set(MAXINE_AR_SDK_DIR "" CACHE PATH "NVIDIA Maxine AR SDK folder (contains nvAR.h and nvARProxy.cpp somewhere inside)")

# ---------------------------------------------------------------- After Effects SDK
if(EXISTS "${AE_SDK_DIR}/Examples/Headers/AE_Effect.h")
  set(AE_SDK_DIR "${AE_SDK_DIR}/Examples")
endif()
if(NOT EXISTS "${AE_SDK_DIR}/Headers/AE_Effect.h")
  file(GLOB_RECURSE _ae_hdr "${AE_SDK_DIR}/AE_Effect.h")
  list(FILTER _ae_hdr INCLUDE REGEX "/Headers/AE_Effect.h$")
  if(_ae_hdr)
    list(GET _ae_hdr 0 _ae_hdr)
    get_filename_component(_ae_headers "${_ae_hdr}" DIRECTORY)
    get_filename_component(AE_SDK_DIR "${_ae_headers}" DIRECTORY)
  else()
    message(FATAL_ERROR "AE_SDK_DIR='${AE_SDK_DIR}' does not contain Headers/AE_Effect.h. Download the After Effects SDK from Adobe and point AE_SDK_DIR at it.")
  endif()
endif()
set(AE_HEADERS   "${AE_SDK_DIR}/Headers")
set(AE_UTIL      "${AE_SDK_DIR}/Util")
set(AE_RESOURCES "${AE_SDK_DIR}/Resources")
find_program(PIPLTOOL PiPLTool HINTS "${AE_RESOURCES}" NO_DEFAULT_PATH)
if(NOT PIPLTOOL)
  message(FATAL_ERROR "PiPLTool.exe not found in ${AE_RESOURCES}")
endif()
message(STATUS "After Effects SDK: ${AE_SDK_DIR}")

# ---------------------------------------------------------------- Maxine AR SDK
file(GLOB_RECURSE _nvar_h     "${MAXINE_AR_SDK_DIR}/nvAR.h")
file(GLOB_RECURSE _nvar_proxy "${MAXINE_AR_SDK_DIR}/nvARProxy.cpp")
file(GLOB_RECURSE _nvcv_proxy "${MAXINE_AR_SDK_DIR}/nvCVImageProxy.cpp")
if(NOT _nvar_h OR NOT _nvar_proxy OR NOT _nvcv_proxy)
  message(FATAL_ERROR "MAXINE_AR_SDK_DIR='${MAXINE_AR_SDK_DIR}' must contain nvAR.h, nvARProxy.cpp and nvCVImageProxy.cpp (the AR SDK developer package, not only the runtime).")
endif()
list(GET _nvar_h 0 _nvar_h)
list(GET _nvar_proxy 0 _nvar_proxy)
list(GET _nvcv_proxy 0 _nvcv_proxy)
get_filename_component(NVAR_INCLUDE "${_nvar_h}" DIRECTORY)
message(STATUS "Maxine AR SDK headers: ${NVAR_INCLUDE}")

# ---------------------------------------------------------------- PiPL resource
set(PIPL_R  "${CMAKE_CURRENT_SOURCE_DIR}/src/EyeContactPiPL.r")
set(PIPL_RC "${CMAKE_CURRENT_BINARY_DIR}/EyeContactPiPL.rc")
add_custom_command(
  OUTPUT "${PIPL_RC}"
  COMMAND "${CMAKE_COMMAND}"
          "-DCL=${CMAKE_CXX_COMPILER}"
          "-DPIPLTOOL=${PIPLTOOL}"
          "-DAE_HEADERS=${AE_HEADERS}"
          "-DAE_RESOURCES=${AE_RESOURCES}"
          "-DIN=${PIPL_R}"
          "-DOUT=${PIPL_RC}"
          "-DWORK=${CMAKE_CURRENT_BINARY_DIR}"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/BuildPiPL.cmake"
  DEPENDS "${PIPL_R}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/BuildPiPL.cmake"
  COMMENT "Compiling PiPL resource"
  VERBATIM)

# ---------------------------------------------------------------- plugin
add_library(EyeContact SHARED
  src/EyeContact.cpp
  src/MaxineEngine.cpp
  src/MaxineEngine.h
  "${_nvar_proxy}"
  "${_nvcv_proxy}"
  "${PIPL_RC}")

set_target_properties(EyeContact PROPERTIES
  SUFFIX ".aex"
  # Static CRT: nothing extra to install next to the .aex.
  MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

target_include_directories(EyeContact PRIVATE
  "${AE_HEADERS}"
  "${AE_HEADERS}/SP"
  "${AE_HEADERS}/Win"
  "${AE_HEADERS}/adobesdk"
  "${AE_UTIL}"
  "${NVAR_INCLUDE}")

target_compile_definitions(EyeContact PRIVATE
  MSWindows WIN32 _WINDOWS NOMINMAX _CRT_SECURE_NO_WARNINGS)

target_compile_options(EyeContact PRIVATE /W3 /MP /utf-8 /EHsc)

# ---------------------------------------------------------------- test tool (engine without Premiere)
add_executable(EyeContactTest
  tools/EyeContactTest.cpp
  src/MaxineEngine.cpp
  "${_nvar_proxy}"
  "${_nvcv_proxy}")
set_target_properties(EyeContactTest PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
target_include_directories(EyeContactTest PRIVATE src "${NVAR_INCLUDE}")
target_compile_definitions(EyeContactTest PRIVATE WIN32 _WINDOWS NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(EyeContactTest PRIVATE /W3 /utf-8 /EHsc)
