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
#include "util/sokol_imgui.h"
#include "util/sokol_gfx_imgui.h"
#include "ImGuizmo.h"
#include "ImSequencer.h"

#include "mmd/mmd.hxx"
#include "HandmadeMath.h"
#include "shader/main.glsl.h"
#include "nfd.h"

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
        if (ImGui::BeginMenu("Tools")) {
            ImGui::MenuItem("Model Transform (ImGuizmo)", nullptr, &g_state.guizmo_enabled);
            ImGui::MenuItem("Animation Sequencer", nullptr, &g_state.sequencer_enabled);
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
        // ImGuizmo uses column-major format, HMM uses row-major internally but stores as column-major
        model_mat.Elements[0][0] = g_state.model_matrix[0];
        model_mat.Elements[0][1] = g_state.model_matrix[1];
        model_mat.Elements[0][2] = g_state.model_matrix[2];
        model_mat.Elements[0][3] = g_state.model_matrix[3];
        model_mat.Elements[1][0] = g_state.model_matrix[4];
        model_mat.Elements[1][1] = g_state.model_matrix[5];
        model_mat.Elements[1][2] = g_state.model_matrix[6];
        model_mat.Elements[1][3] = g_state.model_matrix[7];
        model_mat.Elements[2][0] = g_state.model_matrix[8];
        model_mat.Elements[2][1] = g_state.model_matrix[9];
        model_mat.Elements[2][2] = g_state.model_matrix[10];
        model_mat.Elements[2][3] = g_state.model_matrix[11];
        model_mat.Elements[3][0] = g_state.model_matrix[12];
        model_mat.Elements[3][1] = g_state.model_matrix[13];
        model_mat.Elements[3][2] = g_state.model_matrix[14];
        model_mat.Elements[3][3] = g_state.model_matrix[15];
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
    
    // Draw ImGuizmo gizmo if enabled (must be after simgui_render to use ImGui draw list)
    if (g_state.guizmo_enabled && g_state.model_loaded) {
        ImGuizmo::SetDrawlist();
        ImGuizmo::SetRect(0, 0, (float)width, (float)height);
        
        // Convert view and projection matrices to float arrays for ImGuizmo
        float view_array[16];
        float proj_array[16];
        
        // View matrix (column-major)
        HMM_Mat4 view = HMM_LookAt_RH(g_state.camera_pos, g_state.camera_target, HMM_Vec3{0.0f, 1.0f, 0.0f});
        view_array[0] = view.Elements[0][0]; view_array[4] = view.Elements[0][1]; view_array[8] = view.Elements[0][2]; view_array[12] = view.Elements[0][3];
        view_array[1] = view.Elements[1][0]; view_array[5] = view.Elements[1][1]; view_array[9] = view.Elements[1][2]; view_array[13] = view.Elements[1][3];
        view_array[2] = view.Elements[2][0]; view_array[6] = view.Elements[2][1]; view_array[10] = view.Elements[2][2]; view_array[14] = view.Elements[2][3];
        view_array[3] = view.Elements[3][0]; view_array[7] = view.Elements[3][1]; view_array[11] = view.Elements[3][2]; view_array[15] = view.Elements[3][3];
        
        // Projection matrix (column-major)
        HMM_Mat4 proj = HMM_Perspective_RH_NO(g_state.camera_fov * HMM_DegToRad, (float)width / (float)height, 0.1f, 1000.0f);
        proj_array[0] = proj.Elements[0][0]; proj_array[4] = proj.Elements[0][1]; proj_array[8] = proj.Elements[0][2]; proj_array[12] = proj.Elements[0][3];
        proj_array[1] = proj.Elements[1][0]; proj_array[5] = proj.Elements[1][1]; proj_array[9] = proj.Elements[1][2]; proj_array[13] = proj.Elements[1][3];
        proj_array[2] = proj.Elements[2][0]; proj_array[6] = proj.Elements[2][1]; proj_array[10] = proj.Elements[2][2]; proj_array[14] = proj.Elements[2][3];
        proj_array[3] = proj.Elements[3][0]; proj_array[7] = proj.Elements[3][1]; proj_array[11] = proj.Elements[3][2]; proj_array[15] = proj.Elements[3][3];
        
        // Manipulate model matrix
        ImGuizmo::Manipulate(view_array, proj_array, g_state.guizmo_operation, g_state.guizmo_mode, g_state.model_matrix);
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
