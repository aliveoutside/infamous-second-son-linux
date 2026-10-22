# SPDX-License-Identifier: GPL-2.0-or-later
# Build of the loader and PS4 runtime (iss-probe) and its tests. The graphics library (libiss-gpu.so) is gpu/CMakeLists.txt.
#
#   python3 scripts/prepare.py --game <game folder>; python3 scripts/link_modules.py --game <game folder>
#   cmake -S . -B build -G Ninja && cmake --build build
cmake_minimum_required(VERSION 3.20)
project(iss_port C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

set(ISS_SHADPS4_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/shadPS4" CACHE PATH "shadPS4 sources (git submodule)")
option(ISS_BUILD_GPU "Build libiss-gpu.so" OFF)

# PS4 import NID -> API name table, from shadPS4's aerolib (no game files).
set(ISS_AEROLIB "${ISS_SHADPS4_DIR}/src/core/aerolib/aerolib.inl")
add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/generated/api_names.h"
  COMMAND python3 "${CMAKE_CURRENT_SOURCE_DIR}/scripts/linux/gen_api_names.py" "${ISS_AEROLIB}"
          "${CMAKE_BINARY_DIR}/generated/api_names.h"
  DEPENDS "${ISS_AEROLIB}" scripts/linux/gen_api_names.py)
add_custom_target(iss-api-names DEPENDS "${CMAKE_BINARY_DIR}/generated/api_names.h")

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL3 REQUIRED IMPORTED_TARGET sdl3)

# decodes the guest code for the fs: -> gs: rewrite.
set(ZYDIS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_DOXYGEN OFF CACHE BOOL "" FORCE)
set(ZYAN_ZYCORE_PATH "${ISS_SHADPS4_DIR}/externals/zydis/dependencies/zycore" CACHE PATH "" FORCE)
add_subdirectory("${ISS_SHADPS4_DIR}/externals/zydis" "${CMAKE_BINARY_DIR}/zydis" EXCLUDE_FROM_ALL)

set(ISS_RUNTIME_SOURCES
  src/posix/probe.c src/runtime.c src/posix/memory.c src/sync.c src/kernel_sync.c src/posix/threads.c
  src/posix/time.c src/posix/files.c src/services.c src/dialogs.c src/save_dialog.c src/np.c src/network.c
  src/posix/http.c src/saves.c src/trophies.c src/pad.c src/posix/pad_panel.c src/audio.c src/gpu.c
  src/content.c src/posix/tcb_patch.c src/posix/native_hooks.cpp native/ultrawide.cpp native/mouse_look.cpp native/graffiti.cpp
  src/settings.c)
set(ISS_COMPILE_OPTIONS -Wall -Wextra -Wno-format -Wno-unused-parameter -Wno-sign-compare -Wno-missing-field-initializers
  -Wno-unused-function -Wno-unused-but-set-variable -Wno-unused-variable -Wno-misleading-indentation -Wno-maybe-uninitialized)

function(iss_runtime_target name main)
  set(sources ${ISS_RUNTIME_SOURCES})
  list(REMOVE_ITEM sources src/posix/probe.c)
  add_executable(${name} ${main} ${sources})
  target_compile_definitions(${name} PRIVATE _GNU_SOURCE)
  target_compile_options(${name} PRIVATE ${ISS_COMPILE_OPTIONS})
  target_include_directories(${name} PRIVATE src "${CMAKE_BINARY_DIR}/generated")
  add_dependencies(${name} iss-api-names)
  target_link_libraries(${name} PRIVATE PkgConfig::SDL3 Threads::Threads ${CMAKE_DL_LIBS} m)
  # The guest is a SysV x86-64 program; keep the host's executable position independent but
  # export nothing: the GPU library gets its callbacks through iss_gpu.h.
endfunction()

find_package(Threads REQUIRED)
iss_runtime_target(iss-probe src/posix/probe.c)

# The launcher (launcher/launcher.cpp): a settings form and PLAY, on SDL3 and Dear ImGui.
set(ISS_IMGUI_DIR "${ISS_SHADPS4_DIR}/externals/imgui")
add_executable(iss-launcher launcher/launcher.cpp src/settings.c
  "${ISS_IMGUI_DIR}/imgui.cpp" "${ISS_IMGUI_DIR}/imgui_draw.cpp" "${ISS_IMGUI_DIR}/imgui_tables.cpp"
  "${ISS_IMGUI_DIR}/imgui_widgets.cpp" "${ISS_IMGUI_DIR}/backends/imgui_impl_sdl3.cpp"
  "${ISS_IMGUI_DIR}/backends/imgui_impl_sdlrenderer3.cpp")
