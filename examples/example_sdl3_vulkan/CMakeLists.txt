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

# Optional: disable test-engine integration
option(USE_IMGUI_TEST_ENGINE "Build with imgui_test_engine" ON)

# ── External dependencies ──────────────────────────────────────────────────────
find_package(PkgConfig REQUIRED)
find_package(Vulkan REQUIRED)
pkg_check_modules(SDL3      REQUIRED IMPORTED_TARGET sdl3)
pkg_check_modules(FREETYPE  REQUIRED IMPORTED_TARGET freetype2)
pkg_check_modules(PLUTOSVG  REQUIRED IMPORTED_TARGET plutosvg)

# ── Sources ────────────────────────────────────────────────────────────────────
set(EXAMPLE_SOURCES
    main.cpp
    vulkan_context.cpp
    sdl_window.cpp
    imgui_layer.cpp
    imgui_console.cpp
)

set(IMGUI_CORE_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdl3.cpp
    ${IMGUI_DIR}/backends/imgui_impl_vulkan.cpp
    ${IMGUI_DIR}/misc/freetype/imgui_freetype.cpp
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

# ── Target ─────────────────────────────────────────────────────────────────────
add_executable(${PROJECT_NAME}
    ${EXAMPLE_SOURCES}
    ${IMGUI_CORE_SOURCES}
)

if(USE_IMGUI_TEST_ENGINE)
    target_sources(${PROJECT_NAME} PRIVATE
        test_engine_layer.cpp
        ${IMGUI_TE_SOURCES}
    )
    target_include_directories(${PROJECT_NAME} PRIVATE ${IMGUI_TE_DIR})
    target_compile_definitions(${PROJECT_NAME} PRIVATE IMGUI_TEST_ENGINE_ENABLE=1)
endif()

# ── Include directories ────────────────────────────────────────────────────────
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${IMGUI_DIR}/misc/freetype
    ${Vulkan_INCLUDE_DIRS}
)

# ── Link libraries ─────────────────────────────────────────────────────────────
target_link_libraries(${PROJECT_NAME} PRIVATE
    PkgConfig::SDL3
    PkgConfig::FREETYPE
    PkgConfig::PLUTOSVG
    Vulkan::Vulkan
    ${CMAKE_DL_LIBS}
)

# ── Compile options ────────────────────────────────────────────────────────────
target_compile_options(${PROJECT_NAME} PRIVATE
    -Wall
    -Wformat
    $<$<CONFIG:Debug>:-g>
    $<$<CONFIG:Release>:-O2>
)
