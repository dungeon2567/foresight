cmake_minimum_required(VERSION 3.21)

# -------------------------
# Policy setup
# -------------------------
if (POLICY CMP0141)
    cmake_policy(SET CMP0141 NEW)

    set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT
        "$<IF:$<C_COMPILER_ID:MSVC>,$<$<CONFIG:Debug,RelWithDebInfo>:EditAndContinue>,$<$<CONFIG:Debug,RelWithDebInfo>:ProgramDatabase>>"
    )
endif()

# -------------------------
# Force Clang on Windows
# -------------------------
# MSVC C11 atomics are incomplete; prefer clang-cl
if (WIN32)

    if (CMAKE_GENERATOR MATCHES "Visual Studio")
        if (NOT CMAKE_GENERATOR_TOOLSET MATCHES "ClangCL")
            set(CMAKE_GENERATOR_TOOLSET "ClangCL"
                CACHE STRING "Force ClangCL toolset" FORCE
            )
        endif()

    elseif (NOT DEFINED CMAKE_C_COMPILER)
        find_program(CLANG_C_BIN NAMES clang-cl clang)

        if (CLANG_C_BIN)
            set(CMAKE_C_COMPILER "${CLANG_C_BIN}"
                CACHE FILEPATH "Force clang on Windows" FORCE
            )
        else()
            message(FATAL_ERROR
                "clang/clang-cl not found in PATH. Install LLVM or pass -T ClangCL."
            )
        endif()
    endif()

endif()

# -------------------------
# Project
# -------------------------
project(ecs C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# -------------------------
# mimalloc (Release-only allocator)
#   Debug builds use libc malloc so Valgrind / ASan / debugging
#   tools see real allocator state -- mimalloc's internal pool
#   layout confuses memcheck and obscures use-after-free reports.
# -------------------------
include(FetchContent)
if (NOT DEFINED CMAKE_BUILD_TYPE OR CMAKE_BUILD_TYPE STREQUAL "")
    set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "" FORCE)
endif()

