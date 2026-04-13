cmake_minimum_required(VERSION 3.10)
project(imgui C CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GLFW     REQUIRED glfw3)
pkg_check_modules(FREETYPE REQUIRED freetype2)
pkg_check_modules(PLUTOSVG REQUIRED plutosvg)
find_package(Vulkan REQUIRED)

set(IMGUI_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/imgui.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/imgui_draw.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/imgui_demo.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/imgui_tables.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/imgui_widgets.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/backends/imgui_impl_null.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/misc/freetype/imgui_freetype.cpp
)

add_library(imgui STATIC ${IMGUI_SOURCES})
target_include_directories(imgui PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/backends
    ${CMAKE_CURRENT_SOURCE_DIR}/misc/freetype
    ${GLFW_INCLUDE_DIRS}
    ${Vulkan_INCLUDE_DIRS}
    ${FREETYPE_INCLUDE_DIRS}
    ${PLUTOSVG_INCLUDE_DIRS}
)
target_compile_definitions(imgui PUBLIC ${PLUTOSVG_CFLAGS_OTHER})
target_link_libraries(imgui PUBLIC ${GLFW_LIBRARIES} ${Vulkan_LIBRARIES} ${FREETYPE_LIBRARIES} ${PLUTOSVG_LIBRARIES})

# ── Examples ───────────────────────────────────────────────────────────────────
# Place all executables under CMAKE_BINARY_DIR so launch.json can use a
# stable path regardless of subdirectory nesting.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/examples/example_sdl3_vulkan")
add_subdirectory(examples/example_sdl3_vulkan)
