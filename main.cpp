#define SOKOL_IMPL
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

#include "mmd/mmd.hxx"
#include "HandmadeMath.h"
#include "shader/main.glsl.h"

#include <iostream>
#include <vector>
#include <memory>
#include <string>
#include <codecvt>
#include <locale>
#include <fstream>
#include <algorithm>

// Vertex structure
struct Vertex {
    float pos[3];
    float normal[3];
    float uv[2];
};

// Application state
struct {
    sg_pipeline pip;
    sg_bindings bind;
    sg_pass_action pass_action;
    
    std::shared_ptr<mmd::Model> model;
    std::shared_ptr<mmd::Motion> motion;
    mmd::Poser poser;
    
    sg_buffer vertex_buffer = {0};
    sg_buffer index_buffer = {0};
    
    float time = 0.0f;
    bool model_loaded = false;
    bool motion_loaded = false;
    
    // Camera parameters
    HMM_Vec3 camera_pos = {0.0f, 10.0f, 30.0f};
    HMM_Vec3 camera_target = {0.0f, 10.0f, 0.0f};
    float camera_fov = 45.0f;
    
    std::string model_filename;
    std::string motion_filename;
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

// Update model vertex buffers
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
        indices.push_back(triangle.v[0]);
        indices.push_back(triangle.v[1]);
        indices.push_back(triangle.v[2]);
    }
    
    // Create vertex buffer
    sg_buffer_desc vbuf_desc = {};
    vbuf_desc.data = SG_RANGE(vertices);
    vbuf_desc.label = "model-vertices";
    g_state.vertex_buffer = sg_make_buffer(&vbuf_desc);
    
    // Create index buffer
    sg_buffer_desc ibuf_desc = {};
    ibuf_desc.usage.index_buffer = true;
    ibuf_desc.data = SG_RANGE(indices);
    ibuf_desc.label = "model-indices";
    g_state.index_buffer = sg_make_buffer(&ibuf_desc);
    
    // Update bindings
    g_state.bind.vertex_buffers[0] = g_state.vertex_buffer;
    g_state.bind.index_buffer = g_state.index_buffer;
}

// Initialization function
void init(void) {
    sg_desc _sg_desc{};
    _sg_desc.environment = sglue_environment();
    _sg_desc.logger.func = slog_func;

    sg_setup(&_sg_desc);
    
    // Create shader (using generated shader descriptor)
    sg_shader shd = sg_make_shader(mmd_mmd_shader_desc(sg_query_backend()));
    
    // Create render pipeline
    sg_pipeline_desc _sg_pipeline_desc{};
    _sg_pipeline_desc.shader = shd;
    _sg_pipeline_desc.layout.attrs[ATTR_mmd_mmd_position] = { .offset = 0, .format = SG_VERTEXFORMAT_FLOAT3 };
    _sg_pipeline_desc.layout.attrs[ATTR_mmd_mmd_normal] = { .offset = sizeof(float) * 3, .format = SG_VERTEXFORMAT_FLOAT3 };
    _sg_pipeline_desc.layout.attrs[ATTR_mmd_mmd_texcoord0] = { .offset = sizeof(float) * 6, .format = SG_VERTEXFORMAT_FLOAT2 };
    _sg_pipeline_desc.depth.write_enabled = true;
    _sg_pipeline_desc.depth.compare = SG_COMPAREFUNC_LESS_EQUAL;
    _sg_pipeline_desc.cull_mode = SG_CULLMODE_BACK;
    _sg_pipeline_desc.label = "model-pipeline";

    g_state.pip = sg_make_pipeline(&_sg_pipeline_desc);
    
    // Set clear color
    g_state.pass_action.colors[0] = { .load_action = SG_LOADACTION_CLEAR, .clear_value = {0.1f,0.1f, 0.15f, 1.0f} };

    // Initialize time
    stm_setup();
    
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
    g_state.time += dt;
    
    // Update animation
    if (g_state.model_loaded && g_state.motion_loaded && g_state.model && g_state.motion) {
        // Calculate current frame (assuming 30 FPS)
        float frame_time = g_state.time * 30.0f;
        
        // Apply motion to model
        g_state.poser.SetModel(g_state.model.get());
        g_state.poser.SetMotion(g_state.motion.get());
        g_state.poser.SetFrame(frame_time);
        g_state.poser.Update();
    }
    
    int width = sapp_width();
    int height = sapp_height();
    
    // Calculate MVP matrix
    HMM_Mat4 proj = HMM_Perspective_RH_NO(g_state.camera_fov, (float)width / (float)height, 0.1f, 1000.0f);
    HMM_Mat4 view = HMM_LookAt_RH(g_state.camera_pos, g_state.camera_target, HMM_Vec3{0.0f, 1.0f, 0.0f});
    HMM_Mat4 model_mat = HMM_M4D(1.0f);
    HMM_Mat4 mvp = HMM_Mul(proj, HMM_Mul(view, model_mat));
    
    // Update uniform buffer
    mmd_vs_params_t params;
    params.mvp = mvp;
    sg_apply_uniforms(UB_mmd_vs_params, SG_RANGE(params));
    
    // Begin rendering
    sg_pass _sg_pass{};
    _sg_pass.action = g_state.pass_action;
    _sg_pass.swapchain = sglue_swapchain();

    sg_begin_pass(&_sg_pass);
    
    if (g_state.model_loaded && g_state.vertex_buffer.id != 0) {
        sg_apply_pipeline(g_state.pip);
        sg_apply_bindings(&g_state.bind);
        
        sg_draw(0, (int)(g_state.model->GetTriangleNum() * 3), 1);
    }
    
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
    sg_shutdown();
}

// Input event handler
void input(const sapp_event* ev) {
    if (ev->type == SAPP_EVENTTYPE_KEY_DOWN) {
        if (ev->key_code == SAPP_KEYCODE_ESCAPE) {
            sapp_request_quit();
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