if (CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(ECS_USE_MIMALLOC FALSE)
else()
    set(ECS_USE_MIMALLOC TRUE)
endif()

if (ECS_USE_MIMALLOC)
    set(MI_OVERRIDE        OFF CACHE BOOL "" FORCE)
    set(MI_BUILD_SHARED    OFF CACHE BOOL "" FORCE)
    set(MI_BUILD_OBJECT    OFF CACHE BOOL "" FORCE)
    set(MI_BUILD_TESTS     OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        mimalloc
        GIT_REPOSITORY https://github.com/microsoft/mimalloc.git
        GIT_TAG        v2.1.7
    )
    FetchContent_MakeAvailable(mimalloc)
endif()

# -------------------------
# zstd (static, used by serialize tests)
# -------------------------
set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_SHARED   OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_STATIC   ON  CACHE BOOL "" FORCE)
set(ZSTD_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(ZSTD_LEGACY_SUPPORT OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    zstd
    GIT_REPOSITORY https://github.com/facebook/zstd.git
    GIT_TAG        v1.5.6
    SOURCE_SUBDIR  build/cmake
)
FetchContent_MakeAvailable(zstd)

# -------------------------
# Target selection (gated by CMAKE_BUILD_TYPE — single-config generator)
#   Debug    → ecs       (tests, sanitizers, asserts)
#   Release  → ecs_bench (Google Benchmark only, no test runtime)
# Override either side with -DECS_FORCE_BUILD_TESTS=ON / -DECS_FORCE_BUILD_BENCH=ON.
# -------------------------
option(ECS_FORCE_BUILD_TESTS "Force-build the ecs (tests) target" OFF)
option(ECS_FORCE_BUILD_BENCH "Force-build the ecs_bench target"   OFF)

if (CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(ECS_BUILD_TESTS TRUE)
    set(ECS_BUILD_BENCH ${ECS_FORCE_BUILD_BENCH})
else()
    set(ECS_BUILD_TESTS ${ECS_FORCE_BUILD_TESTS})
    set(ECS_BUILD_BENCH TRUE)
endif()

# -------------------------
# Executables
# -------------------------
if (ECS_BUILD_TESTS)
    add_executable(ecs
        main.c
        src/ecs_crc64.c
        src/ecs_tree.c
        src/ecs_world.c
        src/ecs_query.c
        src/ecs_broadphase.c
        src/ecs_input.c
        # Scripting layer (runtime + VM + query evaluator).
        src/ecs_script.c
        src/ecs_script_query.c
        src/ecs_vm.c
        src/ecs_effect.c
        src/ecs_formula.c
        # Compile-time pipeline: source → AST → blob.
        src/compiler/ecs_compiler.c
        src/compiler/ecs_lexer.c
        src/compiler/ecs_parser.c
        src/compiler/ecs_schema.c
        src/compiler/ecs_codegen.c
    )

    target_include_directories(ecs PRIVATE
        src
        src/compiler
        tests
        ${zstd_SOURCE_DIR}/lib
    )

    # Path to .script samples for test_script_compile.h.
    target_compile_definitions(ecs PRIVATE
        SCRIPTS_DIR="${CMAKE_SOURCE_DIR}/scripts"
    )

    if (ECS_USE_MIMALLOC)
        target_link_libraries(ecs PRIVATE mimalloc-static libzstd_static)
    else()
        target_compile_definitions(ecs PRIVATE ECS_NO_MIMALLOC)
        target_link_libraries(ecs PRIVATE libzstd_static)
    endif()
endif()

if (ECS_BUILD_BENCH)
    set(BENCHMARK_ENABLE_TESTING     OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_INSTALL     OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_WERROR      OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        googlebenchmark
        GIT_REPOSITORY https://github.com/google/benchmark.git
        GIT_TAG        v1.9.0
    )
    FetchContent_MakeAvailable(googlebenchmark)

    add_executable(ecs_bench
        benchmarks/bench_main.cpp
        benchmarks/bench_integrate.c
        benchmarks/bench_broadphase.c
        src/ecs_crc64.c
        src/ecs_tree.c
        src/ecs_world.c
        src/ecs_query.c
        src/ecs_broadphase.c
    )

    target_include_directories(ecs_bench PRIVATE
        src
        benchmarks
    )

    if (ECS_USE_MIMALLOC)
        target_link_libraries(ecs_bench PRIVATE mimalloc-static benchmark::benchmark)
    else()
        target_compile_definitions(ecs_bench PRIVATE ECS_NO_MIMALLOC)
        target_link_libraries(ecs_bench PRIVATE benchmark::benchmark)
    endif()
endif()

# -------------------------
# Compiler options
# -------------------------
# ecs_fixed.h SIMD backend selects via arch macros: x86 → SSE4.1+AVX2, arm64 → NEON,
# else scalar. x86-only flags (/arch:AVX2, -msse4.1, -mavx2) must be gated.
if (CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64|i.86|x86)$")
    set(ECS_ARCH_X86 TRUE)
else()
    set(ECS_ARCH_X86 FALSE)
endif()

set(_ecs_targets)
if (ECS_BUILD_TESTS)
    list(APPEND _ecs_targets ecs)
endif()
if (ECS_BUILD_BENCH)
    list(APPEND _ecs_targets ecs_bench)
endif()

# -------------------------
# Optional: ecs_app — SDL3 + wgpu-native + Dear ImGui demo host.
#   Gated by -DECS_BUILD_APP=ON so test/bench builds stay lean.
#   Native-first; CMake is structured to add an Emscripten preset later
#   without touching the ecs / ecs_bench targets.
# -------------------------
option(ECS_BUILD_APP "Build the SDL3 + wgpu-native + ImGui demo (ecs_app)" OFF)

if (ECS_BUILD_APP)
    # ---- SDL3 (static) ---------------------------------------------------
    set(SDL_SHARED       OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC       ON  CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_TESTS        OFF CACHE BOOL "" FORCE)
    set(SDL_EXAMPLES     OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        SDL3
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG        release-3.2.0
    )
    FetchContent_MakeAvailable(SDL3)

    # ---- wgpu-native (prebuilt release archive) --------------------------
    # wgpu-native ships per-platform prebuilt zips with include/ and lib/.
    # Pin to a known-good tag matching the Dear ImGui wgpu backend below.
    set(_ECS_WGPU_TAG "v22.1.0.5")

    if (WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(_ECS_WGPU_URL "https://github.com/gfx-rs/wgpu-native/releases/download/${_ECS_WGPU_TAG}/wgpu-windows-x86_64-msvc-release.zip")
    elseif (APPLE)
        if (CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
            set(_ECS_WGPU_URL "https://github.com/gfx-rs/wgpu-native/releases/download/${_ECS_WGPU_TAG}/wgpu-macos-aarch64-release.zip")
        else()
            set(_ECS_WGPU_URL "https://github.com/gfx-rs/wgpu-native/releases/download/${_ECS_WGPU_TAG}/wgpu-macos-x86_64-release.zip")
        endif()
    elseif (UNIX)
        set(_ECS_WGPU_URL "https://github.com/gfx-rs/wgpu-native/releases/download/${_ECS_WGPU_TAG}/wgpu-linux-x86_64-release.zip")
    else()
        message(FATAL_ERROR "ECS_BUILD_APP: no wgpu-native prebuilt URL for this platform.")
    endif()

    FetchContent_Declare(
        wgpu_native
        URL ${_ECS_WGPU_URL}
    )
    FetchContent_MakeAvailable(wgpu_native)

    add_library(wgpu_native INTERFACE)
    # wgpu-native ships headers under include/webgpu/ and include/wgpu/.
    # wgpu.h does `#include "webgpu.h"` (no subdir prefix), so the inner
    # `webgpu/` dir must be on the include path too.
    target_include_directories(wgpu_native INTERFACE
        ${wgpu_native_SOURCE_DIR}/include
        ${wgpu_native_SOURCE_DIR}/include/webgpu
    )

    if (WIN32)
        # Import lib for the shipped DLL; copy DLL next to the exe at build time.
        target_link_libraries(wgpu_native INTERFACE
            ${wgpu_native_SOURCE_DIR}/lib/wgpu_native.dll.lib
            d3dcompiler
            user32 gdi32 opengl32
            ws2_32 userenv bcrypt ntdll
        )
    elseif (APPLE)
        target_link_libraries(wgpu_native INTERFACE
            ${wgpu_native_SOURCE_DIR}/lib/libwgpu_native.dylib
            "-framework Metal"
            "-framework QuartzCore"
            "-framework Foundation"
            "-framework Cocoa"
            "-framework IOKit"
        )
    else()
        target_link_libraries(wgpu_native INTERFACE
            ${wgpu_native_SOURCE_DIR}/lib/libwgpu_native.so
            ${CMAKE_DL_LIBS}
            m
        )
    endif()

    # ---- Dear ImGui (sources + SDL3 / wgpu backends) ---------------------
    FetchContent_Declare(
        imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG        v1.91.5-docking
    )
    FetchContent_MakeAvailable(imgui)

    add_library(imgui STATIC
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_demo.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_wgpu.cpp
    )
    target_include_directories(imgui PUBLIC
        ${imgui_SOURCE_DIR}
        ${imgui_SOURCE_DIR}/backends
    )
    # Tell imgui_impl_wgpu.cpp which webgpu.h dialect it's compiling against.
    target_compile_definitions(imgui PUBLIC IMGUI_IMPL_WEBGPU_BACKEND_WGPU)
    target_link_libraries(imgui PUBLIC SDL3::SDL3-static wgpu_native)

    # ---- ImGuiColorTextEdit (script editor: highlight + auto-indent) -----
    FetchContent_Declare(
        imgui_color_text_edit
        GIT_REPOSITORY https://github.com/BalazsJako/ImGuiColorTextEdit.git
        GIT_TAG        0a88824f7de8d0bd11d8419066caa7d3469395c4
    )
    FetchContent_MakeAvailable(imgui_color_text_edit)

    # ImGui 1.91 removed ImGui::GetKeyIndex() (it now lives only behind the
    # IMGUI_DISABLE_OBSOLETE_KEYIO opt-in). BalazsJako/ImGuiColorTextEdit is
    # frozen since 2021 and still wraps every key with it. Strip the wrapper
    # at configure time so the modern ImGuiKey_* values pass straight to
    # IsKeyPressed/IsKeyDown.
    set(_TE_SRC "${imgui_color_text_edit_SOURCE_DIR}/TextEditor.cpp")
    if (EXISTS "${_TE_SRC}")
        file(READ "${_TE_SRC}" _TE_CONTENT)
        if (_TE_CONTENT MATCHES "ImGui::GetKeyIndex")
            string(REGEX REPLACE
                "ImGui::GetKeyIndex\\(([^)]+)\\)"
                "\\1"
                _TE_CONTENT "${_TE_CONTENT}")
            file(WRITE "${_TE_SRC}" "${_TE_CONTENT}")
            message(STATUS "Patched out ImGui::GetKeyIndex from TextEditor.cpp")
        endif()
    endif()
    unset(_TE_CONTENT)
    unset(_TE_SRC)

    add_library(imgui_color_text_edit STATIC
        ${imgui_color_text_edit_SOURCE_DIR}/TextEditor.cpp
    )
    target_include_directories(imgui_color_text_edit PUBLIC
        ${imgui_color_text_edit_SOURCE_DIR}
    )
    target_link_libraries(imgui_color_text_edit PUBLIC imgui)
    set_target_properties(imgui_color_text_edit PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
    )

    # ---- ecs_app target --------------------------------------------------
    add_executable(ecs_app
        app/main.cpp
        app/wgpu_surface_sdl.cpp
        app/editor_ui.cpp
        app/viewport_renderer.cpp
    )
    target_link_libraries(ecs_app PRIVATE
        imgui
        imgui_color_text_edit
        SDL3::SDL3-static
        wgpu_native
    )
    # Script editor panel scans this folder for *.script files.
    target_compile_definitions(ecs_app PRIVATE
        SCRIPTS_DIR="${CMAKE_SOURCE_DIR}/scripts"
    )
    set_target_properties(ecs_app PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
        WIN32_EXECUTABLE OFF
    )

    if (WIN32)
        # Stage the wgpu_native runtime DLL next to ecs_app.exe so the
        # debugger and `cmake --build && run` cycle both work without
        # touching PATH.
        add_custom_command(TARGET ecs_app POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${wgpu_native_SOURCE_DIR}/lib/wgpu_native.dll
                $<TARGET_FILE_DIR:ecs_app>
            VERBATIM
        )
    endif()

    # Stage the body font next to the exe so ImGui can load it at startup.
    # Roboto-Medium ships with the Dear ImGui source tree under misc/fonts/.
    add_custom_command(TARGET ecs_app POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${imgui_SOURCE_DIR}/misc/fonts/Roboto-Medium.ttf
            $<TARGET_FILE_DIR:ecs_app>
        VERBATIM
    )
endif()

foreach(_tgt ${_ecs_targets})
    if (CMAKE_C_COMPILER_ID STREQUAL "Clang"
        AND CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")

        # clang-cl (MSVC-style flags). /arch:AVX2 enables SSE4.1 + AVX2 intrinsics.
        if (ECS_ARCH_X86)
            target_compile_options(${_tgt} PRIVATE /arch:AVX2)
        endif()

        target_compile_options(${_tgt} PRIVATE
            $<$<CONFIG:Release>:/O2 /DNDEBUG>
            $<$<CONFIG:RelWithDebInfo>:/O2 /DNDEBUG>
        )

    elseif (MSVC)

        target_compile_options(${_tgt} PRIVATE
            $<$<COMPILE_LANGUAGE:C>:/experimental:c11atomics>
        )
        if (ECS_ARCH_X86)
            target_compile_options(${_tgt} PRIVATE /arch:AVX2)
        endif()

        target_compile_options(${_tgt} PRIVATE
            $<$<CONFIG:Release>:/O2 /Oi /GL /DNDEBUG>
            $<$<CONFIG:RelWithDebInfo>:/O2 /Oi /GL /DNDEBUG>
        )

        target_link_options(${_tgt} PRIVATE
            $<$<CONFIG:Release>:/LTCG>
            $<$<CONFIG:RelWithDebInfo>:/LTCG>
        )

    else()

        # GCC / Clang (GNU-style). x86 Debug needs -msse4.1 -mavx2 to enable intrinsics;
        # arm64 / other archs use the scalar or NEON backend with no extra flags.
        # Release uses -march=native everywhere — picks up whatever the host supports.
        if (ECS_ARCH_X86)
            target_compile_options(${_tgt} PRIVATE
                $<$<CONFIG:Debug>:-msse4.1 -mavx2>
            )
        endif()

        target_compile_options(${_tgt} PRIVATE
            $<$<CONFIG:Release>:-O3 -flto -march=native -DNDEBUG>
            $<$<CONFIG:RelWithDebInfo>:-O3 -flto -march=native -DNDEBUG>
        )

        target_link_options(${_tgt} PRIVATE
            $<$<CONFIG:Release>:-flto>
            $<$<CONFIG:RelWithDebInfo>:-flto>
        )

    endif()
endforeach()

