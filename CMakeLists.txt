cmake_minimum_required(VERSION 3.14)
project(hid-router)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# SDL3
set(SDL_TESTS OFF CACHE BOOL "Disable SDL tests" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "Disable SDL examples" FORCE)
set(SDL_SHARED OFF CACHE BOOL "Disable SDL shared library" FORCE)
set(SDL_STATIC ON CACHE BOOL "Build SDL static library" FORCE)
add_subdirectory(external/SDL)

# hidapi
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build static libraries" FORCE)
add_subdirectory(external/hidapi)

# ViGEmClient
add_subdirectory(external/ViGEmClient)

# Dear ImGui
set(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/imgui")
add_library(imgui STATIC
  ${IMGUI_DIR}/imgui.cpp
  ${IMGUI_DIR}/imgui_demo.cpp
  ${IMGUI_DIR}/imgui_draw.cpp
  ${IMGUI_DIR}/imgui_tables.cpp
  ${IMGUI_DIR}/imgui_widgets.cpp
  ${IMGUI_DIR}/backends/imgui_impl_sdl3.cpp
  ${IMGUI_DIR}/backends/imgui_impl_sdlgpu3.cpp
)
target_include_directories(imgui PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends)
target_link_libraries(imgui PRIVATE SDL3::SDL3-static)

# Each application is the shared code plus its own backend.
function(add_app name backend_source)
  add_executable(${name} WIN32 src/main.cpp src/hid_receiver.cpp ${backend_source})
  target_link_libraries(${name} PRIVATE SDL3::SDL3-static)
  target_link_libraries(${name} PRIVATE imgui)
  target_link_libraries(${name} PRIVATE ws2_32)
  target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/external/json/single_include)
endfunction()

add_app(any2xinput src/any2xinput.cpp)
target_link_libraries(any2xinput PRIVATE hidapi::hidapi)

add_app(sdl-transmitter src/sdl_transmitter.cpp)

add_app(any2flex src/any2flex.cpp)
target_link_libraries(any2flex PRIVATE hidapi::hidapi)

add_app(any2xinput-vigembus src/any2xinput_vigembus.cpp)
target_link_libraries(any2xinput-vigembus PRIVATE ViGEmClient::ViGEmClient setupapi)

# Set the startup project for Visual Studio
set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT any2xinput)
