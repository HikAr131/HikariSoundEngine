# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Hikari
cmake_minimum_required(VERSION 3.25)
cmake_policy(SET CMP0091 NEW)
project(HikariSoundEngine VERSION 1.0.1 LANGUAGES C CXX RC)
if(NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "MSVC v143 x64 is required")
endif()
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreaded)
set(CMAKE_C_FLAGS_RELEASE "/O2 /Ob2 /DNDEBUG")
set(CMAKE_CXX_FLAGS_RELEASE "/O2 /Ob2 /DNDEBUG")
set(UPSTREAM "${CMAKE_SOURCE_DIR}/build/upstream")

function(upstream_library target project_file)
  file(READ "${project_file}" xml)
  string(REGEX MATCHALL "<ClCompile Include=\"[^\"]+\"" entries "${xml}")
  get_filename_component(base "${project_file}" DIRECTORY)
  set(sources)
  foreach(entry IN LISTS entries)
    string(REGEX REPLACE "<ClCompile Include=\"([^\"]+)\"" "\\1" source "${entry}")
    string(REPLACE "\\" "/" source "${source}")
    list(APPEND sources "${base}/${source}")
  endforeach()
  add_library(${target} STATIC ${sources})
  target_compile_options(${target} PRIVATE /Brepro /utf-8 /W0 /wd4996 /EHsc)
  target_compile_definitions(${target} PRIVATE UNICODE _UNICODE NDEBUG _LIB
    _CRT_SECURE_NO_WARNINGS _CRT_NON_CONFORMING_SWPRINTFS)
  if(target STREQUAL "fxsound_dsp")
    target_include_directories(${target} PRIVATE "${UPSTREAM}/dsp/ptutil/include"
      "${UPSTREAM}/dsp/include" "${UPSTREAM}/audiopassthru/include" "${CMAKE_SOURCE_DIR}/tools")
  else()
    target_include_directories(${target} PRIVATE "${UPSTREAM}/audiopassthru/include"
      "${UPSTREAM}/dsp/include" "${UPSTREAM}/dsp/ptutil/include" "${CMAKE_SOURCE_DIR}/tools")
  endif()
endfunction()

upstream_library(fxsound_dsp "${UPSTREAM}/dsp/DfxDsp.vcxproj")
target_compile_definitions(fxsound_dsp PRIVATE PT_NON_MFC DSPSOFT_TARGET PT_DSP_BUILD=PT_DSP_DFX)
upstream_library(fxsound_audio "${UPSTREAM}/audiopassthru/audiopassthru.vcxproj")

file(GLOB HOST_SOURCES CONFIGURE_DEPENDS "src/*.cpp" "tests/*.cpp")
add_executable(HikariSoundEngine WIN32 ${HOST_SOURCES} src/version.rc app.manifest)
target_compile_options(HikariSoundEngine PRIVATE /Brepro /utf-8 /W4 /permissive- /EHsc)
target_compile_definitions(HikariSoundEngine PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(HikariSoundEngine PRIVATE src tools
  "${UPSTREAM}/dsp/include" "${UPSTREAM}/audiopassthru/include")
target_link_libraries(HikariSoundEngine PRIVATE fxsound_audio fxsound_dsp
  ole32 oleaut32 uuid avrt winmm advapi32 propsys shell32 shlwapi user32 wtsapi32 bcrypt cfgmgr32 delayimp)
# Static imports stay KnownDLLs; the rest load lazily after main restricts the DLL search to System32.
target_link_options(HikariSoundEngine PRIVATE /Brepro /DEBUG:NONE /INCREMENTAL:NO /OPT:REF /OPT:ICF
  /DELAYLOAD:cfgmgr32.dll /DELAYLOAD:wtsapi32.dll
  "/MANIFESTINPUT:${CMAKE_SOURCE_DIR}/app.manifest")
set_target_properties(HikariSoundEngine PROPERTIES VS_GLOBAL_EnableManagedIncrementalBuild false)

add_executable(HikariDspSelfTest EXCLUDE_FROM_ALL tools/dsp-selftest-main.cpp
  src/dsp_adapter.cpp src/eq_stage.cpp src/parameters.cpp src/parametric_eq.cpp src/graphic_eq.cpp tests/dsp_tests.cpp)
target_compile_options(HikariDspSelfTest PRIVATE /Brepro /utf-8 /W4 /permissive- /EHsc)
target_compile_definitions(HikariDspSelfTest PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(HikariDspSelfTest PRIVATE src tools
  "${UPSTREAM}/dsp/include" "${UPSTREAM}/audiopassthru/include")
target_link_libraries(HikariDspSelfTest PRIVATE fxsound_audio fxsound_dsp
  ole32 oleaut32 uuid winmm advapi32 propsys shell32 shlwapi user32)
target_link_options(HikariDspSelfTest PRIVATE /Brepro /DEBUG:NONE /INCREMENTAL:NO /OPT:REF /OPT:ICF)
