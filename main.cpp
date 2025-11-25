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
#include "shader/test_triangle.glsl.h"

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
    sg_pass_action pass_action;
    
    std::shared_ptr<mmd::Model> model;
    std::shared_ptr<mmd::Motion> motion;
    std::unique_ptr<mmd::Poser> poser;
    std::unique_ptr<mmd::MotionPlayer> motion_player;
    
    sg_buffer vertex_buffer = {0};
    sg_buffer index_buffer = {0};
    
    // Test triangle buffers
    sg_buffer test_vertex_buffer = {0};
    sg_buffer test_index_buffer = {0};
    sg_pipeline test_pipeline = {0};
    
    float time = 0.0f;
    bool model_loaded = false;
    bool motion_loaded = false;
    bool test_mode = false;  // Start with test mode
    
    // Camera parameters
    HMM_Vec3 camera_pos = {0.0f, 0.0f, 5.0f};  // Closer to model
    HMM_Vec3 camera_target = {0.0f, 0.0f, 0.0f};  // Look at origin
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
    // vbuf_desc.data = SG_RANGE(vertices);
    vbuf_desc.data.ptr = vertices.data();
    vbuf_desc.data.size = vertices.size() * sizeof(Vertex);
    vbuf_desc.label = "model-vertices";
    g_state.vertex_buffer = sg_make_buffer(&vbuf_desc);
    
    // Create index buffer
    if (indices.empty()) {
        std::cerr << "Error: Index data is empty!" << std::endl;
        return;
    }
    sg_buffer_desc ibuf_desc = {};
    ibuf_desc.usage.index_buffer = true;
    // ibuf_desc.data = SG_RANGE(indices);
    ibuf_desc.data.ptr = indices.data();
    ibuf_desc.data.size = indices.size() * sizeof(uint32_t);
    ibuf_desc.label = "model-indices";
    g_state.index_buffer = sg_make_buffer(&ibuf_desc);
    
    if (g_state.index_buffer.id == SG_INVALID_ID) {
        std::cerr << "Error: Failed to create index buffer!" << std::endl;
        return;
    }
    
    std::cout << "  Index buffer created with " << indices.size() << " indices" << std::endl;
    

    
    std::cout << "Updated model buffers: " << vertices.size() << " vertices, " << indices.size() << " indices" << std::endl;
    std::cout << "  Vertex buffer id: " << g_state.vertex_buffer.id << std::endl;
    std::cout << "  Index buffer id: " << g_state.index_buffer.id << std::endl;
}

// Create test triangle (following sokol triangle-sapp example)
void CreateTestTriangle() {
    // Simple triangle vertices with colors (following triangle-sapp example format)
    // positions (vec4) + colors (vec4)
    float vertices[] = {
        // positions            // colors
        -0.5f, -0.5f, 0.5f, 1.0f,   1.0f, 0.0f, 0.0f, 1.0f,  // Red
         0.5f, -0.5f, 0.5f, 1.0f,   0.0f, 1.0f, 0.0f, 1.0f,  // Green
         0.5f,  0.5f, 0.5f, 1.0f,   0.0f, 0.0f, 1.0f, 1.0f,  // Blue
        -0.5f,  0.5f, 0.5f, 1.0f,   1.0f, 1.0f, 1.0f, 1.0f,  // White
    };

    const uint32_t indices[] = { 0, 1, 2, 0, 2, 3, };
    
    // Create vertex buffer
    sg_buffer_desc vbuf_desc = {};
    vbuf_desc.data = SG_RANGE(vertices);
    vbuf_desc.label = "test-triangle-vertices";
    g_state.test_vertex_buffer = sg_make_buffer(&vbuf_desc);

    // Create index buffer
    sg_buffer_desc ibuf_desc = {};
    ibuf_desc.usage.index_buffer = true;
    ibuf_desc.data = SG_RANGE(indices);
    ibuf_desc.label = "test-triangle-indices";
    g_state.test_index_buffer = sg_make_buffer(&ibuf_desc);
    
    // Create shader from code-generated sg_shader_desc
    sg_shader test_shd = sg_make_shader(triangle_shader_desc(sg_query_backend()));
    
    if (test_shd.id == SG_INVALID_ID) {
        std::cerr << "Error: Failed to create test shader" << std::endl;
    } else {
        std::cout << "Test shader created successfully, id: " << test_shd.id << std::endl;
    }
    
    // Create pipeline (following triangle-sapp example, using C++ compatible syntax)
    sg_pipeline_desc pip_desc = {};
    pip_desc.shader = test_shd;
    pip_desc.layout.attrs[ATTR_triangle_position].format = SG_VERTEXFORMAT_FLOAT4;
    pip_desc.layout.attrs[ATTR_triangle_color0].format = SG_VERTEXFORMAT_FLOAT4;
    pip_desc.index_type = SG_INDEXTYPE_UINT32;
    pip_desc.label = "test-triangle-pipeline";
    g_state.test_pipeline = sg_make_pipeline(&pip_desc);
    
    if (g_state.test_pipeline.id == SG_INVALID_ID) {
        std::cerr << "Error: Failed to create test pipeline" << std::endl;
    } else {
        std::cout << "Test pipeline created successfully, id: " << g_state.test_pipeline.id << std::endl;
    }
    
    std::cout << "Test triangle created:" << std::endl;
    std::cout << "  Test vertex buffer id: " << g_state.test_vertex_buffer.id << std::endl;
}

