# SPDX-License-Identifier: GPL-2.0-or-later
# libiss-gpu.so: the shadPS4 renderer (video_core, shader_recompiler, GnmDriver, VideoOut, AJM,
# Videodec) with this port's overrides. The shadPS4 libraries it links (fmt, spdlog, sirit, SDL3,
# ...) come from a shadPS4 build in <build>/shadps4, configured here on the first run and built
# with this target; scripts/linux/shadps4_flags.py reads its flags and libraries.
set(ISS_SHADPS4_SRC "${ISS_SHADPS4_DIR}/src")
set(ISS_SHADPS4_BUILD "${CMAKE_BINARY_DIR}/shadps4" CACHE PATH "shadPS4 build folder")
if(NOT EXISTS "${ISS_SHADPS4_BUILD}/build.ninja")
  # The options of the tested build: release, clang when installed, no emulator GUI or extras.
  find_program(ISS_CLANG clang)
  find_program(ISS_CLANGXX clang++)
  set(compilers "")
  if(ISS_CLANG AND ISS_CLANGXX)
    set(compilers -DCMAKE_C_COMPILER=${ISS_CLANG} -DCMAKE_CXX_COMPILER=${ISS_CLANGXX})
  endif()
  message(STATUS "Configuring shadPS4 in ${ISS_SHADPS4_BUILD}")
  execute_process(COMMAND ${CMAKE_COMMAND} -S ${ISS_SHADPS4_DIR} -B ${ISS_SHADPS4_BUILD} -G Ninja
                    -DCMAKE_BUILD_TYPE=Release ${compilers} -DENABLE_QT_GUI=OFF -DENABLE_DISCORD_RPC=OFF
                    -DENABLE_UPDATER=OFF -DENABLE_TESTS=OFF
                  RESULT_VARIABLE shadps4_rc)
  if(shadps4_rc)
    message(FATAL_ERROR "configuring shadPS4 failed (is the submodule checked out? git submodule update --init --recursive)")
  endif()
endif()
set(flags_file "${CMAKE_CURRENT_BINARY_DIR}/shadps4_flags.cmake")
execute_process(COMMAND python3 ${CMAKE_SOURCE_DIR}/scripts/linux/shadps4_flags.py ${ISS_SHADPS4_BUILD} ${flags_file}
                RESULT_VARIABLE flags_rc)
if(flags_rc)
  message(FATAL_ERROR "cannot read the shadPS4 build in ${ISS_SHADPS4_BUILD}")
endif()
include(${flags_file})
find_program(ISS_NINJA ninja REQUIRED)
add_custom_target(shadps4-libraries
  # The emulator target itself: it also generates the headers the renderer includes (host
  # shaders, fonts, protobuf). Its libraries are what libiss-gpu links.
  COMMAND ${ISS_NINJA} -C ${ISS_SHADPS4_BUILD} shadps4
  BYPRODUCTS ${SHADPS4_LIBRARY_FILES} USES_TERMINAL
  COMMENT "Building the shadPS4 libraries")

set(overrides
  "video_core/buffer_cache/buffer_cache.cpp" buffer_cache.cpp
  "video_core/renderer_vulkan/vk_rasterizer.cpp" vk_rasterizer.cpp
  "video_core/renderer_vulkan/vk_pipeline_cache.cpp" vk_pipeline_cache.cpp
  "video_core/renderer_vulkan/vk_graphics_pipeline.cpp" vk_graphics_pipeline.cpp
  "video_core/renderer_vulkan/vk_pipeline_serialization.cpp" vk_pipeline_serialization.cpp
  "video_core/renderer_vulkan/vk_runtime.cpp" vk_runtime.cpp
  "video_core/amdgpu/liverpool.cpp" liverpool.cpp
  "video_core/page_manager.cpp" page_manager.cpp
  "video_core/renderer_vulkan/vk_instance.cpp" vk_instance.cpp
  "video_core/renderer_vulkan/vk_resource_pool.cpp" vk_resource_pool.cpp
  "video_core/buffer_cache/buffer.cpp" buffer.cpp
  "video_core/renderer_vulkan/vk_presenter.cpp" vk_presenter.cpp
  "video_core/renderer_vulkan/vk_scheduler.cpp" vk_scheduler.cpp
  "video_core/renderer_vulkan/vk_staging_buffer_pool.cpp" vk_staging_buffer_pool.cpp
  "video_core/texture_cache/texture_cache.cpp" texture_cache.cpp
  "shader_recompiler/ir/passes/flatten_extended_userdata_pass.cpp" flatten_extended_userdata_pass.cpp
  "shader_recompiler/recompiler.cpp" recompiler.cpp
  "shader_recompiler/ir/passes/resource_patching_pass.cpp" resource_patching_pass.cpp
  "shader_recompiler/frontend/fetch_shader.cpp" fetch_shader.cpp
  "video_core/cache_storage.cpp" cache_storage.cpp
  "video_core/renderer_vulkan/vk_swapchain.cpp" vk_swapchain.cpp
  "video_core/texture_cache/image.cpp" image.cpp)