target_compile_definitions(iss-launcher PRIVATE _GNU_SOURCE)
target_include_directories(iss-launcher PRIVATE src "${ISS_IMGUI_DIR}" "${ISS_IMGUI_DIR}/backends"
  "${ISS_SHADPS4_DIR}/externals/stb")
target_link_libraries(iss-launcher PRIVATE PkgConfig::SDL3)

enable_testing()
add_executable(settings_test tests/settings_test.c src/settings.c)
target_include_directories(settings_test PRIVATE src)
add_test(NAME settings_test COMMAND settings_test)
foreach(test runtime_test sync_test host_lock_test save_dialog_test save_search_test pad_test)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/${test}.c")
    iss_runtime_target(${test} tests/${test}.c)
    add_test(NAME ${test} COMMAND ${test})
    if(test STREQUAL pad_test)
      # Only the test's virtual controllers count: a real one plugged in would be picked instead.
      set_tests_properties(pad_test PROPERTIES ENVIRONMENT "SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT=0x054c/0x09cc,0x045e/0x02ff")
    endif()
  endif()
endforeach()
add_executable(memory_clamp_cache_test tests/memory_clamp_cache_test.c)
target_compile_definitions(memory_clamp_cache_test PRIVATE _GNU_SOURCE)
target_include_directories(memory_clamp_cache_test PRIVATE src)
add_test(NAME memory_clamp_cache_test COMMAND memory_clamp_cache_test)

# Header-level renderer tests: each is one file, plus the source it tests where it has one.
set(ISS_CXX_TESTS
  buffer_read_memo_test descriptor_delta_test native_backing_alias_cache_test page_write_provenance_test
  readback_hint_test test_feedback_candidates test_feedback_identity_registry test_feedback_record
  test_g4_asc_owner_pause test_g4_conditional_backing_write test_g4_copy_hle_plan test_g4_cp_coordination
  test_g4_dma_fallback test_g4_graphics_control test_g4_packet_hazards test_g4_readback_writers
  test_g4_record_ownership test_g4_scheduler_timeline_bridge test_g4_sparse_residency
  test_g4_texture_negative_index test_graphics_pipeline_reuse test_sparse_copy test_srt_write_watch
  test_upload_parts texture_lookup_memo_test timeline_buffer_test
  "native_draw_run_test:gpu/native_draw_run.cpp" "native_instanced_run_test:gpu/native_instanced_run.cpp"
  "native_geometry_batch_test:gpu/native_geometry_batch.cpp"
  "native_instanced_batch_test:gpu/native_instanced_batch.cpp"
  "test_g4_fallback_packet:gpu/g4_fallback_packet.cpp" "test_g4_liverpool_handoff:gpu/g4_fallback_packet.cpp"
  "test_srt_walker_capture:gpu/srt_walker_capture.cpp" "visual_capture_test:gpu/visual_capture.cpp")
foreach(entry IN LISTS ISS_CXX_TESTS)
  string(REPLACE ":" ";" parts "${entry}")
  list(POP_FRONT parts test)
  add_executable(${test} tests/${test}.cpp ${parts})
  set_target_properties(${test} PROPERTIES CXX_STANDARD 23)
  target_compile_definitions(${test} PRIVATE _GNU_SOURCE)
  target_compile_options(${test} PRIVATE -w -UNDEBUG -march=x86-64-v3 "SHELL:-include mutex" "SHELL:-include cstring"
    "SHELL:-include ${CMAKE_CURRENT_SOURCE_DIR}/gpu/posix/win32_compat.h")
  # fmt and Boost are shadPS4's bundled copies (fmt header-only): the tests need neither installed.
  target_include_directories(${test} PRIVATE gpu/posix gpu/shim gpu gpu/infamous_renderer native
    "${ISS_SHADPS4_DIR}/src" "${ISS_SHADPS4_DIR}/externals/fmt/include" "${ISS_SHADPS4_DIR}/externals/ext-boost")
  target_compile_definitions(${test} PRIVATE FMT_HEADER_ONLY)
  target_link_libraries(${test} PRIVATE Threads::Threads)
  add_test(NAME ${test} COMMAND ${test})
endforeach()
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
  add_test(NAME link_modules_test COMMAND ${Python3_EXECUTABLE} -m unittest tests/test_link_modules.py
           WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endif()

if(ISS_BUILD_GPU)
  add_subdirectory(gpu)
endif()
