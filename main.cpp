#define SOKOL_IMPL
#define SOKOL_TRACE_HOOKS
#ifdef _WIN32
// #define SOKOL_D3D11
#define STBI_WINDOWS_UTF8
#include <windows.h>
#else
// #define SOKOL_GLCORE
#endif
#include "sokol_app.h"
#include "sokol_gfx.h"
#include "sokol_log.h"
#include "sokol_glue.h"
#include "sokol_time.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "util/sokol_imgui.h"
#include "util/sokol_gfx_imgui.h"
#include "ImGuizmo.h"
#include "ImSequencer.h"

#include "mmd/mmd.hxx"
#include "HandmadeMath.h"
#include "shader/main.glsl.h"
#include "shader/ground.glsl.h"
#include "shader/ibl.glsl.h"
#include "shader/shadow.glsl.h"
#include "nfd.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include <iostream>
#include <vector>
#include <memory>
#include <string>
#include <codecvt>
#include <locale>
#include <fstream>
#include <algorithm>
#include <cmath>
#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#include <limits.h>
#endif

// Vertex structure
struct Vertex {
    float pos[3];
    float normal[3];
    float uv[2];
};

// Sequencer interface for VMD animation
class MotionSequencer : public ImSequencer::SequenceInterface {
public:
    MotionSequencer(std::shared_ptr<mmd::Motion> motion) : motion_(motion) {
        if (motion_) {
            // Create a simple entry representing the entire animation
            int max_frame = static_cast<int>(motion_->GetLength());
            if (max_frame == 0) {
                max_frame = 10000; // Default if length is 0
            }
            entries_.push_back({0, max_frame, 0});
        }
    }
    
    virtual int GetFrameMin() const override {
        return 0;
    }
    
    virtual int GetFrameMax() const override {
        if (motion_) {
            int max_frame = static_cast<int>(motion_->GetLength());
            return max_frame > 0 ? max_frame : 10000;
        }
        return 10000;
    }
    
    virtual int GetItemCount() const override {
        return static_cast<int>(entries_.size());
    }
    
    virtual void Get(int index, int** start, int** end, int* type, unsigned int* color) override {
        if (index >= 0 && index < static_cast<int>(entries_.size())) {
            if (start) {
                *start = &entries_[index].start;
            }
            if (end) {
                *end = &entries_[index].end;
            }
            if (type) {
                *type = entries_[index].type;
            }
            if (color) {
                *color = 0xFFAA0000; // Red color for animation entry
            }
        }
    }
    
    virtual const char* GetItemLabel(int index) const override {
        if (index == 0) {
            return "VMD Animation";
        }
        return "";
    }
    
    virtual void DoubleClick(int index) override {
        // Could seek to frame on double click
    }
    
private:
    std::shared_ptr<mmd::Motion> motion_;
    struct Entry {
        int start;
        int end;
        int type;
    };
    std::vector<Entry> entries_;
};


// Application state
struct {
    sg_pipeline pip;
    sg_bindings bind;
    sg_pass_action main_pass_action;
    sg_pass_action ui_pass_action;
    
    std::shared_ptr<mmd::Model> model;
    std::shared_ptr<mmd::Motion> motion;
    std::unique_ptr<mmd::Poser> poser;
    std::unique_ptr<mmd::MotionPlayer> motion_player;
    
    sg_buffer vertex_buffer = {0};
    sg_buffer index_buffer = {0};
    
    sgimgui_t sgimgui;
    
    float time = 0.0f;
    bool model_loaded = false;
    bool motion_loaded = false;

    // Camera parameters
    // Camera settings in meters (MMD models are now converted to meters)
    HMM_Vec3 camera_pos = {0.0f, 1.6f, 4.0f}; // ~1.6m height (eye level), 4m away
    HMM_Vec3 camera_target = {0.0f, 0.0f, 0.0f}; // Look at origin
    float camera_fov = 45.0f;
    float camera_distance = 4.0f; // 4 meters (typical viewing distance for character)
    float camera_rotation_x = 0.0f;  // Horizontal rotation (around Y axis)
    float camera_rotation_y = 0.0f;  // Vertical rotation (around X axis)
    
    // Camera control state
    bool camera_rotating = false;
    bool camera_panning = false;
    float last_mouse_x = 0.0f;
    float last_mouse_y = 0.0f;
    bool camera_window_open = false;
    bool keys_down[256] = {false};
    
    std::string model_filename;
    std::string motion_filename;
    
    // ImGuizmo model transform
    bool guizmo_enabled = false;
    float model_matrix[16] = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f
    };
    ImGuizmo::OPERATION guizmo_operation = ImGuizmo::TRANSLATE;
    ImGuizmo::MODE guizmo_mode = ImGuizmo::WORLD;
    bool guizmo_use_snap = false;
    float guizmo_snap[3] = {1.0f, 1.0f, 1.0f};
    bool guizmo_debug_window = false;
    bool guizmo_draw_grid = false;  // DrawGrid开关
    
    // ImSequencer animation timeline
    bool sequencer_enabled = false;
    int sequencer_current_frame = 0;
    bool sequencer_expanded = true;
    int sequencer_selected_entry = -1;
    int sequencer_first_frame = 0;
    std::unique_ptr<MotionSequencer> sequencer;
    bool animation_playing = true;  // Animation playback state
    bool sequencer_manual_control = false;  // Whether user is manually controlling sequencer
    int sequencer_last_frame = -1;  // Track last frame to detect manual changes
    
    // Skybox resources
    sg_image equirectangular_map = {0};
    sg_image environment_cubemap = {0};
    sg_view environment_cubemap_view = {0}; // Persistent view for environment cubemap
    sg_sampler default_sampler = {0};
    sg_pipeline equirect_to_cubemap_pip = {0};
    sg_pipeline skybox_pip = {0};
    sg_buffer skybox_vertex_buffer = {0};
    sg_buffer skybox_index_buffer = {0};
    bool ibl_initialized = false;
    bool show_skybox = true;
    
    // Material textures (map from material index to texture image)
    std::vector<sg_image> material_textures;
    std::vector<sg_view> material_texture_views; // Persistent views for material textures
    sg_image default_texture = {0}; // White 1x1 texture for materials without texture
    sg_view default_texture_view = {0}; // Persistent view for default texture
    
    // Shadow mapping resources
    sg_image shadow_map = {0};
    sg_view shadow_map_view = {0};
    sg_view shadow_map_ds_view = {0};
    sg_sampler shadow_sampler = {0};
    sg_pipeline shadow_pip = {0};
    sg_pass_action shadow_pass_action;
    const int shadow_map_size = 2048;
    
    // Dummy color attachment for OpenGL workaround (depth-only framebuffers need glDrawBuffer(GL_NONE))
    // This is a workaround for sokol not setting glDrawBuffer(GL_NONE) when there are no color attachments
    sg_image shadow_dummy_color = {0};
    sg_view shadow_dummy_color_view = {0};
    
    // Ground plane (stage)
    sg_buffer ground_vertex_buffer = {0};
    sg_buffer ground_index_buffer = {0};
    sg_pipeline ground_pip = {0};
    
    // Directional light (sun/sky light)
    // Default: light from top-right-front, pointing downward (normalized)
    HMM_Vec3 light_direction = HMM_NormV3(HMM_Vec3{0.3f, -1.0f, 0.2f}); // Normalized direction (light from above, slightly from front-right)
    HMM_Vec3 light_color = {1.0f, 1.0f, 1.0f}; // White light
    float light_intensity = 1.0f;
    bool shadows_enabled = true;
    bool light_window_open = false;
    
    // Figure/Resin material parameters
    float rim_power = 3.0f; // Rim light power (higher = sharper rim, typical: 2.0-5.0)
    float rim_intensity = 1.0f; // Rim light intensity (typical: 0.5-2.0)
    HMM_Vec3 rim_color = {1.0f, 1.0f, 1.0f}; // Rim light color (white for neutral, can be tinted)
    float specular_power = 64.0f; // Specular highlight power (higher = sharper, typical: 32.0-128.0)
    float specular_intensity = 1.0f; // Specular highlight intensity (typical: 0.5-2.0)
} g_state;


// UTF-8 string conversion helper functions
#ifdef _WIN32
std::wstring utf8_to_wstring(const std::string& str) {
    if (str.empty()) return std::wstring();
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), NULL, 0);
    std::wstring wstrTo(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), &wstrTo[0], size_needed);
    return wstrTo;
}

std::string wstring_to_utf8(const std::wstring& wstr) {
    if (wstr.empty()) return std::string();
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
    std::string strTo(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &strTo[0], size_needed, NULL, NULL);
    return strTo;
}
#else
// Linux/macOS use standard library
std::wstring utf8_to_wstring(const std::string& str) {
    std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
    return converter.from_bytes(str);
}

std::string wstring_to_utf8(const std::wstring& wstr) {
    std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
    return converter.to_bytes(wstr);
}
#endif