list(LENGTH overrides override_count)
file(GLOB_RECURSE shadps4_sources "${ISS_SHADPS4_SRC}/video_core/*.cpp" "${ISS_SHADPS4_SRC}/shader_recompiler/*.cpp")
set(sources "")
foreach(source IN LISTS shadps4_sources)
  file(RELATIVE_PATH rel "${ISS_SHADPS4_SRC}" "${source}")
  if(rel STREQUAL "video_core/renderdoc.cpp")
    continue()
  endif()
  set(replacement "")
  math(EXPR last "${override_count} - 2")
  foreach(i RANGE 0 ${last} 2)
    list(GET overrides ${i} key)
    if(key STREQUAL rel)
      math(EXPR j "${i} + 1")
      list(GET overrides ${j} replacement)
    endif()
  endforeach()
  if(replacement)
    list(APPEND sources "${CMAKE_CURRENT_SOURCE_DIR}/overrides/${replacement}")
  else()
    list(APPEND sources "${source}")
  endif()
endforeach()
foreach(n decoder error io_file native_clock number_utils rdtsc spin_lock recursive_lock string_util thread signal_context)
  list(APPEND sources "${ISS_SHADPS4_SRC}/common/${n}.cpp")
endforeach()
list(APPEND sources
  "${ISS_SHADPS4_SRC}/common/logging/classes.cpp"
  ${CMAKE_CURRENT_SOURCE_DIR}/overrides/gnmdriver.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/overrides/videoout_driver.cpp
  "${ISS_SHADPS4_SRC}/core/libraries/videoout/video_out.cpp"
  "${ISS_SHADPS4_SRC}/core/libraries/kernel/equeue.cpp"
  "${ISS_SHADPS4_SRC}/imgui/renderer/imgui_impl_vulkan.cpp"
  "${ISS_SHADPS4_SRC}/imgui/renderer/texture_manager.cpp"
  "${ISS_SHADPS4_SRC}/common/stb.cpp"
  "${ISS_SHADPS4_SRC}/core/libraries/videodec/videodec.cpp"
  "${ISS_SHADPS4_SRC}/core/libraries/videodec/videodec_impl.cpp"
  "${ISS_SHADPS4_SRC}/core/libraries/videodec/video_utils.cpp")
