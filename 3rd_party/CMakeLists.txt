# HandMadeMath
add_library(hmm INTERFACE)
target_include_directories(hmm INTERFACE HandmadeMath)

# imgui
add_library(imgui
        imgui/imgui.cpp
        imgui/imgui_draw.cpp
        imgui/imgui_tables.cpp
        imgui/imgui_widgets.cpp)
target_include_directories(imgui PUBLIC imgui)

# libmmd
add_library(mmd INTERFACE)
target_include_directories(mmd INTERFACE libmmd/include)

# sokol
add_library(sokol INTERFACE)
target_include_directories(sokol INTERFACE sokol)

# stb
add_library(stb INTERFACE)
target_include_directories(stb INTERFACE stb)

# nfd
add_subdirectory(nativefiledialog-extended)