// Initialization function
void init(void) {
    // Setup sokol-gfx (following triangle-sapp example, using C++ compatible syntax)
    sg_desc _sg_desc = {};
    _sg_desc.environment = sglue_environment();
    _sg_desc.logger.func = slog_func;
    sg_setup(&_sg_desc);
    
    // Create test triangle first
    CreateTestTriangle();
    
    // Create shader (using generated shader descriptor)
    sg_shader shd = sg_make_shader(mmd_mmd_shader_desc(sg_query_backend()));
    
    // Verify shader creation
    if (shd.id == SG_INVALID_ID) {
        std::cerr << "Error: Failed to create shader" << std::endl;
    } else {
        std::cout << "Shader created successfully, id: " << shd.id << std::endl;
    }
    
    // Create render pipeline
    sg_pipeline_desc _sg_pipeline_desc{};
    _sg_pipeline_desc.shader = shd;
    
    // Set vertex layout with stride
    _sg_pipeline_desc.layout.buffers[0].stride = sizeof(Vertex);
    _sg_pipeline_desc.layout.attrs[ATTR_mmd_mmd_position].offset = 0;
    _sg_pipeline_desc.layout.attrs[ATTR_mmd_mmd_position].format = SG_VERTEXFORMAT_FLOAT3;
    _sg_pipeline_desc.layout.attrs[ATTR_mmd_mmd_normal].offset = sizeof(float) * 3;
    _sg_pipeline_desc.layout.attrs[ATTR_mmd_mmd_normal].format = SG_VERTEXFORMAT_FLOAT3;
    _sg_pipeline_desc.layout.attrs[ATTR_mmd_mmd_texcoord0].offset = sizeof(float) * 6;
    _sg_pipeline_desc.layout.attrs[ATTR_mmd_mmd_texcoord0].format = SG_VERTEXFORMAT_FLOAT2;
    
    _sg_pipeline_desc.depth.write_enabled = true;
    _sg_pipeline_desc.depth.compare = SG_COMPAREFUNC_LESS_EQUAL;
    _sg_pipeline_desc.cull_mode = SG_CULLMODE_NONE;  // Disable culling for debugging
    _sg_pipeline_desc.index_type = SG_INDEXTYPE_UINT32;
    _sg_pipeline_desc.primitive_type = SG_PRIMITIVETYPE_TRIANGLES;
    _sg_pipeline_desc.label = "model-pipeline";

    g_state.pip = sg_make_pipeline(&_sg_pipeline_desc);
    
    // Verify pipeline creation
    if (g_state.pip.id == SG_INVALID_ID) {
        std::cerr << "Error: Failed to create pipeline" << std::endl;
    } else {
        std::cout << "Pipeline created successfully, id: " << g_state.pip.id << std::endl;
    }
    
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
    if (g_state.model_loaded && g_state.motion_loaded && g_state.motion_player && g_state.poser) {
        // Calculate current frame (assuming 30 FPS)
        size_t frame = static_cast<size_t>(g_state.time * 30.0f);
        
        // Seek to current frame and apply motion
        g_state.motion_player->SeekFrame(frame);
        g_state.poser->ResetPosing();
        g_state.poser->Deform();
    }
    
    int width = sapp_width();
    int height = sapp_height();
    
    // Calculate MVP matrix
    HMM_Mat4 proj = HMM_Perspective_RH_NO(g_state.camera_fov, (float)width / (float)height, 0.1f, 1000.0f);
    HMM_Mat4 view = HMM_LookAt_RH(g_state.camera_pos, g_state.camera_target, HMM_Vec3{0.0f, 1.0f, 0.0f});
    HMM_Mat4 model_mat = HMM_M4D(1.0f);
    HMM_Mat4 mvp = HMM_Mul(proj, HMM_Mul(view, model_mat));
    
    // Transpose matrix for shader (shaders typically expect column-major)
    HMM_Mat4 mvp_transposed = HMM_TransposeM4(mvp);
    
    // Prepare uniform data
    mmd_vs_params_t params;
    params.mvp = mvp_transposed;
    
    // Begin rendering
    sg_pass _sg_pass{};
    _sg_pass.action = g_state.pass_action;
    _sg_pass.swapchain = sglue_swapchain();

    sg_begin_pass(&_sg_pass);
    
    // Set viewport and scissor
    sg_apply_viewport(0, 0, width, height, true);
    sg_apply_scissor_rect(0, 0, width, height, true);
    
    // Test mode: draw simple triangle (following triangle-sapp example)
    if (g_state.test_mode && g_state.test_pipeline.id != 0) {
        // Apply test pipeline
        sg_apply_pipeline(g_state.test_pipeline);
        
        // Apply bindings (no index buffer needed, just vertex buffer)
        sg_bindings test_bind = {};
        test_bind.vertex_buffers[0] = g_state.test_vertex_buffer;
        test_bind.index_buffer = g_state.test_index_buffer;
        sg_apply_bindings(&test_bind);
        
        // Draw test triangle (following triangle-sapp example: sg_draw(0, 3, 1))
        static int test_draw_count = 0;
        if (test_draw_count < 3) {
            std::cout << "Drawing test triangle, frame " << test_draw_count << std::endl;
            std::cout << "  Test vertex buffer: " << g_state.test_vertex_buffer.id << std::endl;
            std::cout << "  Test index buffer: " << g_state.test_index_buffer.id << std::endl;
            std::cout << "  Test pipeline: " << g_state.test_pipeline.id << std::endl;
            test_draw_count++;
        }
        sg_draw(0, 6, 1);
    }
    // Model mode: draw loaded model
    if (g_state.model_loaded && g_state.vertex_buffer.id != 0 && g_state.index_buffer.id != 0) {
        static int draw_count = 0;
        bool debug_frame = (draw_count < 5);
        if (debug_frame) {
            std::cout << "=== Drawing model frame " << draw_count << " ===" << std::endl;
            std::cout << "  Pipeline id: " << g_state.pip.id << std::endl;
            std::cout << "  Vertex buffer id: " << g_state.vertex_buffer.id << std::endl;
            std::cout << "  Index buffer id: " << g_state.index_buffer.id << std::endl;
        }
        draw_count++;
        
        // Apply pipeline first
        sg_apply_pipeline(g_state.pip);
        // sg_apply_pipeline(g_state.test_pipeline);
        
        // Ensure bindings are up to date
        // Update bindings
        sg_bindings _bind = {};
        _bind.vertex_buffers[0] = g_state.vertex_buffer;
        _bind.index_buffer = g_state.index_buffer;

        // Verify bindings before applying (same as test square)
        if (debug_frame) {
            std::cout << "  Binding check:" << std::endl;
            std::cout << "    Vertex buffer in bindings: " << _bind.vertex_buffers[0].id << std::endl;
            std::cout << "    Index buffer in bindings: " << _bind.index_buffer.id << std::endl;
            std::cout << "    Index buffer offset: " << _bind.index_buffer_offset << std::endl;
        }
        
        // Apply bindings
        sg_apply_bindings(&_bind);
        
        // Apply uniforms (after pipeline and bindings)
        // Check uniform data size matches shader expectation (16 bytes * 4 = 64 bytes for mat4)
        if (debug_frame) {
            std::cout << "  Uniform data size: " << sizeof(mmd_vs_params_t) << " bytes" << std::endl;
            std::cout << "  Expected size for mat4: 64 bytes" << std::endl;
            // Verify uniform block slot
            std::cout << "  Uniform block slot: " << UB_mmd_vs_params << std::endl;
        }
        // Try applying uniforms - if this fails validation, drawcall will be skipped
        sg_apply_uniforms(UB_mmd_vs_params, SG_RANGE(params));
        
        // Draw with index buffer
        int num_indices = (int)(g_state.model->GetTriangleNum() * 3);
        if (num_indices > 0) {
            if (debug_frame) {
                std::cout << "  Drawing " << num_indices << " indices" << std::endl;
                std::cout << "  Pipeline state: " << (g_state.pip.id != 0 ? "valid" : "invalid") << std::endl;
                std::cout << "  Vertex buffer state: " << (g_state.vertex_buffer.id != 0 ? "valid" : "invalid") << std::endl;
                std::cout << "  Index buffer state: " << (g_state.index_buffer.id != 0 ? "valid" : "invalid") << std::endl;
            }
            // base_element: starting index in index buffer, num_elements: number of indices to draw
            sg_draw(0, num_indices, 1);
        } else {
            static bool printed = false;
            if (!printed) {
                std::cout << "Warning: num_indices is 0, not drawing" << std::endl;
                printed = true;
            }
        }
    } else {
        // Debug: print why we're not drawing
        if (!g_state.model_loaded) {
            static bool printed = false;
            if (!printed) {
                std::cout << "Not drawing: model not loaded" << std::endl;
                printed = true;
            }
        } else if (g_state.vertex_buffer.id == 0) {
            static bool printed = false;
            if (!printed) {
                std::cout << "Not drawing: vertex buffer not created" << std::endl;
                printed = true;
            }
        } else if (g_state.index_buffer.id == 0) {
            static bool printed = false;
            if (!printed) {
                std::cout << "Not drawing: index buffer not created" << std::endl;
                printed = true;
            }
        }
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
