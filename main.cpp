#define SOKOL_IMPL
#define SOKOL_TRACE_HOOKS
#ifdef _WIN32
// #define SOKOL_D3D11
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
#include "shader/ibl.glsl.h"
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
    sg_pass_action pass_action;
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
    HMM_Vec3 camera_pos = {0.0f, 10.0f, 40.0f};
    HMM_Vec3 camera_target = {0.0f, 0.0f, 0.0f};
    float camera_fov = 45.0f;
    float camera_distance = 40.0f;
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
    
    // IBL (Image Based Lighting) resources
    sg_image equirectangular_map = {0};
    sg_image environment_cubemap = {0};
    sg_image irradiance_map = {0};
    sg_image prefilter_map = {0};
    sg_sampler default_sampler = {0};
    sg_pipeline equirect_to_cubemap_pip = {0};
    sg_pipeline irradiance_pip = {0};
    sg_pipeline prefilter_pip = {0};
    sg_pipeline skybox_pip = {0};
    sg_buffer skybox_vertex_buffer = {0};
    sg_buffer skybox_index_buffer = {0};
    bool ibl_initialized = false;
    bool show_skybox = true;
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
    
    for (size_t i = 0; i < vertex_num; ++i) {
        mmd::Model::Vertex<mmd::ref> vertex = g_state.model->GetVertex(i);
        mmd::Vector3f pos = vertex.GetCoordinate();
        mmd::Vector3f normal = vertex.GetNormal();
        mmd::Vector2f uv = vertex.GetUVCoordinate();
        
        Vertex v;
        v.pos[0] = pos.p.x;
        v.pos[1] = pos.p.y;
        v.pos[2] = pos.p.z;
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
    
    for (size_t i = 0; i < vertex_num; ++i) {
        mmd::Model::Vertex<mmd::ref> vertex = g_state.model->GetVertex(i);
        mmd::Vector2f uv = vertex.GetUVCoordinate();
        
        // Use deformed coordinates and normals from pose_image
        const mmd::Vector3f& pos = g_state.poser->pose_image.coordinates[i];
        const mmd::Vector3f& normal = g_state.poser->pose_image.normals[i];
        
        Vertex v;
        v.pos[0] = pos.p.x;
        v.pos[1] = pos.p.y;
        v.pos[2] = pos.p.z;
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
    float* hdr_data = stbi_loadf(hdr_path.c_str(), &width, &height, &nrComponents, 0);
    if (!hdr_data) {
        std::cerr << "Failed to load HDR image: " << hdr_path << std::endl;
        return false;
    }
    
    std::cout << "Loaded HDR image: " << width << "x" << height << std::endl;
    
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
                        // dir = HMM_Vec3{1.0f, v, -u};
                        dir = HMM_Vec3{-1.0f, v, -u};
                        break;
                    case 1: // -X (left) - negative X axis
                        // dir = HMM_Vec3{-1.0f, v, u};
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
    
    // For now, use environment cubemap as irradiance and prefilter maps
    // (Full prefilter implementation would require additional passes)
    g_state.irradiance_map = g_state.environment_cubemap;
    g_state.prefilter_map = g_state.environment_cubemap;
    
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
    if (g_state.guizmo_draw_grid) {
        ImGuizmo::DrawGrid(cameraView, cameraProjection, identityMatrix, 100.f);
    }
    
    // Draw cube at model position
    int gizmoCount = 1;
    ImGuizmo::DrawCubes(cameraView, cameraProjection, matrix, gizmoCount);

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
    g_state.pass_action.colors[0] = { .load_action = SG_LOADACTION_CLEAR, .clear_value = {0.1f,0.1f, 0.15f, 1.0f} };
    g_state.pass_action.depth = { .load_action = SG_LOADACTION_CLEAR, .clear_value = 1.0f };

    // Set UI Pass
    g_state.ui_pass_action.colors[0] = { .load_action = SG_LOADACTION_LOAD };

    // Initialize time
    stm_setup();
    
    // Create skybox geometry
    CreateSkyboxGeometry();
    
    // Create default sampler for IBL
    sg_sampler_desc sampler_desc = {};
    sampler_desc.min_filter = SG_FILTER_LINEAR;
    sampler_desc.mag_filter = SG_FILTER_LINEAR;
    sampler_desc.wrap_u = SG_WRAP_CLAMP_TO_EDGE;
    sampler_desc.wrap_v = SG_WRAP_CLAMP_TO_EDGE;
    sampler_desc.wrap_w = SG_WRAP_CLAMP_TO_EDGE;
    sampler_desc.label = "default-ibl-sampler";
    g_state.default_sampler = sg_make_sampler(&sampler_desc);
    
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
            ImGui::DragFloat("Distance", &g_state.camera_distance, 0.5f, 1.0f, 200.0f);
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
                g_state.camera_pos = HMM_Vec3{0.0f, 10.0f, 40.0f};
                g_state.camera_target = HMM_Vec3{0.0f, 0.0f, 0.0f};
                g_state.camera_fov = 45.0f;
                g_state.camera_distance = 40.0f;
                g_state.camera_rotation_x = 0.0f;
                g_state.camera_rotation_y = 0.0f;
            }
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
        const float move_speed = 50.0f;  // units per second
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
    HMM_Mat4 proj = HMM_Perspective_RH_NO(g_state.camera_fov * HMM_DegToRad, (float)width / (float)height, 0.1f, 1000.0f);
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

    // Prepare uniform data
    mmd_vs_params_t params;
    params.mvp = mvp;
    
    // Begin rendering
    sg_pass _sg_pass{};
    _sg_pass.action = g_state.pass_action;
    _sg_pass.swapchain = sglue_swapchain();

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
        
        // Create view for cubemap
        sg_view_desc _cubemap_sg_view_desc = {};
        _cubemap_sg_view_desc.texture.image = g_state.environment_cubemap;
        sg_view cubemap_view = sg_make_view(&_cubemap_sg_view_desc);
        
        sg_bindings skybox_bind = {};
        skybox_bind.vertex_buffers[0] = g_state.skybox_vertex_buffer;
        // Note: Slot names will be generated by sokol-shdc, adjust after compilation if needed
        // Typical names: VIEW_ibl_environment_map for texture, SMP_ibl_environment_smp for sampler
        skybox_bind.views[VIEW_ibl_environment_map] = cubemap_view;
        skybox_bind.samplers[SMP_ibl_environment_smp] = g_state.default_sampler;
        sg_apply_bindings(&skybox_bind);
        sg_apply_uniforms(UB_ibl_vs_params, SG_RANGE(skybox_params));
        
        sg_draw(0, 36, 1);

        sg_destroy_view(cubemap_view);
    }
    
    // Model mode: draw loaded model
    if (g_state.model_loaded && g_state.vertex_buffer.id != 0 && g_state.index_buffer.id != 0) {
        sg_apply_pipeline(g_state.pip);
        
        sg_bindings bind = {};
        bind.vertex_buffers[0] = g_state.vertex_buffer;
        bind.index_buffer = g_state.index_buffer;
        sg_apply_bindings(&bind);
        
        sg_apply_uniforms(UB_mmd_vs_params, SG_RANGE(params));
        
        int num_indices = (int)(g_state.model->GetTriangleNum() * 3);
        if (num_indices > 0) {
            sg_draw(0, num_indices, 1);
        }
    }
    
    // End model rendering pass
    sg_end_pass();
    
    // Begin UI pass for ImGui (separate pass, don't clear color buffer)
    sg_pass ui_pass = {};
    ui_pass.action = g_state.ui_pass_action;
    ui_pass.swapchain = sglue_swapchain();
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
    if (g_state.irradiance_map.id != 0 && g_state.irradiance_map.id != g_state.environment_cubemap.id) {
        sg_destroy_image(g_state.irradiance_map);
    }
    if (g_state.prefilter_map.id != 0 && g_state.prefilter_map.id != g_state.environment_cubemap.id) {
        sg_destroy_image(g_state.prefilter_map);
    }
    if (g_state.default_sampler.id != 0) {
        sg_destroy_sampler(g_state.default_sampler);
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
            const float zoom_speed = 2.0f;
            g_state.camera_distance -= ev->scroll_y * zoom_speed;
            if (g_state.camera_distance < 1.0f) {
                g_state.camera_distance = 1.0f;
            }
            if (g_state.camera_distance > 200.0f) {
                g_state.camera_distance = 200.0f;
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
                g_state.camera_pos = HMM_Vec3{0.0f, 10.0f, 40.0f};
                g_state.camera_target = HMM_Vec3{0.0f, 0.0f, 0.0f};
                g_state.camera_fov = 45.0f;
                g_state.camera_distance = 40.0f;
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