// Helper function to check if file exists (case-insensitive on Windows)
bool FileExists(const std::wstring& path) {
#ifdef _WIN32
    DWORD dwAttrib = GetFileAttributesW(path.c_str());
    if (dwAttrib != INVALID_FILE_ATTRIBUTES && !(dwAttrib & FILE_ATTRIBUTE_DIRECTORY)) {
        return true;
    }
    // On Windows, try case-insensitive search if exact match fails
    // Find the directory and filename parts
    size_t last_slash = path.find_last_of(L"\\/");
    if (last_slash != std::wstring::npos && last_slash + 1 < path.length()) {
        std::wstring dir = path.substr(0, last_slash + 1);
        std::wstring filename = path.substr(last_slash + 1);
        
        // Try to find file with case-insensitive match
        WIN32_FIND_DATAW findData;
        HANDLE hFind = FindFirstFileW(path.c_str(), &findData);
        if (hFind != INVALID_HANDLE_VALUE) {
            FindClose(hFind);
            return !(findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
        }
    }
    return false;
#else
    struct stat buffer;
    std::string path_utf8 = wstring_to_utf8(path);
    return (stat(path_utf8.c_str(), &buffer) == 0);
#endif
}

// Helper to get current working directory
std::wstring GetCurrentWorkingDirectory() {
#ifdef _WIN32
    wchar_t buffer[MAX_PATH];
    DWORD length = GetCurrentDirectoryW(MAX_PATH, buffer);
    if (length > 0 && length < MAX_PATH) {
        std::wstring cwd(buffer);
        if (cwd.back() != L'\\' && cwd.back() != L'/') {
            cwd += L'\\';
        }
        return cwd;
    }
    return L"";
#else
    char buffer[PATH_MAX];
    if (getcwd(buffer, PATH_MAX) != nullptr) {
        std::string cwd_str(buffer);
        std::wstring cwd(cwd_str.begin(), cwd_str.end());
        if (cwd.back() != L'/' && cwd.back() != L'\\') {
            cwd += L'/';
        }
        return cwd;
    }
    return L"";
#endif
}

// Helper to normalize path separators
std::wstring NormalizePath(const std::wstring& path) {
    std::wstring normalized = path;
    std::replace(normalized.begin(), normalized.end(), L'/', L'\\');
    return normalized;
}

// Helper to combine paths
std::wstring CombinePaths(const std::wstring& base, const std::wstring& relative) {
    if (relative.empty()) return base;
    if (base.empty()) return relative;
    
    std::wstring base_norm = NormalizePath(base);
    std::wstring rel_norm = NormalizePath(relative);
    
    // Ensure base ends with separator
    if (base_norm.back() != L'\\' && base_norm.back() != L'/') {
        base_norm += L'\\';
    }
    
    // Remove leading separator from relative if present
    if (!rel_norm.empty() && (rel_norm[0] == L'\\' || rel_norm[0] == L'/')) {
        rel_norm = rel_norm.substr(1);
    }
    
    return base_norm + rel_norm;
}

// Check if path is absolute
bool IsAbsolutePath(const std::wstring& path) {
    if (path.empty()) return false;
#ifdef _WIN32
    // Windows: check for drive letter (C:) or UNC path (\\)
    if (path.length() >= 2) {
        if (path[1] == L':') return true; // Drive letter
        if (path[0] == L'\\' && path[1] == L'\\') return true; // UNC
    }
    return false;
#else
    return path[0] == L'/';
#endif
}

// Helper function to find file with case-insensitive filename (Windows only)
std::wstring FindFileCaseInsensitive(const std::wstring& dir, const std::wstring& filename) {
#ifdef _WIN32
    // On Windows, use FindFirstFile to do case-insensitive search
    std::wstring search_path = CombinePaths(dir, filename);
    WIN32_FIND_DATAW findData;
    HANDLE hFind = FindFirstFileW(search_path.c_str(), &findData);
    if (hFind != INVALID_HANDLE_VALUE) {
        FindClose(hFind);
        if (!(findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            return CombinePaths(dir, findData.cFileName);
        }
    }
    return L"";
#else
    // On Linux/macOS, file system is case-sensitive, so just return empty
    return L"";
#endif
}

// Load texture from file path
sg_image LoadTexture(const std::wstring& texture_path, const std::wstring& model_dir = L"") {
    if (texture_path.empty()) {
        return g_state.default_texture;
    }
    
    // Try multiple path combinations
    std::vector<std::wstring> paths_to_try;
    
    // 1. Use path as-is (may be absolute)
    paths_to_try.push_back(NormalizePath(texture_path));
    
    // 2. If model_dir is provided, try model_dir + texture_path
    if (!model_dir.empty()) {
        paths_to_try.push_back(CombinePaths(model_dir, texture_path));
    }
    
    // 3. If texture_path contains directory separators, try different combinations
    if (!model_dir.empty() && !texture_path.empty()) {
        size_t last_sep = texture_path.find_last_of(L"\\/");
        if (last_sep != std::wstring::npos && last_sep + 1 < texture_path.length()) {
            std::wstring filename_only = texture_path.substr(last_sep + 1);
            std::wstring dir_part = texture_path.substr(0, last_sep + 1);
            
            // Try model_dir + filename_only (in case texture_path has extra directory prefix)
            paths_to_try.push_back(CombinePaths(model_dir, filename_only));
            
            // Try model_dir + dir_part + filename_only (preserve subdirectory structure)
            paths_to_try.push_back(CombinePaths(model_dir, texture_path));
            
            // Also try model_dir + "tex/" + filename_only (common MMD texture location)
            paths_to_try.push_back(CombinePaths(CombinePaths(model_dir, L"tex"), filename_only));
        } else {
            // No directory separator, try both root and tex/ subdirectory
            paths_to_try.push_back(CombinePaths(model_dir, texture_path));
            paths_to_try.push_back(CombinePaths(CombinePaths(model_dir, L"tex"), texture_path));
        }
    }
    
    // 4. Try current working directory + texture_path (if path is relative)
    if (!IsAbsolutePath(texture_path)) {
        std::wstring cwd = GetCurrentWorkingDirectory();
        if (!cwd.empty()) {
            paths_to_try.push_back(CombinePaths(cwd, texture_path));
        }
    }
    
    // Try each path
    std::wstring final_path;
    bool found = false;
    for (const auto& path : paths_to_try) {
        if (FileExists(path)) {
            final_path = path;
            found = true;
            break;
        }
    }
    
    // If not found and on Windows, try case-insensitive search in model_dir
    if (!found && !model_dir.empty()) {
#ifdef _WIN32
        size_t last_sep = texture_path.find_last_of(L"\\/");
        if (last_sep != std::wstring::npos && last_sep + 1 < texture_path.length()) {
            std::wstring filename = texture_path.substr(last_sep + 1);
            std::wstring found_path = FindFileCaseInsensitive(model_dir, filename);
            if (!found_path.empty()) {
                final_path = found_path;
                found = true;
            } else {
                // Also try in tex/ subdirectory
                found_path = FindFileCaseInsensitive(CombinePaths(model_dir, L"tex"), filename);
                if (!found_path.empty()) {
                    final_path = found_path;
                    found = true;
                }
            }
        } else {
            // No directory separator, try case-insensitive search
            std::wstring found_path = FindFileCaseInsensitive(model_dir, texture_path);
            if (!found_path.empty()) {
                final_path = found_path;
                found = true;
            } else {
                found_path = FindFileCaseInsensitive(CombinePaths(model_dir, L"tex"), texture_path);
                if (!found_path.empty()) {
                    final_path = found_path;
                    found = true;
                }
            }
        }
#endif
    }
    
    if (!found) {
        // Print all attempted paths for debugging (only first few to avoid spam)
        static int debug_count = 0;
        if (debug_count < 5) {
            std::cerr << "Failed to load texture: " << wstring_to_utf8(texture_path) << std::endl;
            std::cerr << "Model dir: " << wstring_to_utf8(model_dir) << std::endl;
            std::cerr << "Tried paths:" << std::endl;
            for (const auto& path : paths_to_try) {
                std::cerr << "  " << wstring_to_utf8(path) << std::endl;
            }
            debug_count++;
        }
        return g_state.default_texture;
    }
    
    // Convert wide string to UTF-8 for stb_image
    // With STBI_WINDOWS_UTF8, stbi_load can handle UTF-8 paths directly on Windows
    std::string path_utf8 = wstring_to_utf8(final_path);
    
    int width, height, channels;
    unsigned char* data = stbi_load(path_utf8.c_str(), &width, &height, &channels, 4); // Force RGBA
    if (!data) {
        const char* error = stbi_failure_reason();
        std::cerr << "Failed to load texture: " << path_utf8 << std::endl;
        if (error) {
            std::cerr << "  Error: " << error << std::endl;
        }
        return g_state.default_texture;
    }
    
    // Validate loaded image data
    if (width <= 0 || height <= 0) {
        std::cerr << "Invalid texture dimensions: " << width << "x" << height << std::endl;
        stbi_image_free(data);
        return g_state.default_texture;
    }
    
    // Extract filename for label (use just the filename, not full path)
    std::string label = path_utf8;
    size_t last_slash = label.find_last_of("\\/");
    if (last_slash != std::string::npos && last_slash + 1 < label.length()) {
        label = label.substr(last_slash + 1);
    }
    // Limit label length to avoid issues
    if (label.length() > 64) {
        label = label.substr(0, 64);
    }
    
    sg_image_desc img_desc = {};
    img_desc.type = SG_IMAGETYPE_2D;
    img_desc.width = width;
    img_desc.height = height;
    img_desc.num_mipmaps = 1;
    img_desc.pixel_format = SG_PIXELFORMAT_RGBA8;
    img_desc.usage.immutable = true;
    img_desc.label = label.c_str();
    img_desc.data.mip_levels[0].ptr = data;
    img_desc.data.mip_levels[0].size = width * height * 4;
    
    sg_image img = sg_make_image(&img_desc);
    stbi_image_free(data);
    
    if (img.id == SG_INVALID_ID) {
        std::cerr << "Failed to create sokol image from texture data" << std::endl;
        return g_state.default_texture;
    }
    
    std::cout << "Loaded texture: " << path_utf8 << " (" << width << "x" << height << ", " << channels << " channels)" << std::endl;
    return img;
}

// Helper to convert relative path to absolute path
std::wstring GetAbsolutePath(const std::wstring& path) {
    if (path.empty()) return path;
    if (IsAbsolutePath(path)) return path;
    
#ifdef _WIN32
    wchar_t full_path[MAX_PATH];
    DWORD length = GetFullPathNameW(path.c_str(), MAX_PATH, full_path, nullptr);
    if (length > 0 && length < MAX_PATH) {
        return std::wstring(full_path);
    }
    return path;
#else
    char* resolved = realpath(wstring_to_utf8(path).c_str(), nullptr);
    if (resolved) {
        std::string resolved_str(resolved);
        free(resolved);
        return utf8_to_wstring(resolved_str);
    }
    return path;
#endif
}

// Load all material textures from the model
void LoadMaterialTextures(const std::string& model_filename) {
    if (!g_state.model || !g_state.model_loaded) {
        return;
    }
    
    // Clean up old textures and views
    for (size_t i = 0; i < g_state.material_textures.size(); ++i) {
        if (g_state.material_textures[i].id != 0 && g_state.material_textures[i].id != g_state.default_texture.id) {
            sg_destroy_image(g_state.material_textures[i]);
        }
        if (i < g_state.material_texture_views.size() && g_state.material_texture_views[i].id != 0) {
            sg_destroy_view(g_state.material_texture_views[i]);
        }
    }
    g_state.material_textures.clear();
    g_state.material_texture_views.clear();
    
    // Get model directory from filename (convert to absolute path first)
    std::wstring model_dir;
    if (!model_filename.empty()) {
        std::wstring wfilename = utf8_to_wstring(model_filename);
        // Convert to absolute path
        wfilename = GetAbsolutePath(wfilename);
        
        size_t last_slash = wfilename.find_last_of(L"\\/");
        if (last_slash != std::wstring::npos) {
            model_dir = wfilename.substr(0, last_slash + 1);
        } else {
            // No directory separator, use current directory
            model_dir = GetCurrentWorkingDirectory();
        }
    } else {
        model_dir = GetCurrentWorkingDirectory();
    }
    
    std::cout << "Model directory: " << wstring_to_utf8(model_dir) << std::endl;
    
    size_t part_num = g_state.model->GetPartNum();
    g_state.material_textures.resize(part_num, g_state.default_texture);
    g_state.material_texture_views.resize(part_num, g_state.default_texture_view);
    
    for (size_t i = 0; i < part_num; ++i) {
        const mmd::Model::Part& part = g_state.model->GetPart(i);
        const mmd::Material& material = part.GetMaterial();
        
        const mmd::Texture* texture = material.GetTexture();
        if (texture) {
            std::wstring texture_path = texture->GetTexturePath();
            // Only print first few textures to avoid spam
            if (i < 3) {
                std::cout << "Loading texture " << i << ": " << wstring_to_utf8(texture_path) << std::endl;
            }
            sg_image loaded_tex = LoadTexture(texture_path, model_dir);
            g_state.material_textures[i] = loaded_tex;
            
            // Create persistent view for this texture
            sg_view_desc view_desc = {};
            view_desc.texture.image = loaded_tex;
            g_state.material_texture_views[i] = sg_make_view(&view_desc);
        } else {
            g_state.material_textures[i] = g_state.default_texture;
            g_state.material_texture_views[i] = g_state.default_texture_view;
        }
    }
    
    std::cout << "Loaded " << g_state.material_textures.size() << " material textures" << std::endl;
}

// Load PMX model
bool LoadPMXModel(const std::string& filename) {
    try {
        std::wstring wfilename = utf8_to_wstring(filename);
        mmd::FileReader file(wfilename);
        
        // Read PMX file
        mmd::PmxReader reader(file);
        g_state.model = std::make_shared<mmd::Model>();
        reader.ReadModel(*g_state.model);
        g_state.model_loaded = true;
        
        // Create poser for the model
        if (g_state.model) {
            g_state.poser = std::make_unique<mmd::Poser>(*g_state.model);
            // Create motion player if motion is already loaded
            if (g_state.motion && g_state.poser) {
                g_state.motion_player = std::make_unique<mmd::MotionPlayer>(*g_state.motion, *g_state.poser);
            }
        }
        
        std::wstring model_name = g_state.model->GetName();
        std::string model_name_utf8 = wstring_to_utf8(model_name);
        std::cout << "Loaded PMX model: " << model_name_utf8 << std::endl;
        std::cout << "  Vertices: " << g_state.model->GetVertexNum() << std::endl;
        std::cout << "  Triangles: " << g_state.model->GetTriangleNum() << std::endl;
        std::cout << "  Bones: " << g_state.model->GetBoneNum() << std::endl;
        std::cout << "  Parts: " << g_state.model->GetPartNum() << std::endl;
        
        // Load material textures (pass filename for path resolution)
        LoadMaterialTextures(filename);
        
        return true;
    } catch (const mmd::exception& e) {
        std::cerr << "Error loading PMX model: " << e.what() << std::endl;
        return false;
    } catch (...) {
        std::cerr << "Unknown error loading PMX model" << std::endl;
        return false;
    }
}

// Load VMD motion file
bool LoadVMDMotion(const std::string& filename) {
    try {
        std::wstring wfilename = utf8_to_wstring(filename);
        mmd::FileReader file(wfilename);
        
        mmd::VmdReader reader(file);
        g_state.motion = std::make_shared<mmd::Motion>();
        reader.ReadMotion(*g_state.motion);
        g_state.motion_loaded = true;
        
        // Create sequencer for animation timeline
        g_state.sequencer = std::make_unique<MotionSequencer>(g_state.motion);
        
        // Create motion player if both model and motion are loaded
        if (g_state.model && g_state.motion && g_state.poser) {
            g_state.motion_player = std::make_unique<mmd::MotionPlayer>(*g_state.motion, *g_state.poser);
        }
        
        std::wstring motion_name = g_state.motion->GetName();
        std::string motion_name_utf8 = wstring_to_utf8(motion_name);
        std::cout << "Loaded VMD motion: " << motion_name_utf8 << std::endl;
        
        return true;
    } catch (const mmd::exception& e) {
        std::cerr << "Error loading VMD motion: " << e.what() << std::endl;
        return false;
    } catch (...) {
        std::cerr << "Unknown error loading VMD motion" << std::endl;
        return false;
    }
}

// Update model vertex buffers (initial creation)
void UpdateModelBuffers() {
    if (!g_state.model || !g_state.model_loaded) return;
    
    // Release old buffers
    if (g_state.vertex_buffer.id != 0) {
        sg_destroy_buffer(g_state.vertex_buffer);
    }
    if (g_state.index_buffer.id != 0) {
        sg_destroy_buffer(g_state.index_buffer);
    }
    
    // Prepare vertex data
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    
    size_t vertex_num = g_state.model->GetVertexNum();
    vertices.reserve(vertex_num);
    
    // MMD models use centimeters, convert to meters (divide by 10)
    const float mmd_to_meter = 0.1f; // 10 cm = 0.1 m
    
    for (size_t i = 0; i < vertex_num; ++i) {
        mmd::Model::Vertex<mmd::ref> vertex = g_state.model->GetVertex(i);
        mmd::Vector3f pos = vertex.GetCoordinate();
        mmd::Vector3f normal = vertex.GetNormal();
        mmd::Vector2f uv = vertex.GetUVCoordinate();
        
        Vertex v;
        // Convert position from centimeters to meters
        v.pos[0] = pos.p.x * mmd_to_meter;
        v.pos[1] = pos.p.y * mmd_to_meter;
        v.pos[2] = pos.p.z * mmd_to_meter;
        // Normals don't need conversion (they're unit vectors)
        v.normal[0] = normal.p.x;
        v.normal[1] = normal.p.y;
        v.normal[2] = normal.p.z;
        v.uv[0] = uv.v[0];
        v.uv[1] = uv.v[1];
        
        vertices.push_back(v);
    }
    
    // Prepare index data
    size_t triangle_num = g_state.model->GetTriangleNum();
    indices.reserve(triangle_num * 3);
    
    for (size_t i = 0; i < triangle_num; ++i) {
        const mmd::Vector3D<std::uint32_t>& triangle = g_state.model->GetTriangle(i);
        // workaround for backface culling
        indices.push_back(triangle.v[2]);
        indices.push_back(triangle.v[1]);
        indices.push_back(triangle.v[0]);
    }
    
    // Create vertex buffer with dynamic usage for animation updates
    // For stream_update buffers, we must not provide initial data
    // Initial data will be uploaded in UpdateDeformedVertices() on first frame
    sg_buffer_desc vbuf_desc = {};
    vbuf_desc.size = vertices.size() * sizeof(Vertex);
    vbuf_desc.usage.stream_update = true;  // Enable dynamic updates for animation
    vbuf_desc.label = "model-vertices";
    g_state.vertex_buffer = sg_make_buffer(&vbuf_desc);
    
    // Create index buffer (static, doesn't change)
    if (indices.empty()) {
        std::cerr << "Error: Index data is empty!" << std::endl;
        return;
    }
    sg_buffer_desc ibuf_desc = {};
    ibuf_desc.usage.index_buffer = true;
    ibuf_desc.data.ptr = indices.data();
    ibuf_desc.data.size = indices.size() * sizeof(uint32_t);
    ibuf_desc.label = "model-indices";
    g_state.index_buffer = sg_make_buffer(&ibuf_desc);
    
    if (g_state.index_buffer.id == SG_INVALID_ID) {
        std::cerr << "Error: Failed to create index buffer!" << std::endl;
        return;
    }
    
    // Update bindings
    g_state.bind.vertex_buffers[0] = g_state.vertex_buffer;
    g_state.bind.index_buffer = g_state.index_buffer;
}

// Update vertex buffer with deformed vertices (called each frame after Deform())
void UpdateDeformedVertices() {
    if (!g_state.model || !g_state.model_loaded || !g_state.poser || g_state.vertex_buffer.id == 0) {
        return;
    }
    
    size_t vertex_num = g_state.model->GetVertexNum();
    if (vertex_num == 0 || g_state.poser->pose_image.coordinates.size() < vertex_num) {
        return;
    }
    
    // Prepare vertex data from deformed coordinates
    std::vector<Vertex> vertices;
    vertices.reserve(vertex_num);
    
    // MMD models use centimeters, convert to meters (divide by 100)
    const float mmd_to_meter = 0.1f; // 10 cm = 0.1 m
    
    for (size_t i = 0; i < vertex_num; ++i) {
        mmd::Model::Vertex<mmd::ref> vertex = g_state.model->GetVertex(i);
        mmd::Vector2f uv = vertex.GetUVCoordinate();
        
        // Use deformed coordinates and normals from pose_image
        const mmd::Vector3f& pos = g_state.poser->pose_image.coordinates[i];
        const mmd::Vector3f& normal = g_state.poser->pose_image.normals[i];
        
        Vertex v;
        // Convert position from centimeters to meters
        v.pos[0] = pos.p.x * mmd_to_meter;
        v.pos[1] = pos.p.y * mmd_to_meter;
        v.pos[2] = pos.p.z * mmd_to_meter;
        // Normals don't need conversion (they're unit vectors)
        v.normal[0] = normal.p.x;
        v.normal[1] = normal.p.y;
        v.normal[2] = normal.p.z;
        v.uv[0] = uv.v[0];
        v.uv[1] = uv.v[1];
        
        vertices.push_back(v);
    }
    
    // Update vertex buffer with deformed data
    sg_update_buffer(g_state.vertex_buffer, sg_range{vertices.data(), vertices.size() * sizeof(Vertex)});
}

// Create ground plane geometry (white stage)
void CreateGroundGeometry() {
    // Large ground plane (50m x 50m in meters)
    const float size = 50.0f; // 50 meters
    Vertex ground_vertices[] = {
        // Position          Normal            UV
        {{-size, 0.0f, -size}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f}},
        {{ size, 0.0f, -size}, {0.0f, 1.0f, 0.0f}, {1.0f, 0.0f}},
        {{ size, 0.0f,  size}, {0.0f, 1.0f, 0.0f}, {1.0f, 1.0f}},
        {{-size, 0.0f,  size}, {0.0f, 1.0f, 0.0f}, {0.0f, 1.0f}}
    };
    
    uint32_t ground_indices[] = {
        0, 1, 2,
        2, 3, 0
    };
    
    sg_buffer_desc vbuf_desc = {};
    vbuf_desc.size = sizeof(ground_vertices);
    vbuf_desc.data.ptr = ground_vertices;
    vbuf_desc.data.size = sizeof(ground_vertices);
    vbuf_desc.label = "ground-vertices";
    g_state.ground_vertex_buffer = sg_make_buffer(&vbuf_desc);
    
    sg_buffer_desc ibuf_desc = {};
    ibuf_desc.usage.index_buffer = true;
    ibuf_desc.data.ptr = ground_indices;
    ibuf_desc.data.size = sizeof(ground_indices);
    ibuf_desc.label = "ground-indices";
    g_state.ground_index_buffer = sg_make_buffer(&ibuf_desc);
}

// Initialize shadow mapping resources
void InitializeShadowMapping() {
    // Create shadow map (depth texture)
    sg_image_desc shadow_desc = {};
    shadow_desc.type = SG_IMAGETYPE_2D;
    shadow_desc.usage.depth_stencil_attachment = true;
    shadow_desc.width = g_state.shadow_map_size;
    shadow_desc.height = g_state.shadow_map_size;
    shadow_desc.pixel_format = SG_PIXELFORMAT_DEPTH;
    shadow_desc.sample_count = 1;
    shadow_desc.label = "shadow-map";
    g_state.shadow_map = sg_make_image(&shadow_desc);
    
    // Create persistent views for shadow map (like official demo)
    // One view for texture sampling, one for depth attachment
    sg_view_desc shadow_tex_view_desc = {};
    shadow_tex_view_desc.texture.image = g_state.shadow_map;
    shadow_tex_view_desc.label = "shadow-map-tex-view";
    g_state.shadow_map_view = sg_make_view(&shadow_tex_view_desc);
    
    sg_view_desc shadow_ds_view_desc = {};
    shadow_ds_view_desc.depth_stencil_attachment.image = g_state.shadow_map;
    shadow_ds_view_desc.label = "shadow-map-depth-stencil-view";
    g_state.shadow_map_ds_view = sg_make_view(&shadow_ds_view_desc);
    
    // Create shadow sampler with comparison function (like official demo)
    // This allows hardware-accelerated depth comparison
    sg_sampler_desc shadow_sampler_desc = {};
    shadow_sampler_desc.min_filter = SG_FILTER_LINEAR;
    shadow_sampler_desc.mag_filter = SG_FILTER_LINEAR;
    shadow_sampler_desc.wrap_u = SG_WRAP_CLAMP_TO_EDGE;
    shadow_sampler_desc.wrap_v = SG_WRAP_CLAMP_TO_EDGE;
    shadow_sampler_desc.compare = SG_COMPAREFUNC_LESS;
    shadow_sampler_desc.label = "shadow-sampler";
    g_state.shadow_sampler = sg_make_sampler(&shadow_sampler_desc);
    
    // Create shadow shader and pipeline
    sg_shader shadow_shd = sg_make_shader(shadow_shadow_shader_desc(sg_query_backend()));
    sg_pipeline_desc shadow_pip_desc = {};
    shadow_pip_desc.shader = shadow_shd;
    shadow_pip_desc.layout.buffers[0].stride = sizeof(Vertex);
    shadow_pip_desc.layout.attrs[ATTR_shadow_shadow_position] = { .offset = 0, .format = SG_VERTEXFORMAT_FLOAT3 };
    shadow_pip_desc.depth.write_enabled = true;
    shadow_pip_desc.depth.compare = SG_COMPAREFUNC_LESS_EQUAL;
    shadow_pip_desc.depth.pixel_format = SG_PIXELFORMAT_DEPTH;
    shadow_pip_desc.cull_mode = SG_CULLMODE_FRONT; // render back-faces in shadow pass to prevent shadow acne on front-faces
    shadow_pip_desc.index_type = SG_INDEXTYPE_UINT32;
    shadow_pip_desc.primitive_type = SG_PRIMITIVETYPE_TRIANGLES;
    shadow_pip_desc.label = "shadow-pipeline";
    // No color output for shadow pass
    // shadow_pip_desc.colors[0].pixel_format = SG_PIXELFORMAT_NONE;
    shadow_pip_desc.colors[0].pixel_format = SG_PIXELFORMAT_R8;
    g_state.shadow_pip = sg_make_pipeline(&shadow_pip_desc);
    
    // Create persistent shadow pass (like official demo)
    g_state.shadow_pass_action.depth.load_action = SG_LOADACTION_CLEAR;
    g_state.shadow_pass_action.depth.store_action = SG_STOREACTION_STORE;
    g_state.shadow_pass_action.depth.clear_value = 1.0f;
    
    // Workaround for OpenGL: Create a dummy color attachment
    // In OpenGL, framebuffers with only depth attachments need glDrawBuffer(GL_NONE),
    // but sokol doesn't set this automatically. Adding a dummy color attachment ensures
    // glDrawBuffers is called correctly, allowing depth clear to work.
    sg_image_desc dummy_color_desc = {};
    dummy_color_desc.type = SG_IMAGETYPE_2D;
    dummy_color_desc.width = g_state.shadow_map_size;
    dummy_color_desc.height = g_state.shadow_map_size;
    dummy_color_desc.num_mipmaps = 1;
    dummy_color_desc.pixel_format = SG_PIXELFORMAT_R8;
    dummy_color_desc.usage.color_attachment = true;
    dummy_color_desc.label = "shadow-dummy-color";
    g_state.shadow_dummy_color = sg_make_image(&dummy_color_desc);
    
    sg_view_desc dummy_color_view_desc = {};
    dummy_color_view_desc.color_attachment.image = g_state.shadow_dummy_color;
    dummy_color_view_desc.label = "shadow-dummy-color-view";
    g_state.shadow_dummy_color_view = sg_make_view(&dummy_color_view_desc);

    std::cout << "Initialized shadow mapping (resolution: " << g_state.shadow_map_size << "x" << g_state.shadow_map_size << ")" << std::endl;
}

// Create skybox geometry (cube)
void CreateSkyboxGeometry() {
    // Cube vertices (positions only, no normals/UVs needed for skybox)
    float skybox_vertices[] = {
        // positions          
        -1.0f,  1.0f, -1.0f,
        -1.0f, -1.0f, -1.0f,
         1.0f, -1.0f, -1.0f,
         1.0f, -1.0f, -1.0f,
         1.0f,  1.0f, -1.0f,
        -1.0f,  1.0f, -1.0f,

        -1.0f, -1.0f,  1.0f,
        -1.0f, -1.0f, -1.0f,
        -1.0f,  1.0f, -1.0f,
        -1.0f,  1.0f, -1.0f,
        -1.0f,  1.0f,  1.0f,
        -1.0f, -1.0f,  1.0f,

         1.0f, -1.0f, -1.0f,
         1.0f, -1.0f,  1.0f,
         1.0f,  1.0f,  1.0f,
         1.0f,  1.0f,  1.0f,
         1.0f,  1.0f, -1.0f,
         1.0f, -1.0f, -1.0f,

        -1.0f, -1.0f,  1.0f,
        -1.0f,  1.0f,  1.0f,
         1.0f,  1.0f,  1.0f,
         1.0f,  1.0f,  1.0f,
         1.0f, -1.0f,  1.0f,
        -1.0f, -1.0f,  1.0f,

        -1.0f,  1.0f, -1.0f,
         1.0f,  1.0f, -1.0f,
         1.0f,  1.0f,  1.0f,
         1.0f,  1.0f,  1.0f,
        -1.0f,  1.0f,  1.0f,
        -1.0f,  1.0f, -1.0f,

        -1.0f, -1.0f, -1.0f,
        -1.0f, -1.0f,  1.0f,
         1.0f, -1.0f, -1.0f,
         1.0f, -1.0f, -1.0f,
        -1.0f, -1.0f,  1.0f,
         1.0f, -1.0f,  1.0f
    };
    
    sg_buffer_desc vbuf_desc = {};
    vbuf_desc.size = sizeof(skybox_vertices);
    vbuf_desc.data.ptr = skybox_vertices;
    vbuf_desc.data.size = sizeof(skybox_vertices);
    vbuf_desc.label = "skybox-vertices";
    g_state.skybox_vertex_buffer = sg_make_buffer(&vbuf_desc);
    
    // Skybox uses triangle list, no index buffer needed
    g_state.skybox_index_buffer = {0};
}

// Helper function to convert equirectangular UV to direction vector
HMM_Vec3 EquirectUVToDir(float u, float v) {
    float theta = u * 2.0f * 3.14159265359f;
    float phi = v * 3.14159265359f;
    float sinPhi = sinf(phi);
    return HMM_Vec3{
        cosf(theta) * sinPhi,
        cosf(phi),
        sinf(theta) * sinPhi
    };
}

// Load HDR image and convert to cubemap (CPU-side conversion)
bool LoadHDRAndCreateCubemap(const std::string& hdr_path) {
    int width, height, nrComponents;
    // With STBI_WINDOWS_UTF8, stbi_loadf can handle UTF-8 paths directly on Windows
    float* hdr_data = stbi_loadf(hdr_path.c_str(), &width, &height, &nrComponents, 0);
    if (!hdr_data) {
        const char* error = stbi_failure_reason();
        std::cerr << "Failed to load HDR image: " << hdr_path << std::endl;
        if (error) {
            std::cerr << "  Error: " << error << std::endl;
        }
        return false;
    }
    
    // Validate loaded image data
    if (width <= 0 || height <= 0) {
        std::cerr << "Invalid HDR image dimensions: " << width << "x" << height << std::endl;
        stbi_image_free(hdr_data);
        return false;
    }
    
    if (nrComponents < 3) {
        std::cerr << "HDR image must have at least 3 components (RGB), got: " << nrComponents << std::endl;
        stbi_image_free(hdr_data);
        return false;
    }
    
    std::cout << "Loaded HDR image: " << width << "x" << height << " (" << nrComponents << " components)" << std::endl;
    
    // Create equirectangular texture (for reference, but we'll convert to cubemap)
    sg_image_desc equirect_desc = {};
    equirect_desc.type = SG_IMAGETYPE_2D;
    equirect_desc.width = width;
    equirect_desc.height = height;
    equirect_desc.num_mipmaps = 1;
    equirect_desc.pixel_format = SG_PIXELFORMAT_RGBA32F;
    equirect_desc.usage.immutable = true;
    equirect_desc.label = "equirectangular-hdr";
    
    // Convert to RGBA if needed
    std::vector<float> rgba_data;
    if (nrComponents == 3) {
        rgba_data.resize(width * height * 4);
        for (int i = 0; i < width * height; ++i) {
            rgba_data[i * 4 + 0] = hdr_data[i * 3 + 0];
            rgba_data[i * 4 + 1] = hdr_data[i * 3 + 1];
            rgba_data[i * 4 + 2] = hdr_data[i * 3 + 2];
            rgba_data[i * 4 + 3] = 1.0f;
        }
    } else {
        rgba_data.assign(hdr_data, hdr_data + width * height * 4);
    }
    
    equirect_desc.data.mip_levels[0].ptr = rgba_data.data();
    equirect_desc.data.mip_levels[0].size = width * height * 4 * sizeof(float);
    g_state.equirectangular_map = sg_make_image(&equirect_desc);
    
    // Convert equirectangular to cubemap on CPU
    const int cubemap_size = 512;
    std::vector<std::vector<float>> cubemap_faces(6);
    
    // Standard OpenGL cubemap face layout:
    // Face 0: +X (right)   - looking in +X direction
    // Face 1: -X (left)    - looking in -X direction
    // Face 2: +Y (top)     - looking in +Y direction
    // Face 3: -Y (bottom)  - looking in -Y direction
    // Face 4: +Z (front)    - looking in +Z direction
    // Face 5: -Z (back)    - looking in -Z direction
    
    for (int face = 0; face < 6; ++face) {
        cubemap_faces[face].resize(cubemap_size * cubemap_size * 4);
        
        for (int y = 0; y < cubemap_size; ++y) {
            for (int x = 0; x < cubemap_size; ++x) {
                // Convert cubemap UV to direction vector
                // UV range: [0, 1] -> [-1, 1]
                float u = (x + 0.5f) / cubemap_size * 2.0f - 1.0f;
                float v = (y + 0.5f) / cubemap_size * 2.0f - 1.0f;

                HMM_Vec3 dir;
                // Map UV to direction based on cubemap face
                // sokol cubemap layout (standard OpenGL-style)
                switch (face) {
                    case 0: // +X (right) - positive X axis
                        dir = HMM_Vec3{-1.0f, v, -u};
                        break;
                    case 1: // -X (left) - negative X axis
                        dir = HMM_Vec3{1.0f, v, u};
                        break;
                    case 2: // +Y (top) - positive Y axis
                        dir = HMM_Vec3{-u, 1.0f, -v};
                        break;
                    case 3: // -Y (bottom) - negative Y axis
                        dir = HMM_Vec3{-u, -1.0f, v};
                        break;
                    case 4: // +Z (front) - positive Z axis
                        dir = HMM_Vec3{-u, v, 1.0f};
                        break;
                    case 5: // -Z (back) - negative Z axis
                        dir = HMM_Vec3{u, v, -1.0f};
                        break;
                }
                dir = HMM_NormV3(dir);
                
                // Convert direction to equirectangular UV
                // Equirectangular mapping: theta (azimuth) and phi (elevation)
                float theta = atan2f(dir.Z, dir.X);  // azimuth: [-PI, PI]
                float phi = acosf(dir.Y);             // elevation: [0, PI]
                
                // Normalize to [0, 1]
                float equirect_u = (theta / (2.0f * 3.14159265359f)) + 0.5f;
                float equirect_v = phi / 3.14159265359f;
                
                // Clamp to valid range
                if (equirect_u < 0.0f) equirect_u = 0.0f;
                if (equirect_u > 1.0f) equirect_u = 1.0f;
                if (equirect_v < 0.0f) equirect_v = 0.0f;
                if (equirect_v > 1.0f) equirect_v = 1.0f;
                
                // Sample from equirectangular map
                int src_x = (int)(equirect_u * width);
                int src_y = (int)(equirect_v * height);
                if (src_x >= width) src_x = width - 1;
                if (src_y >= height) src_y = height - 1;
                int src_idx = (src_y * width + src_x) * 4;
                
                // Flip Y coordinate when writing to cubemap to match sokol's image coordinate system
                // sokol uses top-left origin, so we need to flip Y
                int dst_y = cubemap_size - 1 - y;
                int dst_idx = (dst_y * cubemap_size + x) * 4;
                cubemap_faces[face][dst_idx + 0] = rgba_data[src_idx + 0];
                cubemap_faces[face][dst_idx + 1] = rgba_data[src_idx + 1];
                cubemap_faces[face][dst_idx + 2] = rgba_data[src_idx + 2];
                cubemap_faces[face][dst_idx + 3] = rgba_data[src_idx + 3];
            }
        }
    }
    
    // Create cubemap texture
    // For cubemap, all 6 faces are packed into a single mip level
    // The data should be: face0, face1, face2, face3, face4, face5
    std::vector<float> cubemap_packed;
    cubemap_packed.reserve(cubemap_size * cubemap_size * 4 * 6);
    for (int i = 0; i < 6; ++i) {
        cubemap_packed.insert(cubemap_packed.end(), cubemap_faces[i].begin(), cubemap_faces[i].end());
    }
    
    sg_image_desc cubemap_desc = {};
    cubemap_desc.type = SG_IMAGETYPE_CUBE;
    cubemap_desc.width = cubemap_size;
    cubemap_desc.height = cubemap_size;
    cubemap_desc.num_slices = 6;
    cubemap_desc.num_mipmaps = 1;
    cubemap_desc.pixel_format = SG_PIXELFORMAT_RGBA32F;
    cubemap_desc.usage.immutable = true;
    cubemap_desc.label = "environment-cubemap";
    cubemap_desc.data.mip_levels[0].ptr = cubemap_packed.data();
    cubemap_desc.data.mip_levels[0].size = cubemap_size * cubemap_size * 4 * sizeof(float) * 6;
    g_state.environment_cubemap = sg_make_image(&cubemap_desc);
    
    // Create persistent view for environment cubemap
    sg_view_desc env_cubemap_view_desc = {};
    env_cubemap_view_desc.texture.image = g_state.environment_cubemap;
    g_state.environment_cubemap_view = sg_make_view(&env_cubemap_view_desc);
    
    stbi_image_free(hdr_data);
    
    std::cout << "Created environment cubemap from HDR" << std::endl;
    return true;
}

// ImGuizmo helper functions following official demo pattern
void TransformStart(float* cameraView, float* cameraProjection, float* matrix) {
    // Identity matrix for DrawGrid (column-major)
    static const float identityMatrix[16] = {
        1.f, 0.f, 0.f, 0.f,
        0.f, 1.f, 0.f, 0.f,
        0.f, 0.f, 1.f, 0.f,
        0.f, 0.f, 0.f, 1.f
    };

    ImGuiIO& io = ImGui::GetIO();
    float viewManipulateRight = io.DisplaySize.x;
    float viewManipulateTop = 0;
    static ImGuiWindowFlags gizmoWindowFlags = 0;
    
    // We don't use a window (useWindow = false), so draw directly to screen
    bool useWindow = false;
    
    if (useWindow) {
        ImGui::SetNextWindowSize(ImVec2(800, 400), ImGuiCond_Appearing);
        ImGui::SetNextWindowPos(ImVec2(400, 20), ImGuiCond_Appearing);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, (ImVec4)ImColor(0.35f, 0.3f, 0.3f));
        ImGui::Begin("Gizmo", 0, gizmoWindowFlags);
        ImGuizmo::SetDrawlist();
    }
    // When useWindow = false, don't call SetDrawlist() - BeginFrame() already set it
    // This matches the official demo behavior
    
    float windowWidth = useWindow ? (float)ImGui::GetWindowWidth() : io.DisplaySize.x;
    float windowHeight = useWindow ? (float)ImGui::GetWindowHeight() : io.DisplaySize.y;

    if (!useWindow) {
        ImGuizmo::SetRect(0, 0, io.DisplaySize.x, io.DisplaySize.y);
    } else {
        ImGuizmo::SetRect(ImGui::GetWindowPos().x, ImGui::GetWindowPos().y, windowWidth, windowHeight);
    }
    
    viewManipulateRight = useWindow ? (ImGui::GetWindowPos().x + windowWidth) : io.DisplaySize.x;
    viewManipulateTop = useWindow ? ImGui::GetWindowPos().y : 0;
    
    if (useWindow) {
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        gizmoWindowFlags = ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(window->InnerRect.Min, window->InnerRect.Max) ? ImGuiWindowFlags_NoMove : 0;
    } else {
        // When useWindow = false, we need to ensure we have a valid drawlist
        // BeginFrame() creates a hidden window, but we should use foreground drawlist for full-screen
        // Actually, let's not call SetDrawlist here - BeginFrame() should have set it
        // But we need to make sure it's still valid
    }

    // Draw helper grid and cubes (only if enabled)
    // Grid size in meters (1 meter per grid cell)
    if (g_state.guizmo_draw_grid) {
        ImGuizmo::DrawGrid(cameraView, cameraProjection, identityMatrix, 1.0f);
    }
    
    // Draw cube at model position
    // int gizmoCount = 1;
    // ImGuizmo::DrawCubes(cameraView, cameraProjection, matrix, gizmoCount);

    // View manipulate widget (camera control in top-right corner)
    // ViewManipulate modifies the view matrix directly, so we need to update camera parameters after it
    ImGuizmo::ViewManipulate(cameraView, g_state.camera_distance, ImVec2(viewManipulateRight - 128, viewManipulateTop), ImVec2(128, 128), 0x10101010);
}

