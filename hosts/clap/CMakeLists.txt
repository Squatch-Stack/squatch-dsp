# SPDX-License-Identifier: MIT
# The desktop plugins: every block named in generated/plugins.cmake (from contracts/) as CLAP,
# and through clap-wrapper as VST3 and AUv2. Dependencies are the pinned checkouts that
# tools/fetch-plugin-deps puts in build/deps; nothing is downloaded here.
#
#   cmake -S hosts/clap -B build/plugins -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build build/plugins            # bundles in build/plugins/bundles
#
# SQUATCH_WRAPPERS=OFF builds only the plugin libraries and the headless test host, which is how
# the sanitizer legs build (make plugins-check).
cmake_minimum_required(VERSION 3.21)
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "oldest macOS the plugins load on")
project(squatch-plugins VERSION 0.1.0 LANGUAGES C CXX)
if(APPLE)
  enable_language(OBJC OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

get_filename_component(SQUATCH_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(SQUATCH_DEPS "${SQUATCH_ROOT}/build/deps" CACHE PATH "pinned dependencies (tools/fetch-plugin-deps)")
option(SQUATCH_WRAPPERS "build CLAP, VST3 and AUv2 bundles through clap-wrapper" ON)
set(SQUATCH_FORMATS CLAP VST3 AUV2 CACHE STRING "clap-wrapper formats")
set(SQUATCH_WARN -Wall -Wextra -Wpedantic -Wshadow -Werror)
if(APPLE)
  # With a macOS 11 target the linker emits lazy binding: the first call to, say, powf from the
  # audio thread would run dyld's binder, which takes a lock (the test host's lock watch caught
  # it). Bind everything when the plugin loads instead.
  add_link_options(-Wl,-bind_at_load)
endif()

foreach(dep clap clap-wrapper)
  if(NOT EXISTS "${SQUATCH_DEPS}/${dep}")
    message(FATAL_ERROR "${SQUATCH_DEPS}/${dep} is missing: run tools/fetch-plugin-deps")
  endif()
endforeach()

# The core, exactly as the targets build it: no exceptions, no RTTI.
file(GLOB SQUATCH_CORE_SOURCES "${SQUATCH_ROOT}/src/*.cpp")
add_library(squatch-dsp-core STATIC ${SQUATCH_CORE_SOURCES})
target_include_directories(squatch-dsp-core PUBLIC "${SQUATCH_ROOT}/include" "${SQUATCH_ROOT}/blocks/tuner/include")
target_compile_options(squatch-dsp-core PRIVATE -fno-exceptions -fno-rtti ${SQUATCH_WARN})

if(SQUATCH_WRAPPERS)
  set(CLAP_SDK_ROOT "${SQUATCH_DEPS}/clap" CACHE PATH "" FORCE)
  set(VST3_SDK_ROOT "${SQUATCH_DEPS}/vst3sdk" CACHE PATH "" FORCE)
  set(AUDIOUNIT_SDK_ROOT "${SQUATCH_DEPS}/AudioUnitSDK" CACHE PATH "" FORCE)
  set(CLAP_WRAPPER_DOWNLOAD_DEPENDENCIES OFF CACHE BOOL "" FORCE)
  add_subdirectory("${SQUATCH_DEPS}/clap-wrapper" clap-wrapper EXCLUDE_FROM_ALL)
else()
  add_library(clap INTERFACE)
  target_include_directories(clap INTERFACE "${SQUATCH_DEPS}/clap/include")
  add_library(clap-wrapper-extensions INTERFACE)
  target_include_directories(clap-wrapper-extensions INTERFACE "${SQUATCH_DEPS}/clap-wrapper/include")
endif()

set(SQUATCH_PLUGIN_KEYS "")
function(squatch_plugin key name id version subtype)
  set(impl squatch-${key}-impl)
  add_library(${impl} STATIC "${CMAKE_CURRENT_SOURCE_DIR}/generated/${key}.cpp")
  target_link_libraries(${impl} PUBLIC squatch-dsp-core clap clap-wrapper-extensions)
  target_compile_options(${impl} PRIVATE -fno-rtti ${SQUATCH_WARN})
  set(SQUATCH_PLUGIN_KEYS ${SQUATCH_PLUGIN_KEYS} ${key} PARENT_SCOPE)
  if(NOT SQUATCH_WRAPPERS)
    return()
  endif()
  set_property(GLOBAL APPEND_STRING PROPERTY SQUATCH_BUNDLES "${key}=${name}|")
  set(KEY ${key})
  configure_file("${CMAKE_CURRENT_SOURCE_DIR}/entry.cpp.in" "${CMAKE_BINARY_DIR}/entry/${key}.cpp" @ONLY)
  make_clapfirst_plugins(
    TARGET_NAME squatch-${key}
    IMPL_TARGET ${impl}
    OUTPUT_NAME "${name}"
    ENTRY_SOURCE "${CMAKE_BINARY_DIR}/entry/${key}.cpp"
    BUNDLE_IDENTIFIER "${id}"
    BUNDLE_VERSION "${version}"
    COPY_AFTER_BUILD FALSE
    PLUGIN_FORMATS ${SQUATCH_FORMATS}
    ASSET_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bundles"
    AUV2_MANUFACTURER_NAME "Squatch Stack"
    AUV2_MANUFACTURER_CODE "SqSt"
    AUV2_SUBTYPE_CODE "${subtype}"
    AUV2_INSTRUMENT_TYPE "aufx")
endfunction()
include("${CMAKE_CURRENT_SOURCE_DIR}/generated/plugins.cmake")
# Plugins of blocks that live only in the development tree (tools/private-paths); the public
# export has no such file.
include("${CMAKE_CURRENT_SOURCE_DIR}/generated/plugins-private.cmake" OPTIONAL)

if(SQUATCH_WRAPPERS AND APPLE)
  get_property(bundles GLOBAL PROPERTY SQUATCH_BUNDLES)
  set(format_targets "")
  foreach(key ${SQUATCH_PLUGIN_KEYS})
    list(APPEND format_targets squatch-${key}_all)
  endforeach()
  add_custom_target(squatch-finish-bundles ALL
    COMMAND "${CMAKE_COMMAND}" "-DBUNDLES=${CMAKE_BINARY_DIR}/bundles" "-DBUILD=${CMAKE_BINARY_DIR}" "-DPLUGINS=${bundles}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/finish-bundles.cmake"
    DEPENDS ${format_targets}
    COMMENT "AU Info.plist and ad hoc signatures"
    VERBATIM)
endif()

# The headless host (tests/): every plugin linked in, plus a mode that loads built bundles. The
# list of linked plugins is written here; tests/extra_*.cpp add cases for one plugin each.
set(linked "")
foreach(key ${SQUATCH_PLUGIN_KEYS})
  string(APPEND linked "SQUATCH_LINKED(${key})\n")
endforeach()
file(CONFIGURE OUTPUT "${CMAKE_BINARY_DIR}/generated/linked_plugins.inc" CONTENT "${linked}")
file(GLOB SQUATCH_EXTRA_CASES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/extra_*.cpp")
add_executable(squatch-clap-host tests/harness.cpp tests/host.cpp tests/cases.cpp tests/cases_audio.cpp tests/cases_threads.cpp
               ${SQUATCH_EXTRA_CASES} "${SQUATCH_ROOT}/tests/rt_watch.cpp")
target_include_directories(squatch-clap-host PRIVATE "${SQUATCH_ROOT}/tests" "${CMAKE_BINARY_DIR}/generated")
target_compile_options(squatch-clap-host PRIVATE ${SQUATCH_WARN})
foreach(key ${SQUATCH_PLUGIN_KEYS})
  target_link_libraries(squatch-clap-host PRIVATE squatch-${key}-impl)
endforeach()
if(APPLE)
  add_library(lockcount SHARED "${SQUATCH_ROOT}/tests/lockcount.c")
  target_link_libraries(squatch-clap-host PRIVATE lockcount ${CMAKE_DL_LIBS})
else()
  target_compile_definitions(squatch-clap-host PRIVATE SQUATCH_NO_LOCK_COUNT)
  target_link_libraries(squatch-clap-host PRIVATE ${CMAKE_DL_LIBS} pthread)
endif()