file(GLOB ajm_sources "${ISS_SHADPS4_SRC}/core/libraries/ajm/*.cpp")
file(GLOB glue_sources "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
list(APPEND sources ${ajm_sources} ${glue_sources})

add_library(iss-gpu SHARED ${sources})
set_target_properties(iss-gpu PROPERTIES OUTPUT_NAME iss-gpu PREFIX "lib" CXX_STANDARD 23 POSITION_INDEPENDENT_CODE ON)
target_compile_definitions(iss-gpu PRIVATE ${SHADPS4_DEFINES})
target_compile_options(iss-gpu PRIVATE ${SHADPS4_FLAGS} -include ${CMAKE_CURRENT_SOURCE_DIR}/posix/win32_compat.h
  -Wno-deprecated-declarations -Wno-invalid-offsetof -w -fPIC -ftls-model=initial-exec -fno-extern-tls-init
  # Nothing overrides the library's functions: GCC may inline them and calls skip the PLT.
  -fno-semantic-interposition)
# The shim headers come first so they shadow shadPS4's own files, as with the Windows build.
target_include_directories(iss-gpu BEFORE PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/shim ${CMAKE_CURRENT_SOURCE_DIR}/posix ${CMAKE_CURRENT_SOURCE_DIR}
  "${ISS_SHADPS4_SRC}/core/libraries/gnmdriver")
target_compile_options(iss-gpu PRIVATE ${SHADPS4_INCLUDES})
target_link_options(iss-gpu PRIVATE -Wl,-Bsymbolic-functions)
# Profile-guided and link-time optimization (the command processor thread is ~27% cheaper per
# draw with both). ISS_PGO=auto (default): use the profile in ISS_PGO_DIR when there is one.
# ISS_PGO=generate: an instrumented library that writes its profile there every 20 s (the game
# ends without exit handlers); play the slow scenes for a few minutes, then reconfigure with
# auto. Functions changed since the profile compile without it; retrain after larger changes.
set(ISS_PGO "auto" CACHE STRING "Profile-guided optimization: auto, off, generate or use")
set(ISS_PGO_DIR "${CMAKE_SOURCE_DIR}/pgo" CACHE PATH "Profile directory")
option(ISS_LTO "Link-time optimization of libiss-gpu" ON)
string(REGEX MATCH "^[0-9]+" gcc_major "${CMAKE_CXX_COMPILER_VERSION}")
if(ISS_PGO STREQUAL "auto")
  file(GLOB_RECURSE iss_pgo_files "${ISS_PGO_DIR}/*.gcda")
  # A profile only fits the GCC major version that trained it (pgo/gcc-version).
  set(pgo_major "")
  if(EXISTS "${ISS_PGO_DIR}/gcc-version")
    file(STRINGS "${ISS_PGO_DIR}/gcc-version" pgo_major LIMIT_COUNT 1)
  endif()
  if(iss_pgo_files AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND pgo_major STREQUAL gcc_major)
    set(ISS_PGO "use")
  elseif(iss_pgo_files)
    message(STATUS "libiss-gpu PGO: the profile is for GCC ${pgo_major}, this is "
      "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}: building without it")
  endif()
  message(STATUS "libiss-gpu PGO: ${ISS_PGO} (${ISS_PGO_DIR})")
endif()
# The profile files are named after the object files. -fprofile-prefix-path keeps those names
# relative (gpu#CMakeFiles#... for the port, src#... for the shadPS4 sources), so the profile in
# the repository works from any checkout and build folder.
if(ISS_PGO STREQUAL "generate" OR ISS_PGO STREQUAL "use")
  target_compile_options(iss-gpu PRIVATE -fprofile-prefix-path=${CMAKE_BINARY_DIR})
  get_filename_component(shadps4_root "${ISS_SHADPS4_DIR}" ABSOLUTE)
  # CMake names the objects of a source in the project tree by its path relative to this folder
  # (".." becomes "__"), and of a source outside by its absolute path.
  file(RELATIVE_PATH shadps4_rel "${CMAKE_SOURCE_DIR}" "${shadps4_root}")
  if(shadps4_rel MATCHES "^\\.\\.")
    set(shadps4_prefix "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/iss-gpu.dir${shadps4_root}")
  else()
    file(RELATIVE_PATH shadps4_rel "${CMAKE_CURRENT_SOURCE_DIR}" "${shadps4_root}")
    string(REPLACE "../" "__/" shadps4_rel "${shadps4_rel}")
    set(shadps4_prefix "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/iss-gpu.dir/${shadps4_rel}")
  endif()
  foreach(source IN LISTS sources)
    get_filename_component(source_abs "${source}" ABSOLUTE)
    string(FIND "${source_abs}" "${shadps4_root}/" at)
    if(at EQUAL 0)
      set_property(SOURCE "${source}" APPEND PROPERTY COMPILE_OPTIONS -fprofile-prefix-path=${shadps4_prefix})
    endif()
  endforeach()
endif()
if(ISS_PGO STREQUAL "generate")
  file(WRITE "${ISS_PGO_DIR}/gcc-version" "${gcc_major}\n")
  target_compile_options(iss-gpu PRIVATE -fprofile-generate=${ISS_PGO_DIR} -fprofile-update=atomic)
  target_link_options(iss-gpu PRIVATE -fprofile-generate=${ISS_PGO_DIR})
  target_compile_definitions(iss-gpu PRIVATE ISS_PGO_GENERATE=1)
elseif(ISS_PGO STREQUAL "use")
  target_compile_options(iss-gpu PRIVATE -fprofile-use=${ISS_PGO_DIR} -fprofile-partial-training
    -fprofile-correction -Wno-missing-profile -Wno-coverage-mismatch)
endif()
if(ISS_LTO)
  target_compile_options(iss-gpu PRIVATE -flto=auto)
  target_link_options(iss-gpu PRIVATE -flto=auto)
endif()
add_dependencies(iss-gpu shadps4-libraries)
target_link_libraries(iss-gpu PRIVATE -Wl,--start-group ${SHADPS4_LIBRARIES} -Wl,--end-group pthread dl vulkan tbb)