void TransformEnd() {
    bool useWindow = false;
    if (useWindow) {
        ImGui::End();
        ImGui::PopStyleColor(1);
    }
}

void EditTransform(float* cameraView, float* cameraProjection, float* matrix) {
    ImGuiIO& io = ImGui::GetIO();
    bool useWindow = false;
    
    // Ensure drawlist is set before calling Manipulate
    // When useWindow = false, BeginFrame() should have set it, but we need to ensure it's still valid
    if (!useWindow) {
        // Use foreground drawlist for full-screen gizmo to ensure it's always valid
        ImGuizmo::SetDrawlist(ImGui::GetForegroundDrawList());
        ImGuizmo::SetRect(0, 0, io.DisplaySize.x, io.DisplaySize.y);
    } else {
        float windowWidth = (float)ImGui::GetWindowWidth();
        float windowHeight = (float)ImGui::GetWindowHeight();
        ImGuizmo::SetRect(ImGui::GetWindowPos().x, ImGui::GetWindowPos().y, windowWidth, windowHeight);
    }
    
    // Enable gizmo (must be called before Manipulate)
    ImGuizmo::Enable(true);
    
    // Call Manipulate - this draws the gizmo
    ImGuizmo::Manipulate(cameraView, cameraProjection, g_state.guizmo_operation, g_state.guizmo_mode, matrix, NULL, g_state.guizmo_use_snap ? g_state.guizmo_snap : NULL);
}

