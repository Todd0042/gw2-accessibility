cmake_minimum_required(VERSION 3.20)
project(GW2Accessibility VERSION 1.0.1)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui")

add_library(GW2Accessibility SHARED
    src/dllmain.cpp
    src/addon.cpp
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
)

target_include_directories(GW2Accessibility PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${IMGUI_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)

set_target_properties(GW2Accessibility PROPERTIES
    PREFIX  ""
    SUFFIX  ".dll"
    OUTPUT_NAME "GW2Accessibility"
)

target_compile_definitions(GW2Accessibility PRIVATE
    _WIN32_WINNT=0x0A00
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

target_link_libraries(GW2Accessibility PRIVATE
    -static-libgcc
    -static-libstdc++
    -Wl,-Bstatic,--whole-archive,-lwinpthread,--no-whole-archive
    -Wl,-Bdynamic
    -luuid
    -lsapi
    -lwinhttp
    -lshell32
    -lole32
    -ladvapi32
)
