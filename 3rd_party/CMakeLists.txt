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

# ImGuizmo
add_library(imguizmo
        ImGuizmo/GraphEditor.cpp
        ImGuizmo/ImCurveEdit.cpp
        ImGuizmo/ImGradient.cpp
        ImGuizmo/ImGuizmo.cpp
        ImGuizmo/ImSequencer.cpp)
target_include_directories(imguizmo PUBLIC ImGuizmo)
target_link_libraries(imguizmo PRIVATE imgui)

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

# bullet
option(BUILD_BULLET2_DEMOS OFF)
option(BUILD_CPU_DEMOS OFF)
option(BUILD_EXTRAS OFF)
option(BUILD_OPENGL3_DEMOS OFF)
option(BUILD_UNIT_TESTS OFF)
add_subdirectory(bullet)

# JoltPhysics
set(DOUBLE_PRECISION ON)
set(GENERATE_DEBUG_SYMBOLS ON)
set(OVERRIDE_CXX_FLAGS ON)
set(CROSS_PLATFORM_DETERMINISTIC OFF)
set(INTERPROCEDURAL_OPTIMIZATION ON)
set(FLOATING_POINT_EXCEPTIONS_ENABLED OFF)
set(CPP_EXCEPTIONS_ENABLED OFF)
set(CPP_RTTI_ENABLED OFF)
# MMD uses 16 collision groups, need 32-bit ObjectLayer for 16-bit group + 16-bit mask
set(OBJECT_LAYER_BITS 32)

set(USE_SSE4_1 ON)
set(USE_SSE4_2 ON)
set(USE_AVX ON)
set(USE_AVX2 ON)
set(USE_AVX512 OFF)
set(USE_LZCNT ON)
set(USE_TZCNT ON)
set(USE_F16C ON)
set(USE_FMADD ON)
set(USE_STATIC_MSVC_RUNTIME_LIBRARY OFF)
add_subdirectory(JoltPhysics/Build)
