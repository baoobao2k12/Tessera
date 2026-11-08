cmake_minimum_required(VERSION 3.16)
project(Tessera VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(TESSERA_ARCH "auto" CACHE STRING "Target instruction set: auto, native, bmi2, popcnt, generic")
set_property(CACHE TESSERA_ARCH PROPERTY STRINGS auto native bmi2 popcnt generic)
option(TESSERA_LTO "Enable link-time optimisation in release builds" ON)
set(TESSERA_PGO "off" CACHE STRING "Profile-guided optimisation stage: off, generate, use")
set(TESSERA_PGO_DIR "${CMAKE_BINARY_DIR}/pgo-profile" CACHE PATH "Directory for PGO profile data")
set(TESSERA_SANITIZE "" CACHE STRING "Sanitizers to enable, e.g. address,undefined or thread")

# ---------------------------------------------------------------------------
# Instruction set selection
set(_arch "${TESSERA_ARCH}")
set(_x86 OFF)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64|i.86")
  set(_x86 ON)
endif()
if(_arch STREQUAL "auto" OR _arch STREQUAL "native")
  if(_x86 AND NOT CMAKE_CROSSCOMPILING)
    try_run(_cpu_run _cpu_compile ${CMAKE_BINARY_DIR}/cpudetect ${CMAKE_SOURCE_DIR}/cmake/cpudetect.cpp
            RUN_OUTPUT_VARIABLE _cpu_out)
    if(_cpu_compile AND _cpu_run EQUAL 0)
      string(STRIP "${_cpu_out}" _detected)
    else()
      set(_detected generic)
    endif()
  else()
    set(_detected generic)
  endif()
  if(_arch STREQUAL "auto")
    set(_arch ${_detected})
  endif()
endif()
if(NOT _x86 AND NOT _arch STREQUAL "generic")
  message(STATUS "Non-x86 target: falling back to the generic build")
  set(_arch generic)
endif()
message(STATUS "Tessera instruction set: ${_arch}")

set(TESSERA_ARCH_DEFS "")
set(TESSERA_ARCH_FLAGS "")
if(_arch STREQUAL "native")
  list(APPEND TESSERA_ARCH_DEFS USE_POPCNT)
  if(_detected STREQUAL "bmi2")
    list(APPEND TESSERA_ARCH_DEFS USE_PEXT)
  endif()
  if(NOT MSVC)
    list(APPEND TESSERA_ARCH_FLAGS -march=native)
  else()
    list(APPEND TESSERA_ARCH_FLAGS /arch:AVX2)
  endif()
elseif(_arch STREQUAL "bmi2")
  list(APPEND TESSERA_ARCH_DEFS USE_POPCNT USE_PEXT)
  if(MSVC)
    list(APPEND TESSERA_ARCH_FLAGS /arch:AVX2)
  else()
    list(APPEND TESSERA_ARCH_FLAGS -msse4.2 -mpopcnt -mbmi -mbmi2)
  endif()
elseif(_arch STREQUAL "popcnt")
  list(APPEND TESSERA_ARCH_DEFS USE_POPCNT)
  if(NOT MSVC)
    list(APPEND TESSERA_ARCH_FLAGS -msse4.2 -mpopcnt)
  endif()
endif()

# ---------------------------------------------------------------------------
# Common flags
set(TESSERA_WARN_FLAGS "")
if(MSVC)
  list(APPEND TESSERA_WARN_FLAGS /W3 /permissive- /Zc:__cplusplus)
  set(TESSERA_OPT_FLAGS $<$<CONFIG:Release>:/O2 /Oi /GL>)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX)
else()
  list(APPEND TESSERA_WARN_FLAGS -Wall -Wextra -Wshadow -Wno-unused-parameter)
  set(TESSERA_OPT_FLAGS $<$<CONFIG:Release>:-O3 -fno-plt> $<$<CONFIG:RelWithDebInfo>:-O2 -g>)
endif()

