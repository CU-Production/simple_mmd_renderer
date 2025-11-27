cmake_minimum_required(VERSION 3.15)
project(simple_mmd_renderer)

set(CMAKE_CXX_STANDARD 20)

add_subdirectory(3rd_party)

# Set UTF-8 encoding support
if(MSVC)
    add_compile_options(/utf-8)
    add_definitions(-DNOMINMAX)
else()
    add_compile_options(-finput-charset=UTF-8 -fexec-charset=UTF-8)
endif()

# Windows-specific settings
if(WIN32)
#    add_definitions(-DSOKOL_D3D11)
    add_definitions(-DSOKOL_GLCORE)
#    add_definitions(-DMMD_WINDOWS)
endif()

# Linux-specific settings
if(UNIX AND NOT APPLE)
    add_definitions(-DSOKOL_GLCORE)
endif()

# macOS-specific settings
if(APPLE)
    add_definitions(-DSOKOL_METAL)
endif()

# Find sokol-shdc tool
if(WIN32)
    set(SOKOL_SHDC ${CMAKE_SOURCE_DIR}/3rd_party/sokol-tools-bin/bin/win32/sokol-shdc.exe)
elseif(APPLE)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64")
        set(SOKOL_SHDC ${CMAKE_SOURCE_DIR}/3rd_party/sokol-tools-bin/bin/osx_arm64/sokol-shdc)
    else()
        set(SOKOL_SHDC ${CMAKE_SOURCE_DIR}/3rd_party/sokol-tools-bin/bin/osx/sokol-shdc)
    endif()
else()
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
        set(SOKOL_SHDC ${CMAKE_SOURCE_DIR}/3rd_party/sokol-tools-bin/bin/linux_arm64/sokol-shdc)
    else()
        set(SOKOL_SHDC ${CMAKE_SOURCE_DIR}/3rd_party/sokol-tools-bin/bin/linux/sokol-shdc)
    endif()
endif()

# Set sokol-shdc command
set(SOKOL_SHDC_COMMAND ${SOKOL_SHDC})

# On non-Windows platforms, ensure sokol-shdc has execute permissions
if(NOT WIN32)
    file(COPY ${SOKOL_SHDC} DESTINATION ${CMAKE_BINARY_DIR} 
         FILE_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE 
         GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
    get_filename_component(SHDC_NAME ${SOKOL_SHDC} NAME)
    set(SOKOL_SHDC_COMMAND ${CMAKE_BINARY_DIR}/${SHDC_NAME})
endif()

# Determine shader languages to generate (sokol-shdc supports multi-language output, use colon separator)
# Generate all required shader languages to support cross-platform
set(SHADER_LANG "glsl430:hlsl5:metal_macos")

# Compile shader files
set(SHADER_SRC ${CMAKE_SOURCE_DIR}/shader/main.glsl)
set(SHADER_HEADER ${CMAKE_BINARY_DIR}/shader/main.glsl.h)
set(IBL_SHADER_SRC ${CMAKE_SOURCE_DIR}/shader/ibl.glsl)
set(IBL_SHADER_HEADER ${CMAKE_BINARY_DIR}/shader/ibl.glsl.h)
set(SHADOW_SHADER_SRC ${CMAKE_SOURCE_DIR}/shader/shadow.glsl)
set(SHADOW_SHADER_HEADER ${CMAKE_BINARY_DIR}/shader/shadow.glsl.h)

# Create shader output directory
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/shader)

# Add custom command to compile main shader
add_custom_command(
    OUTPUT ${SHADER_HEADER}
    COMMAND ${SOKOL_SHDC_COMMAND}
        --input ${SHADER_SRC}
        --output ${SHADER_HEADER}
        --slang ${SHADER_LANG}
    DEPENDS ${SHADER_SRC}
    COMMENT "Compiling shader ${SHADER_SRC} -> ${SHADER_HEADER}"
    VERBATIM
)

# Add custom command to compile IBL shader
add_custom_command(
    OUTPUT ${IBL_SHADER_HEADER}
    COMMAND ${SOKOL_SHDC_COMMAND}
        --input ${IBL_SHADER_SRC}
        --output ${IBL_SHADER_HEADER}
        --slang ${SHADER_LANG}
    DEPENDS ${IBL_SHADER_SRC}
    COMMENT "Compiling shader ${IBL_SHADER_SRC} -> ${IBL_SHADER_HEADER}"
    VERBATIM
)

# Add custom command to compile shadow shader
add_custom_command(
    OUTPUT ${SHADOW_SHADER_HEADER}
    COMMAND ${SOKOL_SHDC_COMMAND}
        --input ${SHADOW_SHADER_SRC}
        --output ${SHADOW_SHADER_HEADER}
        --slang ${SHADER_LANG}
    DEPENDS ${SHADOW_SHADER_SRC}
    COMMENT "Compiling shader ${SHADOW_SHADER_SRC} -> ${SHADOW_SHADER_HEADER}"
    VERBATIM
)

# Create shader header file target
add_custom_target(shader_header DEPENDS ${SHADER_HEADER} ${IBL_SHADER_HEADER} ${SHADOW_SHADER_HEADER})

# Include generated shader header file directory
include_directories(${CMAKE_BINARY_DIR})

add_executable(simple_mmd_renderer main.cpp)
add_dependencies(simple_mmd_renderer shader_header)
target_link_libraries(simple_mmd_renderer PRIVATE hmm imgui imguizmo mmd sokol stb nfd)
add_custom_command(
    TARGET simple_mmd_renderer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory ARGS "${CMAKE_CURRENT_SOURCE_DIR}/assets" "${CMAKE_CURRENT_BINARY_DIR}/assets"
)
