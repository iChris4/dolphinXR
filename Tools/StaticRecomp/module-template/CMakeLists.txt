cmake_minimum_required(VERSION 3.16)
project(recompcore_module C)

# Per-game native module for RecompCore: generated chunks + DolRecomp CPU
# runtime + ABI export glue, no other host dependencies.
#
# Configure with:
#   cmake -B build -DGAME_ID=G4QE01 \
#         -DGENERATED_DIR=/path/to/generated \
#         -DGXRUNTIME_DIR=/path/to/GXRuntime

set(CMAKE_C_STANDARD 11)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(GAME_ID "" CACHE STRING "Six-character disc ID, e.g. G4QE01")
if(NOT GAME_ID)
  message(FATAL_ERROR "Set -DGAME_ID=<disc id> (e.g. -DGAME_ID=G4QE01)")
endif()
set(GENERATED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/generated" CACHE PATH "DolRecomp output directory (generated.h, generated_smc.txt, main.dol, chunks/)")
if(NOT EXISTS "${GENERATED_DIR}")
  message(FATAL_ERROR "Generated directory not found at ${GENERATED_DIR}. Set -DGENERATED_DIR=<path to DolRecomp output>")
endif()
foreach(REQUIRED_GENERATED_FILE generated.h generated_smc.txt main.dol)
  if(NOT EXISTS "${GENERATED_DIR}/${REQUIRED_GENERATED_FILE}")
    message(FATAL_ERROR
      "DolRecomp output is incomplete: ${GENERATED_DIR}/${REQUIRED_GENERATED_FILE} is missing")
  endif()
endforeach()
set(GXRUNTIME_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../Externals/GXRuntime" CACHE PATH "GXRuntime checkout")
if(NOT EXISTS "${GXRUNTIME_DIR}")
  message(FATAL_ERROR "GXRuntime not found at ${GXRUNTIME_DIR}. Set -DGXRUNTIME_DIR=<path to GXRuntime>")
endif()
set(CHASSIS_ABI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../Source/Core/Core/PowerPC/StaticRecomp"
    CACHE PATH "RecompCore StaticRecomp ABI directory")

set(MODULE_NAME "g${GAME_ID}_recomp")

find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(MODULE_TABLES "${CMAKE_CURRENT_BINARY_DIR}/module_tables.inc")
add_custom_command(
  OUTPUT "${MODULE_TABLES}"
  COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/gen_module_tables.py"
          "${GENERATED_DIR}/generated.h" "${GENERATED_DIR}/generated_smc.txt"
          "${GENERATED_DIR}/main.dol" "${MODULE_TABLES}"
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/gen_module_tables.py"
          "${GENERATED_DIR}/generated.h" "${GENERATED_DIR}/generated_smc.txt"
          "${GENERATED_DIR}/main.dol"
  COMMENT "Generating module_tables.inc")

file(READ "${GENERATED_DIR}/generated.h" GENERATED_HEADER_TEXT)
if(GENERATED_HEADER_TEXT MATCHES "DOLRECOMP_BACKEND_LLVM")
  set(GENERATED_MANIFEST)
  foreach(MANIFEST_CANDIDATE
          "${GENERATED_DIR}/${GAME_ID}.c"
          "${GENERATED_DIR}/generated.c")
    if(EXISTS "${MANIFEST_CANDIDATE}")
      set(GENERATED_MANIFEST "${MANIFEST_CANDIDATE}")
      break()
    endif()
  endforeach()
  if(NOT GENERATED_MANIFEST)
    message(FATAL_ERROR "DolRecomp LLVM object manifest is missing")
  endif()
  file(STRINGS "${GENERATED_MANIFEST}" CHUNK_OBJECT_LINES
       REGEX "^// object: ")
  foreach(CHUNK_OBJECT_LINE IN LISTS CHUNK_OBJECT_LINES)
    string(REGEX REPLACE "^// object: " "" CHUNK_OBJECT
           "${CHUNK_OBJECT_LINE}")
    list(APPEND CHUNK_INPUTS "${GENERATED_DIR}/${CHUNK_OBJECT}")
  endforeach()
  foreach(CHUNK_INPUT IN LISTS CHUNK_INPUTS)
    if(NOT EXISTS "${CHUNK_INPUT}")
      message(FATAL_ERROR "DolRecomp LLVM object is missing: ${CHUNK_INPUT}")
    endif()
  endforeach()
  set_source_files_properties(${CHUNK_INPUTS} PROPERTIES EXTERNAL_OBJECT TRUE)
else()
  file(GLOB CHUNK_INPUTS CONFIGURE_DEPENDS "${GENERATED_DIR}/chunks/*.c")
endif()
if(NOT CHUNK_INPUTS)
  message(FATAL_ERROR "DolRecomp output has no generated chunks in ${GENERATED_DIR}/chunks")
endif()

add_library(${MODULE_NAME} SHARED
  module_export.c
  "${MODULE_TABLES}"
  "${GXRUNTIME_DIR}/src/core/cpu.c"
  "${GXRUNTIME_DIR}/src/core/cpu_exception.c"
  "${GXRUNTIME_DIR}/src/core/cpu_interpreter.c"
  "${GXRUNTIME_DIR}/src/core/cpu_interpreter_table.c"
  "${GXRUNTIME_DIR}/src/core/cpu_interpreter_float.c"
  "${GXRUNTIME_DIR}/src/core/cpu_interpreter_integer.c"
  ${CHUNK_INPUTS})

set_target_properties(${MODULE_NAME} PROPERTIES
  PREFIX ""
  OUTPUT_NAME "${MODULE_NAME}"
  UNITY_BUILD OFF
  C_VISIBILITY_PRESET hidden)
set_source_files_properties("${MODULE_TABLES}" PROPERTIES HEADER_FILE_ONLY TRUE GENERATED TRUE)

target_compile_definitions(${MODULE_NAME} PRIVATE MODULE_GAME_ID="${GAME_ID}")

target_include_directories(${MODULE_NAME} PRIVATE
  "${GENERATED_DIR}"
  "${GXRUNTIME_DIR}/include"
  "${CHASSIS_ABI_DIR}"
  "${CMAKE_CURRENT_BINARY_DIR}")

if(NOT WIN32)
  target_link_libraries(${MODULE_NAME} PRIVATE m)
endif()
if(UNIX AND NOT APPLE)
  target_link_options(${MODULE_NAME} PRIVATE
    "LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/module.exports")
  set_property(TARGET ${MODULE_NAME} APPEND PROPERTY LINK_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/module.exports")
endif()

option(RECOMPCORE_MODULE_ENABLE_IPO "Enable Clang module interprocedural optimization when supported" ON)
set(RECOMPCORE_MODULE_OPT_LEVEL "2" CACHE STRING
    "Optimization level for generated module translation units (0, 1, 2, or 3)")
set_property(CACHE RECOMPCORE_MODULE_OPT_LEVEL PROPERTY STRINGS 0 1 2 3)
if(NOT RECOMPCORE_MODULE_OPT_LEVEL MATCHES "^[0-3]$")
  message(FATAL_ERROR
    "RECOMPCORE_MODULE_OPT_LEVEL must be one of 0, 1, 2, or 3")
endif()
if(RECOMPCORE_MODULE_ENABLE_IPO AND CMAKE_C_COMPILER_ID MATCHES "Clang")
  include(CheckIPOSupported)
  # moderngekko-port configures with CMAKE_NINJA_FORCE_RESPONSE_FILE=1, which the
  # module needs: it links thousands of chunk objects and the command line does
  # not fit in ARG_MAX. But check_ipo_supported's try-compile inherits the
  # environment, and its static-library step then runs `ar qc libfoo.a @foo.rsp`
  # -- Apple's /usr/bin/ar has no @file support, so the check fails with
  # "ar: @CMakeFiles/foo.rsp: No such file or directory" and IPO is silently
  # disabled. The manifest still records -flto=thin. Every module built this way
  # had NO cross-translation-unit inlining. Scope the variable off for the check
  # only, then restore it before the generator reads it.
  set(_module_forced_rsp "$ENV{CMAKE_NINJA_FORCE_RESPONSE_FILE}")
  unset(ENV{CMAKE_NINJA_FORCE_RESPONSE_FILE})
  check_ipo_supported(RESULT RECOMPCORE_MODULE_IPO_SUPPORTED
                      OUTPUT RECOMPCORE_MODULE_IPO_ERROR LANGUAGES C)
  if(_module_forced_rsp)
    set(ENV{CMAKE_NINJA_FORCE_RESPONSE_FILE} "${_module_forced_rsp}")
  endif()
  if(RECOMPCORE_MODULE_IPO_SUPPORTED)
    set_property(TARGET ${MODULE_NAME} PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
  else()
    message(STATUS "Module IPO disabled: ${RECOMPCORE_MODULE_IPO_ERROR}")
  endif()
endif()

if(MSVC)
  if(RECOMPCORE_MODULE_OPT_LEVEL STREQUAL "0")
    target_compile_options(${MODULE_NAME} PRIVATE /Od /fp:strict)
  else()
    target_compile_options(${MODULE_NAME} PRIVATE /O2 /fp:strict)
  endif()
elseif(CMAKE_C_COMPILER_ID MATCHES "Clang")
  target_compile_options(${MODULE_NAME} PRIVATE
    -O${RECOMPCORE_MODULE_OPT_LEVEL} -ffp-contract=off -fno-fast-math)
elseif(CMAKE_C_COMPILER_ID STREQUAL "GNU")
  target_compile_options(${MODULE_NAME} PRIVATE
    -O${RECOMPCORE_MODULE_OPT_LEVEL} -ffp-contract=off -fno-fast-math)
endif()
