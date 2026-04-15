cmake_minimum_required(VERSION 3.20)
project(example_sdl3_vulkan CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── Paths ──────────────────────────────────────────────────────────────────────
# IMGUI_DIR  : root of the Dear ImGui repository
# IMGUI_TE_DIR : root of the imgui_test_engine source directory
get_filename_component(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

set(IMGUI_TE_DIR
    "/home/joao/vscode/imgui_test_engine/imgui_test_engine"
    CACHE PATH "Path to the imgui_test_engine source directory")
set(APP_DIR "${IMGUI_DIR}/app")

# Optional: disable test-engine integration
option(USE_IMGUI_TEST_ENGINE "Build with imgui_test_engine" ON)

# ── External dependencies ──────────────────────────────────────────────────────
find_package(PkgConfig REQUIRED)
find_package(Vulkan REQUIRED)
# Fallback for workspace-local vcpkg manifest installs.
if(NOT DEFINED reflectcpp_DIR)
    set(reflectcpp_DIR "/home/joao/vscode/imgui-1/vcpkg_installed/x64-linux/share/reflectcpp" CACHE PATH "Path to reflectcpp CMake config")
endif()
if(EXISTS "${reflectcpp_DIR}/reflectcpp-config.cmake")
    list(PREPEND CMAKE_PREFIX_PATH "/home/joao/vscode/imgui-1/vcpkg_installed/x64-linux")
endif()
find_package(reflectcpp CONFIG REQUIRED)
pkg_check_modules(SDL3      REQUIRED IMPORTED_TARGET sdl3)
pkg_check_modules(FREETYPE  REQUIRED IMPORTED_TARGET freetype2)
pkg_check_modules(PLUTOSVG  REQUIRED IMPORTED_TARGET plutosvg)

# ── Sources ────────────────────────────────────────────────────────────────────
set(APP_COMMON_SOURCES
    ${APP_DIR}/platform/sdl_window.cpp
    ${APP_DIR}/platform/plutosvg_ft_override.cpp
    ${APP_DIR}/ui/imgui_console.cpp
)

set(IMGUI_CORE_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/misc/freetype/imgui_freetype.cpp
)

set(IMGUI_BACKEND_VULKAN_SOURCES
    ${IMGUI_DIR}/backends/imgui_impl_sdl3.cpp
    ${IMGUI_DIR}/backends/imgui_impl_vulkan.cpp
)

set(IMGUI_BACKEND_SDLGPU3_SOURCES
    ${IMGUI_DIR}/backends/imgui_impl_sdl3.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdlgpu3.cpp
)

set(IMGUI_TE_SOURCES
    ${IMGUI_TE_DIR}/imgui_te_engine.cpp
    ${IMGUI_TE_DIR}/imgui_te_context.cpp
    ${IMGUI_TE_DIR}/imgui_te_coroutine.cpp
    ${IMGUI_TE_DIR}/imgui_te_exporters.cpp
    ${IMGUI_TE_DIR}/imgui_te_perftool.cpp
    ${IMGUI_TE_DIR}/imgui_te_ui.cpp
    ${IMGUI_TE_DIR}/imgui_te_utils.cpp
    ${IMGUI_TE_DIR}/imgui_capture_tool.cpp
)

# ── Targets ────────────────────────────────────────────────────────────────────
add_executable(example_sdl3_vulkan
    main.cpp
    ${APP_DIR}/renderer/vulkan/vulkan_context.cpp
    ${APP_DIR}/renderer/vulkan/vulkan_emoji_atlas.cpp
    ${APP_DIR}/ui/imgui_layer.cpp
    ${APP_DIR}/ui/emoji_atlas.cpp
    ${APP_COMMON_SOURCES}
    ${IMGUI_CORE_SOURCES}
    ${IMGUI_BACKEND_VULKAN_SOURCES}
)

add_executable(example_sdl3_sdlgpu3
    main_sdlgpu3.cpp
    ${APP_DIR}/renderer/sdlgpu3/sdlgpu3_context.cpp
    ${APP_DIR}/renderer/sdlgpu3/sdlgpu3_emoji_atlas.cpp
    ${APP_DIR}/ui/imgui_layer_sdlgpu3.cpp
    ${APP_DIR}/ui/emoji_atlas.cpp
    ${APP_COMMON_SOURCES}
    ${IMGUI_CORE_SOURCES}
    ${IMGUI_BACKEND_SDLGPU3_SOURCES}
)

if(USE_IMGUI_TEST_ENGINE)
    target_sources(example_sdl3_vulkan PRIVATE
        ${APP_DIR}/ui/test_engine_layer.cpp
        ${IMGUI_TE_SOURCES}
    )
    target_sources(example_sdl3_sdlgpu3 PRIVATE
        ${APP_DIR}/ui/test_engine_layer.cpp
        ${IMGUI_TE_SOURCES}
    )
    target_include_directories(example_sdl3_vulkan PRIVATE ${IMGUI_TE_DIR})
    target_include_directories(example_sdl3_sdlgpu3 PRIVATE ${IMGUI_TE_DIR})
    target_compile_definitions(example_sdl3_vulkan PRIVATE IMGUI_TEST_ENGINE_ENABLE=1)
    target_compile_definitions(example_sdl3_sdlgpu3 PRIVATE IMGUI_TEST_ENGINE_ENABLE=1)
endif()

target_include_directories(example_sdl3_vulkan PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${IMGUI_DIR}/misc/freetype
    ${Vulkan_INCLUDE_DIRS}
)

target_include_directories(example_sdl3_sdlgpu3 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${IMGUI_DIR}/misc/freetype
)

target_link_libraries(example_sdl3_vulkan PRIVATE
    PkgConfig::SDL3
    PkgConfig::FREETYPE
    PkgConfig::PLUTOSVG
    Vulkan::Vulkan
    reflectcpp::reflectcpp
    ${CMAKE_DL_LIBS}
)

target_link_libraries(example_sdl3_sdlgpu3 PRIVATE
    PkgConfig::SDL3
    PkgConfig::FREETYPE
    PkgConfig::PLUTOSVG
    reflectcpp::reflectcpp
    ${CMAKE_DL_LIBS}
)

target_link_options(example_sdl3_vulkan PRIVATE
    -Wl,--wrap=plutosvg_ft_svg_hooks
)

target_link_options(example_sdl3_sdlgpu3 PRIVATE
    -Wl,--wrap=plutosvg_ft_svg_hooks
)

target_compile_options(example_sdl3_vulkan PRIVATE
    -Wall
    -Wformat
    $<$<CONFIG:Debug>:-g>
    $<$<CONFIG:Release>:-O2>
)

target_compile_options(example_sdl3_sdlgpu3 PRIVATE
    -Wall
    -Wformat
    $<$<CONFIG:Debug>:-g>
    $<$<CONFIG:Release>:-O2>
)