// Initialization function
void init(void) {
    // Setup sokol-gfx (following triangle-sapp example, using C++ compatible syntax)
    sg_desc _sg_desc = {};
    _sg_desc.environment = sglue_environment();
    _sg_desc.logger.func = slog_func;
    sg_setup(&_sg_desc);
    
    // Setup ImGui
    simgui_desc_t _simgui_desc = {};
    _simgui_desc.logger.func = slog_func;
    simgui_setup(&_simgui_desc);
    
    // Setup sokol-gfx ImGui debug UI
    sgimgui_desc_t _sgimgui_desc = {};
    sgimgui_init(&g_state.sgimgui, &_sgimgui_desc);
    
    // Create shader (using generated shader descriptor)
    sg_shader shd = sg_make_shader(mmd_mmd_shader_desc(sg_query_backend()));
    
    // Create render pipeline
    sg_pipeline_desc _sg_pipeline_desc{};
    _sg_pipeline_desc.shader = shd;
    
    // Set vertex layout with stride
    _sg_pipeline_desc.layout.buffers[0].stride = sizeof(Vertex);
    _sg_pipeline_desc.layout.attrs[ATTR_mmd_mmd_position] =  { .offset = 0, .format = SG_VERTEXFORMAT_FLOAT3 };
    _sg_pipeline_desc.layout.attrs[ATTR_mmd_mmd_normal] = { .offset = sizeof(float) * 3, .format = SG_VERTEXFORMAT_FLOAT3 };
    _sg_pipeline_desc.layout.attrs[ATTR_mmd_mmd_texcoord0] = { .offset = sizeof(float) * 6, .format = SG_VERTEXFORMAT_FLOAT2 };
    
    _sg_pipeline_desc.depth.write_enabled = true;
    _sg_pipeline_desc.depth.compare = SG_COMPAREFUNC_LESS_EQUAL;
    _sg_pipeline_desc.cull_mode = SG_CULLMODE_BACK;
    _sg_pipeline_desc.index_type = SG_INDEXTYPE_UINT32;
    _sg_pipeline_desc.primitive_type = SG_PRIMITIVETYPE_TRIANGLES;
    _sg_pipeline_desc.label = "model-pipeline";

    g_state.pip = sg_make_pipeline(&_sg_pipeline_desc);
    
    // Set clear color
    g_state.main_pass_action.colors[0] = { .load_action = SG_LOADACTION_CLEAR, .clear_value = {0.1f,0.1f, 0.15f, 1.0f} };
    g_state.main_pass_action.depth = { .load_action = SG_LOADACTION_CLEAR, .clear_value = 1.0f };

    // Set UI Pass
    g_state.ui_pass_action.colors[0] = { .load_action = SG_LOADACTION_LOAD };

    // Initialize time
    stm_setup();
    
    // Create skybox geometry
    CreateSkyboxGeometry();
    
    // Create ground geometry
    CreateGroundGeometry();
    
    // Initialize shadow mapping
    InitializeShadowMapping();
    
    // Create ground pipeline (uses dedicated ground shader with shadows)
    sg_shader ground_shd = sg_make_shader(ground_ground_shader_desc(sg_query_backend()));
    sg_pipeline_desc ground_pip_desc = {};
    ground_pip_desc.shader = ground_shd;
    ground_pip_desc.layout.buffers[0].stride = sizeof(Vertex);
    ground_pip_desc.layout.attrs[ATTR_ground_ground_position] = { .offset = 0, .format = SG_VERTEXFORMAT_FLOAT3 };
    ground_pip_desc.layout.attrs[ATTR_ground_ground_normal] = { .offset = sizeof(float) * 3, .format = SG_VERTEXFORMAT_FLOAT3 };
    ground_pip_desc.layout.attrs[ATTR_ground_ground_texcoord0] = { .offset = sizeof(float) * 6, .format = SG_VERTEXFORMAT_FLOAT2 };
    ground_pip_desc.depth.write_enabled = true;
    ground_pip_desc.depth.compare = SG_COMPAREFUNC_LESS_EQUAL;
    ground_pip_desc.cull_mode = SG_CULLMODE_BACK;
    ground_pip_desc.index_type = SG_INDEXTYPE_UINT32;
    ground_pip_desc.primitive_type = SG_PRIMITIVETYPE_TRIANGLES;
    ground_pip_desc.label = "ground-pipeline";
    g_state.ground_pip = sg_make_pipeline(&ground_pip_desc);
    
    // Create default sampler for textures
    sg_sampler_desc sampler_desc = {};
    sampler_desc.min_filter = SG_FILTER_LINEAR;
    sampler_desc.mag_filter = SG_FILTER_LINEAR;
    sampler_desc.wrap_u = SG_WRAP_REPEAT;  // For textures, use repeat
    sampler_desc.wrap_v = SG_WRAP_REPEAT;
    sampler_desc.wrap_w = SG_WRAP_CLAMP_TO_EDGE;
    sampler_desc.label = "default-sampler";
    g_state.default_sampler = sg_make_sampler(&sampler_desc);
    
    // Create default white texture (1x1 white RGBA)
    unsigned char white_pixel[4] = {255, 255, 255, 255};
    sg_image_desc default_tex_desc = {};
    default_tex_desc.type = SG_IMAGETYPE_2D;
    default_tex_desc.width = 1;
    default_tex_desc.height = 1;
    default_tex_desc.num_mipmaps = 1;
    default_tex_desc.pixel_format = SG_PIXELFORMAT_RGBA8;
    default_tex_desc.usage.immutable = true;
    default_tex_desc.label = "default-white-texture";
    default_tex_desc.data.mip_levels[0].ptr = white_pixel;
    default_tex_desc.data.mip_levels[0].size = 4;
    g_state.default_texture = sg_make_image(&default_tex_desc);
    
    // Create persistent view for default texture
    sg_view_desc default_view_desc = {};
    default_view_desc.texture.image = g_state.default_texture;
    g_state.default_texture_view = sg_make_view(&default_view_desc);
    
    // Create skybox shader and pipeline
    sg_shader skybox_shd = sg_make_shader(ibl_skybox_shader_desc(sg_query_backend()));
    sg_pipeline_desc skybox_pip_desc = {};
    skybox_pip_desc.shader = skybox_shd;
    skybox_pip_desc.layout.attrs[ATTR_ibl_skybox_position].format = SG_VERTEXFORMAT_FLOAT3;
    skybox_pip_desc.depth.write_enabled = false;
    skybox_pip_desc.depth.compare = SG_COMPAREFUNC_LESS_EQUAL;
    skybox_pip_desc.cull_mode = SG_CULLMODE_FRONT;
    skybox_pip_desc.primitive_type = SG_PRIMITIVETYPE_TRIANGLES;
    skybox_pip_desc.label = "skybox-pipeline";
    g_state.skybox_pip = sg_make_pipeline(&skybox_pip_desc);
    
    // Load HDR and create cubemap
    std::string hdr_path = "assets/hdr/modern_evening_street_2k.hdr";
    if (LoadHDRAndCreateCubemap(hdr_path)) {
        g_state.ibl_initialized = true;
        std::cout << "IBL initialized successfully" << std::endl;
    } else {
        std::cerr << "Failed to initialize IBL" << std::endl;
    }
    
    // Try to load model and motion files if they exist
    if (!g_state.model_filename.empty()) {
        LoadPMXModel(g_state.model_filename);
        if (g_state.model_loaded) {
            UpdateModelBuffers();
        }
    }
    if (!g_state.motion_filename.empty()) {
        LoadVMDMotion(g_state.motion_filename);
    }
    
    std::cout << "MMD Renderer initialized" << std::endl;
    std::cout << "Usage: Load PMX and VMD files via code or command line" << std::endl;
}