if(TESSERA_SANITIZE AND NOT MSVC)
  set(_san_flags -fsanitize=${TESSERA_SANITIZE} -fno-omit-frame-pointer -g)
  add_compile_options(${_san_flags})
  add_link_options(-fsanitize=${TESSERA_SANITIZE})
endif()

# PGO
if(NOT MSVC)
  if(TESSERA_PGO STREQUAL "generate")
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
      add_compile_options(-fprofile-instr-generate=${TESSERA_PGO_DIR}/tessera-%p.profraw)
      add_link_options(-fprofile-instr-generate)
    else()
      add_compile_options(-fprofile-generate -fprofile-dir=${TESSERA_PGO_DIR} -fprofile-update=atomic)
      add_link_options(-fprofile-generate)
    endif()
  elseif(TESSERA_PGO STREQUAL "use")
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
      add_compile_options(-fprofile-instr-use=${TESSERA_PGO_DIR}/tessera.profdata -Wno-profile-instr-unprofiled
                          -Wno-profile-instr-out-of-date)
    else()
      add_compile_options(-fprofile-use -fprofile-dir=${TESSERA_PGO_DIR} -fprofile-correction -Wno-missing-profile)
    endif()
  endif()
endif()

include(CheckIPOSupported)
set(_ipo OFF)
if(TESSERA_LTO AND NOT TESSERA_SANITIZE)
  check_ipo_supported(RESULT _ipo OUTPUT _ipo_msg)
endif()

find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# Targets
file(GLOB TESSERA_CORE_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/*.cpp)
list(REMOVE_ITEM TESSERA_CORE_SOURCES ${CMAKE_SOURCE_DIR}/src/main.cpp)

add_library(tessera_core STATIC ${TESSERA_CORE_SOURCES})
target_include_directories(tessera_core PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(tessera_core PUBLIC ${TESSERA_ARCH_DEFS})
target_compile_options(tessera_core PUBLIC ${TESSERA_ARCH_FLAGS} ${TESSERA_WARN_FLAGS} ${TESSERA_OPT_FLAGS})
target_link_libraries(tessera_core PUBLIC Threads::Threads)

# Compiled without -m flags so it can report a missing instruction set instead of crashing
add_library(tessera_cpucheck STATIC ${CMAKE_SOURCE_DIR}/src/platform/cpu_check.cpp)
target_compile_definitions(tessera_cpucheck PRIVATE ${TESSERA_ARCH_DEFS})

add_executable(tessera ${CMAKE_SOURCE_DIR}/src/main.cpp)
target_link_libraries(tessera PRIVATE tessera_cpucheck tessera_core)

add_executable(tessera_tests ${CMAKE_SOURCE_DIR}/tests/tests.cpp)
target_link_libraries(tessera_tests PRIVATE tessera_core)

add_executable(tessera_tune ${CMAKE_SOURCE_DIR}/tools/tune.cpp ${CMAKE_SOURCE_DIR}/tools/datagen.cpp)
target_link_libraries(tessera_tune PRIVATE tessera_core)

set(_tessera_ipo_targets tessera_core tessera tessera_tests tessera_tune)

# Match runner / opening generator (POSIX only: uses fork and pipes)
if(NOT WIN32)
  add_executable(tessera_match ${CMAKE_SOURCE_DIR}/tools/match.cpp)
  target_link_libraries(tessera_match PRIVATE tessera_core)
  list(APPEND _tessera_ipo_targets tessera_match)
endif()

if(_ipo)
  set_property(TARGET ${_tessera_ipo_targets} PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
endif()

# ---------------------------------------------------------------------------
# Tests
enable_testing()
add_test(NAME unit COMMAND tessera_tests unit)
add_test(NAME perft COMMAND tessera_tests perft)
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
  add_test(NAME uci COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/uci_test.py $<TARGET_FILE:tessera>)
  add_test(NAME movegen_crosscheck COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/crosscheck.py
           $<TARGET_FILE:tessera_tests>)
endif()