// Frame update function
void frame(void) {
    const float dt = sapp_frame_duration();
    
    int width = sapp_width();
    int height = sapp_height();
    
    // Setup ImGui frame
    simgui_frame_desc_t simgui_frame = {};
    simgui_frame.width = width;
    simgui_frame.height = height;
    simgui_frame.delta_time = dt;
    simgui_frame.dpi_scale = sapp_dpi_scale();
    simgui_new_frame(&simgui_frame);
    
    // Begin ImGuizmo frame (must be called after ImGui new frame)
    ImGuizmo::BeginFrame();
    
    // Draw menu bar with file operations and debug options
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Open PMX Model...")) {
                nfdu8char_t* outPath = nullptr;
                nfdu8filteritem_t filterItem[1] = {{"PMX Model Files", "pmx"}};
                nfdresult_t result = NFD_OpenDialogU8(&outPath, filterItem, 1, nullptr);
                
                if (result == NFD_OKAY) {
                    std::string filename(outPath);
                    if (LoadPMXModel(filename)) {
                        if (g_state.model_loaded) {
                            UpdateModelBuffers();
                            // Reset animation time when loading new model
                            g_state.time = 0.0f;
                        }
                    }
                    NFD_FreePathU8(outPath);
                } else if (result == NFD_CANCEL) {
                    // User cancelled, do nothing
                } else {
                    std::cerr << "Error opening file dialog: " << NFD_GetError() << std::endl;
                }
            }
            if (ImGui::MenuItem("Open VMD Motion...")) {
                nfdu8char_t* outPath = nullptr;
                nfdu8filteritem_t filterItem[1] = {{"VMD Motion Files", "vmd"}};
                nfdresult_t result = NFD_OpenDialogU8(&outPath, filterItem, 1, nullptr);
                
                if (result == NFD_OKAY) {
                    std::string filename(outPath);
                    if (LoadVMDMotion(filename)) {
                        // Reset animation time when loading new motion
                        g_state.time = 0.0f;
                    }
                    NFD_FreePathU8(outPath);
                } else if (result == NFD_CANCEL) {
                    // User cancelled, do nothing
                } else {
                    std::cerr << "Error opening file dialog: " << NFD_GetError() << std::endl;
                }
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("sokol-gfx")) {
            ImGui::MenuItem("Capabilities", nullptr, &g_state.sgimgui.caps_window.open);
            ImGui::MenuItem("Frame Stats", nullptr, &g_state.sgimgui.frame_stats_window.open);
            ImGui::MenuItem("Buffers", nullptr, &g_state.sgimgui.buffer_window.open);
            ImGui::MenuItem("Images", nullptr, &g_state.sgimgui.image_window.open);
            ImGui::MenuItem("Samplers", nullptr, &g_state.sgimgui.sampler_window.open);
            ImGui::MenuItem("Shaders", nullptr, &g_state.sgimgui.shader_window.open);
            ImGui::MenuItem("Pipelines", nullptr, &g_state.sgimgui.pipeline_window.open);
            ImGui::MenuItem("Views", nullptr, &g_state.sgimgui.view_window.open);
            ImGui::MenuItem("Calls", nullptr, &g_state.sgimgui.capture_window.open);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Camera")) {
            ImGui::MenuItem("Camera Controls", nullptr, &g_state.camera_window_open);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("IBL")) {
            ImGui::MenuItem("Show Skybox", nullptr, &g_state.show_skybox);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Light")) {
            ImGui::MenuItem("Light Controls", nullptr, &g_state.light_window_open);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Tools")) {
            ImGui::MenuItem("Model Transform (ImGuizmo)", nullptr, &g_state.guizmo_enabled);
            ImGui::MenuItem("Animation Sequencer", nullptr, &g_state.sequencer_enabled);
            ImGui::EndMenu();
        }
        if (g_state.guizmo_enabled && ImGui::BeginMenu("Gizmo Debug")) {
            ImGui::MenuItem("Gizmo Controls", nullptr, &g_state.guizmo_debug_window);
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }
    
    // Draw camera debug window
    if (g_state.camera_window_open) {
        if (ImGui::Begin("Camera Controls", &g_state.camera_window_open)) {
            ImGui::Text("Camera Position");
            ImGui::DragFloat3("Position", &g_state.camera_pos.X, 0.1f);
            
            ImGui::Text("Camera Target");
            ImGui::DragFloat3("Target", &g_state.camera_target.X, 0.1f);
            
            ImGui::Separator();
            ImGui::Text("Camera Settings");
            ImGui::DragFloat("FOV", &g_state.camera_fov, 1.0f, 10.0f, 120.0f);
            ImGui::DragFloat("Distance (m)", &g_state.camera_distance, 0.1f, 0.5f, 20.0f);
            ImGui::DragFloat("Rotation X", &g_state.camera_rotation_x, 0.01f, -3.14f, 3.14f);
            ImGui::DragFloat("Rotation Y", &g_state.camera_rotation_y, 0.01f, -1.57f, 1.57f);
            
            ImGui::Separator();
            ImGui::Text("Controls:");
            ImGui::BulletText("Left Mouse Button: Rotate camera");
            ImGui::BulletText("Middle Mouse Button: Pan camera");
            ImGui::BulletText("Mouse Wheel: Zoom in/out");
            ImGui::BulletText("WASD: Move camera");
            ImGui::BulletText("R: Reset camera");
            
            if (ImGui::Button("Reset Camera")) {
                g_state.camera_pos = HMM_Vec3{0.0f, 1.6f, 4.0f}; // ~1.6m height, 4m away
                g_state.camera_target = HMM_Vec3{0.0f, 0.0f, 0.0f};
                g_state.camera_fov = 45.0f;
                g_state.camera_distance = 4.0f; // 4 meters
                g_state.camera_rotation_x = 0.0f;
                g_state.camera_rotation_y = 0.0f;
            }
        }
        ImGui::End();
    }
    
    // Draw light debug window
    if (g_state.light_window_open) {
        if (ImGui::Begin("Light Controls", &g_state.light_window_open)) {
            ImGui::Text("Directional Light (Sun/Sky Light)");
            ImGui::Separator();
            
            // Light direction (normalized)
            float light_dir[3] = {g_state.light_direction.X, g_state.light_direction.Y, g_state.light_direction.Z};
            if (ImGui::DragFloat3("Direction", light_dir, 0.01f, -1.0f, 1.0f)) {
                g_state.light_direction = HMM_Vec3{light_dir[0], light_dir[1], light_dir[2]};
                // Normalize direction
                float len = HMM_LenV3(g_state.light_direction);
                if (len > 0.001f) {
                    g_state.light_direction = HMM_DivV3F(g_state.light_direction, len);
                }
            }
            
            // Light color
            float light_col[3] = {g_state.light_color.X, g_state.light_color.Y, g_state.light_color.Z};
            if (ImGui::ColorEdit3("Color", light_col)) {
                g_state.light_color = HMM_Vec3{light_col[0], light_col[1], light_col[2]};
            }
            
            // Light intensity
            ImGui::DragFloat("Intensity", &g_state.light_intensity, 0.1f, 0.0f, 10.0f);
            
            ImGui::Separator();
            ImGui::Checkbox("Enable Shadows", &g_state.shadows_enabled);
            
            ImGui::Separator();
            ImGui::Text("Figure/Resin Material");
            ImGui::Separator();
            
            // Rim light parameters
            ImGui::Text("Rim Light (Edge Highlight):");
            ImGui::DragFloat("Rim Power", &g_state.rim_power, 0.1f, 1.0f, 10.0f);
            ImGui::DragFloat("Rim Intensity", &g_state.rim_intensity, 0.1f, 0.0f, 3.0f);
            float rim_col[3] = {g_state.rim_color.X, g_state.rim_color.Y, g_state.rim_color.Z};
            if (ImGui::ColorEdit3("Rim Color", rim_col)) {
                g_state.rim_color = HMM_Vec3{rim_col[0], rim_col[1], rim_col[2]};
            }
            
            ImGui::Separator();
            
            // Specular highlight parameters
            ImGui::Text("Specular Highlight:");
            ImGui::DragFloat("Specular Power", &g_state.specular_power, 1.0f, 1.0f, 256.0f);
            ImGui::DragFloat("Specular Intensity", &g_state.specular_intensity, 0.1f, 0.0f, 3.0f);
            
            ImGui::Separator();
            ImGui::Text("Light Info:");
            ImGui::Text("Direction: (%.3f, %.3f, %.3f)", 
                g_state.light_direction.X, g_state.light_direction.Y, g_state.light_direction.Z);
            ImGui::Text("Color: (%.3f, %.3f, %.3f)", 
                g_state.light_color.X, g_state.light_color.Y, g_state.light_color.Z);
            ImGui::Text("Intensity: %.2f", g_state.light_intensity);
        }
        ImGui::End();
    }
    
    // Draw sokol-gfx debug windows
    sgimgui_draw(&g_state.sgimgui);
    
    // Draw ImGuizmo debug window
    if (g_state.guizmo_debug_window && g_state.guizmo_enabled) {
        if (ImGui::Begin("Gizmo Debug", &g_state.guizmo_debug_window)) {
            ImGui::Text("Gizmo State:");
            ImGui::Text("IsOver: %s", ImGuizmo::IsOver() ? "Yes" : "No");
            ImGui::Text("IsUsing: %s", ImGuizmo::IsUsing() ? "Yes" : "No");
            ImGui::Text("IsOver(TRANSLATE): %s", ImGuizmo::IsOver(ImGuizmo::TRANSLATE) ? "Yes" : "No");
            ImGui::Text("IsOver(ROTATE): %s", ImGuizmo::IsOver(ImGuizmo::ROTATE) ? "Yes" : "No");
            ImGui::Text("IsOver(SCALE): %s", ImGuizmo::IsOver(ImGuizmo::SCALE) ? "Yes" : "No");
            
            ImGui::Separator();
            ImGui::Text("Operation:");
            if (ImGui::RadioButton("Translate", g_state.guizmo_operation == ImGuizmo::TRANSLATE)) {
                g_state.guizmo_operation = ImGuizmo::TRANSLATE;
            }
            ImGui::SameLine();
            if (ImGui::RadioButton("Rotate", g_state.guizmo_operation == ImGuizmo::ROTATE)) {
                g_state.guizmo_operation = ImGuizmo::ROTATE;
            }
            ImGui::SameLine();
            if (ImGui::RadioButton("Scale", g_state.guizmo_operation == ImGuizmo::SCALE)) {
                g_state.guizmo_operation = ImGuizmo::SCALE;
            }
            
            ImGui::Separator();
            ImGui::Text("Mode:");
            if (ImGui::RadioButton("Local", g_state.guizmo_mode == ImGuizmo::LOCAL)) {
                g_state.guizmo_mode = ImGuizmo::LOCAL;
            }
            ImGui::SameLine();
            if (ImGui::RadioButton("World", g_state.guizmo_mode == ImGuizmo::WORLD)) {
                g_state.guizmo_mode = ImGuizmo::WORLD;
            }
            
            ImGui::Separator();
            ImGui::Checkbox("Draw Grid", &g_state.guizmo_draw_grid);
            
            ImGui::Separator();
            ImGui::Text("Model Matrix (column-major):");
            ImGui::InputFloat4("Col 0", &g_state.model_matrix[0]);
            ImGui::InputFloat4("Col 1", &g_state.model_matrix[4]);
            ImGui::InputFloat4("Col 2", &g_state.model_matrix[8]);
            ImGui::InputFloat4("Col 3", &g_state.model_matrix[12]);
            
            if (ImGui::Button("Reset Matrix")) {
                g_state.model_matrix[0] = 1.0f; g_state.model_matrix[1] = 0.0f; g_state.model_matrix[2] = 0.0f; g_state.model_matrix[3] = 0.0f;
                g_state.model_matrix[4] = 0.0f; g_state.model_matrix[5] = 1.0f; g_state.model_matrix[6] = 0.0f; g_state.model_matrix[7] = 0.0f;
                g_state.model_matrix[8] = 0.0f; g_state.model_matrix[9] = 0.0f; g_state.model_matrix[10] = 1.0f; g_state.model_matrix[11] = 0.0f;
                g_state.model_matrix[12] = 0.0f; g_state.model_matrix[13] = 0.0f; g_state.model_matrix[14] = 0.0f; g_state.model_matrix[15] = 1.0f;
            }
        }
        ImGui::End();
    }
    
    // Update animation time (only if playing and not manually controlled by sequencer)
    if (g_state.animation_playing && !g_state.sequencer_manual_control) {
        g_state.time += dt;
    }
    
    // Update sequencer current frame from animation time (sync sequencer with animation)
    // This happens BEFORE sequencer window is drawn, so sequencer can detect manual changes
    if (g_state.motion_loaded && g_state.motion_player) {
        int current_frame_from_time = static_cast<int>(g_state.time * 30.0f);
        
        // Initialize sequencer_last_frame if not set (first time sequencer is used)
        if (g_state.sequencer_last_frame < 0) {
            g_state.sequencer_last_frame = current_frame_from_time;
            g_state.sequencer_current_frame = current_frame_from_time;
        }
        
        // Only update sequencer frame if animation is playing and not manually controlled
        if (g_state.animation_playing && !g_state.sequencer_manual_control) {
            g_state.sequencer_current_frame = current_frame_from_time;
            // Update last_frame to match, so we don't detect this as a manual change
            g_state.sequencer_last_frame = g_state.sequencer_current_frame;
        }
        
        // Reset manual control flag if sequencer is disabled
        if (!g_state.sequencer_enabled) {
            g_state.sequencer_manual_control = false;
        }
    }
    
    // Update animation and deformed vertices
    // Ensure Deform() is called before UpdateDeformedVertices() to populate pose_image
    if (g_state.model_loaded && g_state.poser) {
        // Reset posing first (clears all bone poses and morphs)
        g_state.poser->ResetPosing();
        
        // Then apply motion if available
        if (g_state.motion_loaded && g_state.motion_player) {
            // Calculate current frame (assuming 30 FPS)
            size_t frame = static_cast<size_t>(g_state.time * 30.0f);
            
            // Seek to current frame and apply motion (sets bone poses and morphs)
            g_state.motion_player->SeekFrame(frame);
            
            // After setting bone poses, we need to update bone transforms
            // ResetPosing() already called PrePhysicsPosing() and PostPhysicsPosing(),
            // but after SeekFrame() we need to update transforms again
            g_state.poser->PrePhysicsPosing();
            g_state.poser->PostPhysicsPosing();
            
            // // Debug: print frame number every second
            // static size_t last_frame = 0;
            // if (frame != last_frame && frame % 30 == 0) {
            //     std::cout << "Animation frame: " << frame << " (time: " << g_state.time << "s)" << std::endl;
            //     last_frame = frame;
            // }
        }
        
        // Apply deformation (calculates deformed vertex positions)
        g_state.poser->Deform();
        
        // Update vertex buffer with deformed vertices (only once per frame)
        UpdateDeformedVertices();
    }
    
    // Handle continuous keyboard input for camera movement (WASD)
    // Move camera target, camera position will be updated based on orbit
    if (!ImGui::GetIO().WantCaptureKeyboard) {
        const float move_speed = 2.0f;  // meters per second (reasonable walking speed)
        HMM_Vec3 move_dir = HMM_Vec3{0.0f, 0.0f, 0.0f};
        
        // Get forward and right vectors from camera view direction
        float cos_y = cosf(g_state.camera_rotation_y);
        float sin_y = sinf(g_state.camera_rotation_y);
        float cos_x = cosf(g_state.camera_rotation_x);
        float sin_x = sinf(g_state.camera_rotation_x);
        
        // Forward vector (camera looking direction)
        HMM_Vec3 forward;
        forward.X = -cos_y * sin_x;
        forward.Y = -sin_y;
        forward.Z = -cos_y * cos_x;
        
        // Right vector (perpendicular to forward and up)
        HMM_Vec3 right;
        right.X = cos_x;
        right.Y = 0.0f;
        right.Z = -sin_x;
        
        // Up vector
        HMM_Vec3 up = HMM_Vec3{0.0f, 1.0f, 0.0f};
        
        // Check keyboard state
        if (g_state.keys_down[SAPP_KEYCODE_W]) {
            move_dir = HMM_Add(move_dir, forward);
        }
        if (g_state.keys_down[SAPP_KEYCODE_S]) {
            move_dir = HMM_Sub(move_dir, forward);
        }
        if (g_state.keys_down[SAPP_KEYCODE_A]) {
            move_dir = HMM_Sub(move_dir, right);
        }
        if (g_state.keys_down[SAPP_KEYCODE_D]) {
            move_dir = HMM_Add(move_dir, right);
        }
        if (g_state.keys_down[SAPP_KEYCODE_Q]) {
            move_dir = HMM_Sub(move_dir, up);
        }
        if (g_state.keys_down[SAPP_KEYCODE_E]) {
            move_dir = HMM_Add(move_dir, up);
        }
        
        // Apply movement to camera target
        float move_len = HMM_LenV3(move_dir);
        if (move_len > 0.001f) {
            move_dir = HMM_DivV3F(move_dir, move_len);
            HMM_Vec3 move = HMM_MulV3F(move_dir, move_speed * dt);
            g_state.camera_target = HMM_Add(g_state.camera_target, move);
        }
    }
    
    // Update camera position based on rotation and distance (orbit camera)
    float cos_y = cosf(g_state.camera_rotation_y);
    float sin_y = sinf(g_state.camera_rotation_y);
    float cos_x = cosf(g_state.camera_rotation_x);
    float sin_x = sinf(g_state.camera_rotation_x);
    
    HMM_Vec3 camera_offset;
    camera_offset.X = g_state.camera_distance * cos_y * sin_x;
    camera_offset.Y = g_state.camera_distance * sin_y;
    camera_offset.Z = g_state.camera_distance * cos_y * cos_x;
    
    g_state.camera_pos = HMM_Add(g_state.camera_target, camera_offset);
    
    // Calculate MVP matrix
    HMM_Mat4 proj = HMM_Perspective_RH_ZO(g_state.camera_fov * HMM_DegToRad, (float)width / (float)height, 0.1f, 1000.0f);
    HMM_Mat4 view = HMM_LookAt_RH(g_state.camera_pos, g_state.camera_target, HMM_Vec3{0.0f, 1.0f, 0.0f});
    
    // Convert model matrix from ImGuizmo format (column-major float array) to HMM_Mat4
    HMM_Mat4 model_mat = HMM_M4D(1.0f);
    if (g_state.guizmo_enabled && g_state.model_loaded) {
        // ImGuizmo uses column-major format: array[col*4 + row]
        // The array layout in memory (column-major) is:
        //   array[0]  array[4]  array[8]   array[12]   <- row 0
        //   array[1]  array[5]  array[9]   array[13]   <- row 1
        //   array[2]  array[6]  array[10]  array[14]   <- row 2
        //   array[3]  array[7]  array[11]  array[15]   <- row 3
        // HMM Elements[row][col] stores matrix in column-major order internally
        // So: Elements[row][col] = array[col*4 + row]
        // Column 0 (stored at indices 0-3)
        model_mat.Elements[0][0] = g_state.model_matrix[0];
        model_mat.Elements[1][0] = g_state.model_matrix[1];
        model_mat.Elements[2][0] = g_state.model_matrix[2];
        model_mat.Elements[3][0] = g_state.model_matrix[3];
        // Column 1 (stored at indices 4-7)
        model_mat.Elements[0][1] = g_state.model_matrix[4];
        model_mat.Elements[1][1] = g_state.model_matrix[5];
        model_mat.Elements[2][1] = g_state.model_matrix[6];
        model_mat.Elements[3][1] = g_state.model_matrix[7];
        // Column 2 (stored at indices 8-11)
        model_mat.Elements[0][2] = g_state.model_matrix[8];
        model_mat.Elements[1][2] = g_state.model_matrix[9];
        model_mat.Elements[2][2] = g_state.model_matrix[10];
        model_mat.Elements[3][2] = g_state.model_matrix[11];
        // Column 3 (stored at indices 12-15) - translation part
        model_mat.Elements[0][3] = g_state.model_matrix[12];
        model_mat.Elements[1][3] = g_state.model_matrix[13];
        model_mat.Elements[2][3] = g_state.model_matrix[14];
        model_mat.Elements[3][3] = g_state.model_matrix[15];
        
        // Transpose the entire matrix to fix rotation direction
        // This is necessary because ImGuizmo's matrix format doesn't match HMM's convention
        model_mat = HMM_TransposeM4(model_mat);
    }
    HMM_Mat4 mvp = proj * view * model_mat;
    
    // Calculate light space MVP for shadow mapping
    // For directional light, we need to create a view matrix looking along the light direction
    // Light direction points from light source to scene (so we negate it for view direction)
    HMM_Vec3 light_dir = g_state.light_direction;
    
    // Ensure light direction is normalized
    float light_len = HMM_LenV3(light_dir);
    if (light_len > 0.001f) {
        light_dir = HMM_DivV3F(light_dir, light_len);
    } else {
        // Fallback to default direction if invalid
        light_dir = HMM_NormV3(HMM_Vec3{0.0f, -1.0f, 0.0f});
    }
    
    // Calculate up vector perpendicular to light direction
    // Use world up (0,1,0) as reference, but adjust if light is too vertical
    HMM_Vec3 world_up = HMM_Vec3{0.0f, 1.0f, 0.0f};
    HMM_Vec3 right = HMM_Cross(light_dir, world_up);
    float right_len = HMM_LenV3(right);
    
    // If light direction is too close to world up, use a different reference
    if (right_len < 0.001f) {
        world_up = HMM_Vec3{0.0f, 0.0f, 1.0f}; // Use forward as reference instead
        right = HMM_Cross(light_dir, world_up);
        right_len = HMM_LenV3(right);
    }
    
    // Normalize right vector
    if (right_len > 0.001f) {
        right = HMM_DivV3F(right, right_len);
    } else {
        right = HMM_Vec3{1.0f, 0.0f, 0.0f}; // Fallback
    }
    
    // Calculate actual up vector (perpendicular to light direction)
    // Following official demo pattern: use cross product to ensure orthogonality
    HMM_Vec3 light_up = HMM_Cross(right, light_dir);
    float up_len = HMM_LenV3(light_up);
    if (up_len > 0.001f) {
        light_up = HMM_DivV3F(light_up, up_len);
    } else {
        // If light is nearly vertical, use a fixed up vector
        // Check if light is pointing up or down
        if (abs(light_dir.Y) > 0.9f) {
            // Light is nearly vertical, use forward as up reference
            light_up = HMM_Vec3{0.0f, 0.0f, 1.0f};
            // Recalculate right to ensure orthogonality
            right = HMM_Cross(light_dir, light_up);
            right = HMM_DivV3F(right, HMM_LenV3(right));
            light_up = HMM_Cross(right, light_dir);
        } else {
            light_up = HMM_Vec3{0.0f, 1.0f, 0.0f}; // Fallback to world up
        }
    }

    // Position light far away in the direction opposite to light_dir
    // For directional light, position doesn't matter much, but we place it far away
    // The light direction points from light source to scene, so we negate it for position
    // Use a reasonable distance that covers the scene (in meters)
    HMM_Vec3 light_pos = HMM_MulV3F(light_dir, -50.0f); // 50 meters away
    HMM_Vec3 light_target = HMM_Vec3{0.0f, 0.0f, 0.0f}; // Look at origin (scene center)
    
    // Use orthographic projection for directional light
    // Make sure the frustum is large enough to cover the scene (in meters)
    // Adjust these values based on your scene size
    // For a character model (~1.6m tall) on a 50m x 50m ground, we need a larger frustum
    float light_size = 5.0f; // Size of light frustum: 5m x 5m (covers 5 meters in each direction from center)
    float light_near = 0.1f;  // 0.1 meter
    float light_far = 100.0f;  // 100 meters (should cover the scene)
    HMM_Mat4 light_proj = HMM_Orthographic_RH_ZO(-light_size, light_size, -light_size, light_size, light_near, light_far);
    HMM_Mat4 light_view = HMM_LookAt_RH(light_pos, light_target, light_up);
    HMM_Mat4 light_mvp = light_proj * light_view * model_mat;
    
    // Render shadow pass first (before main rendering)
    // Use persistent shadow pass (like official demo)
    if (g_state.shadows_enabled && g_state.shadow_map.id != 0) {
        sg_push_debug_group("Shaodw pass");
        sg_pass _shadow_pass = {0};
        _shadow_pass.action = g_state.shadow_pass_action;
        _shadow_pass.attachments.depth_stencil = g_state.shadow_map_ds_view;
        // Workaround for OpenGL: Add dummy color attachment to ensure glDrawBuffers is called correctly
        // This fixes the issue where depth clear doesn't work in OpenGL when there are no color attachments
        _shadow_pass.attachments.colors[0] = g_state.shadow_dummy_color_view;
        _shadow_pass.action.colors[0].load_action = SG_LOADACTION_DONTCARE; // Don't care about color clear
        _shadow_pass.label = "_shadow_pass";
        sg_begin_pass(&_shadow_pass);
        sg_apply_pipeline(g_state.shadow_pip);
        
        // Render model to shadow map
        if (g_state.model_loaded && g_state.vertex_buffer.id != 0 && g_state.index_buffer.id != 0) {
            shadow_vs_params_t shadow_vs_params;
            shadow_vs_params.light_mvp = light_mvp;
            
            sg_bindings shadow_bind = {};
            shadow_bind.vertex_buffers[0] = g_state.vertex_buffer;
            shadow_bind.index_buffer = g_state.index_buffer;
            sg_apply_bindings(&shadow_bind);
            sg_apply_uniforms(0, SG_RANGE(shadow_vs_params));
            
            // Render all parts
            size_t part_num = g_state.model->GetPartNum();
            for (size_t part_idx = 0; part_idx < part_num; ++part_idx) {
                const mmd::Model::Part& part = g_state.model->GetPart(part_idx);
                const size_t base_shift = part.GetBaseShift();
                const size_t triangle_num = part.GetTriangleNum();
                
                if (triangle_num == 0) continue;
                
                int index_offset = (int)(base_shift * 3);
                int index_count = (int)(triangle_num * 3);
                if (index_count > 0) {
                    sg_draw(index_offset, index_count, 1);
                }
            }
        }
        
        sg_end_pass();
        sg_pop_debug_group();
    }

    // Begin main rendering pass
    sg_push_debug_group("main pass");
    sg_pass _sg_pass{};
    _sg_pass.action = g_state.main_pass_action;
    _sg_pass.swapchain = sglue_swapchain();
    _sg_pass.label = "main pass";

    sg_begin_pass(&_sg_pass);

    // Draw skybox first (before model, but with depth test)
    if (g_state.ibl_initialized && g_state.show_skybox && g_state.environment_cubemap.id != 0 && g_state.skybox_vertex_buffer.id != 0) {
        // Remove translation from view matrix for skybox
        HMM_Mat4 view_no_translation = view;
        view_no_translation.Elements[3][0] = 0.0f;
        view_no_translation.Elements[3][1] = 0.0f;
        view_no_translation.Elements[3][2] = 0.0f;
        
        HMM_Mat4 skybox_mvp = proj * view_no_translation;
        
        ibl_vs_params_t skybox_params;
        skybox_params.mvp = skybox_mvp;
        
        sg_apply_pipeline(g_state.skybox_pip);
        
        // Use persistent view for environment cubemap
        sg_bindings skybox_bind = {};
        skybox_bind.vertex_buffers[0] = g_state.skybox_vertex_buffer;
        // Note: Slot names will be generated by sokol-shdc, adjust after compilation if needed
        // Typical names: VIEW_ibl_environment_map for texture, SMP_ibl_environment_smp for sampler
        skybox_bind.views[VIEW_ibl_environment_map] = g_state.environment_cubemap_view;
        skybox_bind.samplers[SMP_ibl_environment_smp] = g_state.default_sampler;
        sg_apply_bindings(&skybox_bind);
        sg_apply_uniforms(UB_ibl_vs_params, SG_RANGE(skybox_params));

        sg_draw(0, 36, 1);
    }
    
    // Model mode: draw loaded model (render by parts/materials)
    // Simplified: only albedo + rim light, no IBL or directional light
    if (g_state.model_loaded && g_state.vertex_buffer.id != 0 && g_state.index_buffer.id != 0) {
        sg_apply_pipeline(g_state.pip);
        
        // Update VS params (no light_mvp needed anymore)
        mmd_vs_params_t vs_params;
        vs_params.mvp = mvp;
        vs_params.model = model_mat;
        
        // FS params: only view_pos and rim light parameters
        mmd_fs_params_t fs_params;
        fs_params.view_pos = g_state.camera_pos;
        fs_params.rim_power = g_state.rim_power;
        fs_params.rim_intensity = g_state.rim_intensity;
        fs_params.rim_color = g_state.rim_color;
        
        // Render each part with its own texture
        size_t part_num = g_state.model->GetPartNum();
        for (size_t part_idx = 0; part_idx < part_num; ++part_idx) {
            const mmd::Model::Part& part = g_state.model->GetPart(part_idx);
            const size_t base_shift = part.GetBaseShift();
            const size_t triangle_num = part.GetTriangleNum();
            
            if (triangle_num == 0) continue;
            
            sg_bindings bind = {};
            bind.vertex_buffers[0] = g_state.vertex_buffer;
            bind.index_buffer = g_state.index_buffer;
            
            // Use persistent view for material texture (slot 0 for diffuse texture)
            sg_view material_view = (part_idx < g_state.material_texture_views.size() && g_state.material_texture_views[part_idx].id != 0)
                ? g_state.material_texture_views[part_idx]
                : g_state.default_texture_view;
            
            // Bind only diffuse texture (slot 0)
            bind.views[0] = material_view;
            bind.samplers[0] = g_state.default_sampler;
            
            sg_apply_bindings(&bind);
            sg_apply_uniforms(0, SG_RANGE(vs_params));
            sg_apply_uniforms(1, SG_RANGE(fs_params)); // fs_params is now binding 1
            
            // Draw this part's triangles
            int index_offset = (int)(base_shift * 3);
            int index_count = (int)(triangle_num * 3);
            if (index_count > 0) {
                sg_draw(index_offset, index_count, 1);
            }
        }
    }
    
    // Draw ground plane (white stage) - uses dedicated shader with shadows
    if (g_state.ground_vertex_buffer.id != 0 && g_state.ground_index_buffer.id != 0) {
        sg_apply_pipeline(g_state.ground_pip);
        
        HMM_Mat4 ground_model = HMM_M4D(1.0f); // Identity matrix for ground
        HMM_Mat4 ground_mvp = proj * view * ground_model;
        // HMM uses right-to-left matrix multiplication (OpenGL style)
        // So: light_view_proj * ground_model = light_proj * light_view * ground_model
        HMM_Mat4 ground_light_mvp = light_proj * light_view * ground_model;
        
        // Ground uses dedicated shader with different uniform structure
        // Structure names are generated by sokol-shdc: ground_ground_vs_params_t and ground_ground_fs_params_t
        ground_vs_params_t ground_vs_params;
        ground_vs_params.mvp = ground_mvp;
        ground_vs_params.model = ground_model;
        ground_vs_params.light_mvp = ground_light_mvp;
        
        ground_fs_params_t ground_fs_params;
        ground_fs_params.shadows_enabled = g_state.shadows_enabled ? 1.0f : 0.0f;
        ground_fs_params.receive_shadows = 1.0f; // Ground receives shadows
        
        sg_bindings ground_bind = {};
        ground_bind.vertex_buffers[0] = g_state.ground_vertex_buffer;
        ground_bind.index_buffer = g_state.ground_index_buffer;
        
        ground_bind.views[2] = g_state.default_texture_view;
        ground_bind.samplers[2] = g_state.default_sampler;
        if (g_state.shadow_map_view.id != 0) {
            ground_bind.views[3] = g_state.shadow_map_view;
            ground_bind.samplers[3] = g_state.shadow_sampler;
        }
        
        sg_apply_bindings(&ground_bind);
        sg_apply_uniforms(0, SG_RANGE(ground_vs_params));
        sg_apply_uniforms(1, SG_RANGE(ground_fs_params)); // fs_params is binding 1
        
        sg_draw(0, 6, 1); // Ground has 6 indices (2 triangles)
    }
    
    // End model rendering pass
    sg_end_pass();
    sg_pop_debug_group();
    
    // Begin UI pass for ImGui (separate pass, don't clear color buffer)
    sg_push_debug_group("ui_pass");
    sg_pass ui_pass = {};
    ui_pass.action = g_state.ui_pass_action;
    ui_pass.swapchain = sglue_swapchain();
    ui_pass.label = "ui_pass";
    sg_begin_pass(&ui_pass);
    
    // Draw ImGuizmo gizmo if enabled
    // Must be called in ImGui context, after all ImGui windows but before simgui_render()
    // Following the official demo pattern: https://github.com/CedricGuillemet/ImGuizmo/blob/master/example/main.cpp
    if (g_state.guizmo_enabled && g_state.model_loaded) {
        // Convert view and projection matrices to float arrays for ImGuizmo
        float view_array[16];
        float proj_array[16];
        
        // After transposition, copy by columns: array[col*4 + row] = transposed.Elements[row][col]
        // This is equivalent to copying the original matrix by rows
        for (int col = 0; col < 4; col++) {
            for (int row = 0; row < 4; row++) {
                view_array[col * 4 + row] = view.Elements[col][row];
                proj_array[col * 4 + row] = proj.Elements[col][row];
            }
        }

        // Set orthographic mode (must be called before using gizmo, like in official demo)
        ImGuizmo::SetOrthographic(false);  // We're using perspective projection
        
        // Note: BeginFrame() is already called at the start of frame() function (line 560)
        // Following official demo pattern: TransformStart -> EditTransform -> TransformEnd
        TransformStart(view_array, proj_array, g_state.model_matrix);
        EditTransform(view_array, proj_array, g_state.model_matrix);
        TransformEnd();
    }
    
    // Draw Sequencer window if enabled
    if (g_state.sequencer_enabled && g_state.motion_loaded && g_state.sequencer) {
        if (ImGui::Begin("Animation Sequencer", &g_state.sequencer_enabled)) {
            // Playback controls
            bool play_button_clicked = ImGui::Button(g_state.animation_playing ? "Pause" : "Play");
            if (play_button_clicked) {
                g_state.animation_playing = !g_state.animation_playing;
                // Always release manual control when toggling play state
                g_state.sequencer_manual_control = false;
                if (g_state.animation_playing) {
                    // Resume playback - sync sequencer frame with current time
                    g_state.sequencer_current_frame = static_cast<int>(g_state.time * 30.0f);
                    g_state.sequencer_last_frame = g_state.sequencer_current_frame;
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Stop")) {
                g_state.animation_playing = false;
                g_state.time = 0.0f;
                g_state.sequencer_current_frame = 0;
                g_state.sequencer_manual_control = false;
                g_state.sequencer_last_frame = 0;
            }
            ImGui::SameLine();
            ImGui::Text("Frame: %d / %d", g_state.sequencer_current_frame, g_state.sequencer ? g_state.sequencer->GetFrameMax() : 0);
            ImGui::SameLine();
            ImGui::Text("Time: %.2fs", g_state.time);
            
            ImGui::Separator();
            
            // Draw sequencer timeline
            // Store frame before sequencer call to detect manual changes
            int previous_frame = g_state.sequencer_current_frame;
            bool was_playing = g_state.animation_playing;
            
            // Call sequencer - this may modify sequencer_current_frame if user drags
            ImSequencer::Sequencer(
                g_state.sequencer.get(),
                &g_state.sequencer_current_frame,
                &g_state.sequencer_expanded,
                &g_state.sequencer_selected_entry,
                &g_state.sequencer_first_frame,
                ImSequencer::SEQUENCER_CHANGE_FRAME
            );
            
            // Check if user manually dragged the frame marker in sequencer
            // ImSequencer modifies currentFrame when user drags in the top timeline area
            if (previous_frame != g_state.sequencer_current_frame) {
                // Frame changed - check if it's a manual change (different from what we set)
                int expected_frame = static_cast<int>(g_state.time * 30.0f);
                if (abs(g_state.sequencer_current_frame - expected_frame) > 1) {
                    // Frame is significantly different from expected - this is a manual drag
                    g_state.time = g_state.sequencer_current_frame / 30.0f;
                    g_state.sequencer_last_frame = g_state.sequencer_current_frame;
                    
                    // If was playing, pause it
                    if (was_playing) {
                        g_state.animation_playing = false;
                        g_state.sequencer_manual_control = true;
                    }
                } else {
                    // Small difference, might be rounding - update last_frame to match
                    g_state.sequencer_last_frame = g_state.sequencer_current_frame;
                }
            }
            
            ImGui::End();
        }
    }

    // Render ImGui
    simgui_render();
    
    // End UI pass
    sg_end_pass();
    sg_pop_debug_group();
    sg_commit();
}

// Cleanup function
void cleanup(void) {
    if (g_state.vertex_buffer.id != 0) {
        sg_destroy_buffer(g_state.vertex_buffer);
    }
    if (g_state.index_buffer.id != 0) {
        sg_destroy_buffer(g_state.index_buffer);
    }
    if (g_state.skybox_vertex_buffer.id != 0) {
        sg_destroy_buffer(g_state.skybox_vertex_buffer);
    }
    if (g_state.equirectangular_map.id != 0) {
        sg_destroy_image(g_state.equirectangular_map);
    }
    if (g_state.environment_cubemap.id != 0) {
        sg_destroy_image(g_state.environment_cubemap);
    }

    // Clean up IBL views
    if (g_state.environment_cubemap_view.id != 0) {
        sg_destroy_view(g_state.environment_cubemap_view);
    }

    if (g_state.default_sampler.id != 0) {
        sg_destroy_sampler(g_state.default_sampler);
    }
    // Clean up material textures and views
    for (size_t i = 0; i < g_state.material_textures.size(); ++i) {
        if (g_state.material_textures[i].id != 0 && g_state.material_textures[i].id != g_state.default_texture.id) {
            sg_destroy_image(g_state.material_textures[i]);
        }
        if (i < g_state.material_texture_views.size() && g_state.material_texture_views[i].id != 0) {
            sg_destroy_view(g_state.material_texture_views[i]);
        }
    }
    if (g_state.default_texture_view.id != 0) {
        sg_destroy_view(g_state.default_texture_view);
    }
    if (g_state.default_texture.id != 0) {
        sg_destroy_image(g_state.default_texture);
    }
    // Clean up shadow mapping resources
    if (g_state.shadow_map_view.id != 0) {
        sg_destroy_view(g_state.shadow_map_view);
    }
    if (g_state.shadow_map_ds_view.id != 0) {
        sg_destroy_view(g_state.shadow_map_ds_view);
    }
    if (g_state.shadow_map.id != 0) {
        sg_destroy_image(g_state.shadow_map);
    }
    if (g_state.shadow_sampler.id != 0) {
        sg_destroy_sampler(g_state.shadow_sampler);
    }
    // Clean up dummy color attachment (OpenGL workaround)
    if (g_state.shadow_dummy_color_view.id != 0) {
        sg_destroy_view(g_state.shadow_dummy_color_view);
    }
    if (g_state.shadow_dummy_color.id != 0) {
        sg_destroy_image(g_state.shadow_dummy_color);
    }
    // Clean up ground resources
    if (g_state.ground_vertex_buffer.id != 0) {
        sg_destroy_buffer(g_state.ground_vertex_buffer);
    }
    if (g_state.ground_index_buffer.id != 0) {
        sg_destroy_buffer(g_state.ground_index_buffer);
    }
    sgimgui_discard(&g_state.sgimgui);
    simgui_shutdown();
    sg_shutdown();
}

// Input event handler
void input(const sapp_event* ev) {
    // Handle ImGui events first
    bool imgui_wants_input = simgui_handle_event(ev);
    
    // Only handle our own events if ImGui doesn't want them
    if (!imgui_wants_input) {
        // Mouse events for camera rotation and panning
        if (ev->type == SAPP_EVENTTYPE_MOUSE_DOWN) {
            if (ev->mouse_button == SAPP_MOUSEBUTTON_LEFT) {
                g_state.camera_rotating = true;
                g_state.last_mouse_x = ev->mouse_x;
                g_state.last_mouse_y = ev->mouse_y;
            } else if (ev->mouse_button == SAPP_MOUSEBUTTON_MIDDLE) {
                g_state.camera_panning = true;
                g_state.last_mouse_x = ev->mouse_x;
                g_state.last_mouse_y = ev->mouse_y;
            }
        } else if (ev->type == SAPP_EVENTTYPE_MOUSE_UP) {
            if (ev->mouse_button == SAPP_MOUSEBUTTON_LEFT) {
                g_state.camera_rotating = false;
            } else if (ev->mouse_button == SAPP_MOUSEBUTTON_MIDDLE) {
                g_state.camera_panning = false;
            }
        } else if (ev->type == SAPP_EVENTTYPE_MOUSE_MOVE) {
            float dx = ev->mouse_x - g_state.last_mouse_x;
            float dy = ev->mouse_y - g_state.last_mouse_y;
            
            if (g_state.camera_rotating) {
                // Rotate camera around target
                const float rotation_speed = 0.005f;
                g_state.camera_rotation_x += dx * rotation_speed;
                g_state.camera_rotation_y -= dy * rotation_speed;
                
                // Clamp vertical rotation to prevent flipping
                const float max_angle = 1.57f;  // ~90 degrees
                if (g_state.camera_rotation_y > max_angle) {
                    g_state.camera_rotation_y = max_angle;
                }
                if (g_state.camera_rotation_y < -max_angle) {
                    g_state.camera_rotation_y = -max_angle;
                }
            } else if (g_state.camera_panning) {
                // Pan camera (move target based on camera's right and up vectors)
                // Calculate camera orientation
                float cos_y = cosf(g_state.camera_rotation_y);
                float sin_y = sinf(g_state.camera_rotation_y);
                float cos_x = cosf(g_state.camera_rotation_x);
                float sin_x = sinf(g_state.camera_rotation_x);
                
                // Right vector (camera's right direction)
                HMM_Vec3 right;
                right.X = cos_x;
                right.Y = 0.0f;
                right.Z = -sin_x;
                
                // Up vector (camera's up direction, considering pitch)
                HMM_Vec3 up;
                up.X = -sin_y * sin_x;
                up.Y = cos_y;
                up.Z = -sin_y * cos_x;
                
                // Pan speed based on camera distance (closer = slower pan)
                const float pan_speed = 0.01f;
                float pan_factor = pan_speed * g_state.camera_distance;
                
                // Calculate pan movement
                HMM_Vec3 pan_move = HMM_Vec3{0.0f, 0.0f, 0.0f};
                pan_move = HMM_Add(pan_move, HMM_MulV3F(right, -dx * pan_factor));
                pan_move = HMM_Add(pan_move, HMM_MulV3F(up, dy * pan_factor));
                
                // Apply pan to camera target
                g_state.camera_target = HMM_Add(g_state.camera_target, pan_move);
            }
            
            g_state.last_mouse_x = ev->mouse_x;
            g_state.last_mouse_y = ev->mouse_y;
        } else if (ev->type == SAPP_EVENTTYPE_MOUSE_SCROLL) {
            // Zoom with mouse wheel
            const float zoom_speed = 0.2f; // meters per scroll (in meters)
            g_state.camera_distance -= ev->scroll_y * zoom_speed;
            if (g_state.camera_distance < 0.5f) {
                g_state.camera_distance = 0.5f; // Minimum 0.5 meters
            }
            if (g_state.camera_distance > 20.0f) {
                g_state.camera_distance = 20.0f; // Maximum 20 meters
            }
        }
        
        // Keyboard events
        if (ev->type == SAPP_EVENTTYPE_KEY_DOWN) {
            if (ev->key_code < 256) {
                g_state.keys_down[ev->key_code] = true;
            }
            if (ev->key_code == SAPP_KEYCODE_ESCAPE) {
                sapp_request_quit();
            } else if (ev->key_code == SAPP_KEYCODE_R) {
                // Reset camera
                g_state.camera_pos = HMM_Vec3{0.0f, 1.6f, 4.0f}; // ~1.6m height, 4m away
                g_state.camera_target = HMM_Vec3{0.0f, 0.0f, 0.0f};
                g_state.camera_fov = 45.0f;
                g_state.camera_distance = 4.0f; // 4 meters
                g_state.camera_rotation_x = 0.0f;
                g_state.camera_rotation_y = 0.0f;
            }
        } else if (ev->type == SAPP_EVENTTYPE_KEY_UP) {
            if (ev->key_code < 256) {
                g_state.keys_down[ev->key_code] = false;
            }
        }
    }
}

// Main function
sapp_desc sokol_main(int argc, char* argv[]) {
    // Process command line arguments
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        std::string lower_arg = arg;
        std::transform(lower_arg.begin(), lower_arg.end(), lower_arg.begin(), ::tolower);
        
        if (lower_arg.find(".pmx") != std::string::npos) {
            g_state.model_filename = arg;
        } else if (lower_arg.find(".vmd") != std::string::npos) {
            g_state.motion_filename = arg;
        }
    }

    sapp_desc _sapp_desc{};
    _sapp_desc.init_cb = init;
    _sapp_desc.frame_cb = frame;
    _sapp_desc.cleanup_cb = cleanup;
    _sapp_desc.event_cb = input;
    _sapp_desc.width = 1280;
    _sapp_desc.height = 720;
    _sapp_desc.window_title = "Simple MMD Renderer";
    _sapp_desc.logger.func = slog_func;
    return _sapp_desc;
}
