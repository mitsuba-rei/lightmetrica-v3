/*
    Lightmetrica - Copyright (c) 2019 Hisanari Otsu
    Distributed under MIT license. See LICENSE file for details.
*/

#include <lm/lm.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <chrono>
#include <thread>
#include <mutex>
#include <atomic>
#include <condition_variable>
using namespace lm::literals;

// ----------------------------------------------------------------------------

#define THROW_RUNTIME_ERROR() \
    throw std::runtime_error("Consult log outputs for detailed error messages")

static void checkGLError(const char* filename, const int line) {
    if (int err = glGetError(); err != GL_NO_ERROR) {
        LM_ERROR("OpenGL Error: {} {} {}", err, filename, line);
        THROW_RUNTIME_ERROR();
    }
}

#define CHECK_GL_ERROR() checkGLError(__FILE__, __LINE__)

// ----------------------------------------------------------------------------

class GLScene;

// OpenGL material
class GLMaterial {
private:
    friend class GLScene;

private:
    glm::vec3 color_{};
    bool wireframe_ = false;
    bool shade_ = true;
    std::optional<GLuint> texture_;
    int w_;
    int h_;
    float lineWidth_ = 1.f;
    float lineWidthScale_ = 1.f;

public:
    GLMaterial(glm::vec3 color, float lineWidth, bool wireframe, bool shade)
        : color_(color)
        , lineWidth_(lineWidth)
        , wireframe_(wireframe)
        , shade_(shade)
    {}

    GLMaterial(lm::Material* material, bool wireframe, bool shade) : wireframe_(wireframe), shade_(shade) {
        if (material->key() != "material::wavefrontobj") {
            color_ = glm::vec3(0);
            return;
        }
        
        // For material::wavefrontobj, we try to use underlying texture
        auto* diffuse = dynamic_cast<lm::Material*>(material->underlying("diffuse"));
        if (!diffuse) {
            color_ = glm::vec3(0);
            return;
        }
        auto* tex = dynamic_cast<lm::Texture*>(diffuse->underlying("mapKd"));
        if (!tex) {
            color_ = *diffuse->reflectance({}, {});
            return;
        }

        // Create OpenGL texture
        const auto [w, h, c, data] = tex->buffer();
        w_ = w;
        h_ = h;

#if 0
        // Convert the texture to float type
        std::vector<float> data_(w*h * 3);
        for (int i = 0; i < w*h*3; i++) {
            data_[i] = float(data[i]);
        }
#endif

        texture_ = 0;
        glGenTextures(1, &*texture_);
        glBindTexture(GL_TEXTURE_2D, *texture_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        if (c == 3) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, w, h, 0, GL_RGB, GL_FLOAT, data);
        }
        else if (c == 4) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_FLOAT, data);
        }
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    ~GLMaterial() {
        if (texture_) {
            glDeleteTextures(1, &*texture_);
        }
    }

public:
    // Enable material parameters
    void apply(GLuint name, const std::function<void()>& process) const {
        // Wireframe mode
        if (wireframe_) {
            glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
        }
        else {
            glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        }

        // Point size
        glPointSize(20);

        // Line width
        glLineWidth(lineWidth_ * lineWidthScale_);

        glProgramUniform3fv(name, glGetUniformLocation(name, "Color"), 1, &color_.x);
        glProgramUniform1i(name, glGetUniformLocation(name, "Shade"), shade_ ? 1 : 0);
        if (texture_) {
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, *texture_);
            glProgramUniform1i(name, glGetUniformLocation(name, "UseTexture"), 1);
        }
        else {
            glProgramUniform1i(name, glGetUniformLocation(name, "UseTexture"), 0);
        }
        process();

        if (texture_) {
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, 0);
        }
    }
};

// ----------------------------------------------------------------------------

// OpenGL mesh
namespace MeshType {
    enum {
        Triangles = 1<<0,
        LineStrip = 1<<1,
        Lines     = 1<<2,
        Points    = 1<<3,
    };
}
class GLMesh {
private:
    friend class GLScene;

private:
    int type_;
    GLuint count_;
    GLuint bufferP_;
    GLuint bufferN_;
    GLuint bufferT_;
    GLuint bufferI_;
    GLuint vertexArray_;

public:

    GLMesh(int type, const std::vector<lm::Vec3>& vs) : type_(type) {
        std::vector<GLuint> is(vs.size());
        std::iota(is.begin(), is.end(), 0);
        createGLBuffers(vs, {}, {}, is);
    }

    GLMesh(lm::Mesh* mesh) : type_(MeshType::Triangles) {
        // Create OpenGL buffer objects
        std::vector<lm::Vec3> vs;
        std::vector<lm::Vec3> ns;
        std::vector<lm::Vec2> ts;
        std::vector<GLuint> is;
        GLuint count = 0;
        mesh->foreachTriangle([&](int, const lm::Mesh::Tri& tri) {
            vs.insert(vs.end(), { tri.p1.p, tri.p2.p, tri.p3.p });
            ns.insert(ns.end(), { tri.p1.n, tri.p2.n, tri.p3.n });
            ts.insert(ts.end(), { tri.p1.t, tri.p2.t, tri.p3.t });
            is.insert(is.end(), { count, count+1, count+2 });
            count+=3;
        });
        createGLBuffers(vs, ns, ts, is);
    }

    ~GLMesh() {
        glDeleteVertexArrays(1, &vertexArray_);
        glDeleteBuffers(1, &bufferP_);
        glDeleteBuffers(1, &bufferN_);
        glDeleteBuffers(1, &bufferT_);
        glDeleteBuffers(1, &bufferI_);
    }

private:
    void createGLBuffers(
        const std::vector<lm::Vec3>& vs,
        const std::vector<lm::Vec3>& ns,
        const std::vector<lm::Vec2>& ts,
        const std::vector<GLuint>& is)
    {
        glGenBuffers(1, &bufferP_);
        glBindBuffer(GL_ARRAY_BUFFER, bufferP_);
        glBufferData(GL_ARRAY_BUFFER, vs.size() * sizeof(lm::Vec3), vs.data(), GL_STATIC_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        CHECK_GL_ERROR();

        if (!ns.empty()) {
            glGenBuffers(1, &bufferN_);
            glBindBuffer(GL_ARRAY_BUFFER, bufferN_);
            glBufferData(GL_ARRAY_BUFFER, ns.size() * sizeof(lm::Vec3), ns.data(), GL_STATIC_DRAW);
            glBindBuffer(GL_ARRAY_BUFFER, 0);
            CHECK_GL_ERROR();
        }

        if (!ts.empty()) {
            glGenBuffers(1, &bufferT_);
            glBindBuffer(GL_ARRAY_BUFFER, bufferT_);
            glBufferData(GL_ARRAY_BUFFER, ts.size() * sizeof(lm::Vec2), ts.data(), GL_STATIC_DRAW);
            glBindBuffer(GL_ARRAY_BUFFER, 0);
            CHECK_GL_ERROR();
        }

        count_ = int(is.size());
        glGenBuffers(1, &bufferI_);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, bufferI_);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, is.size() * sizeof(GLuint), is.data(), GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
        CHECK_GL_ERROR();

        glGenVertexArrays(1, &vertexArray_);
        glBindVertexArray(vertexArray_);
        glBindBuffer(GL_ARRAY_BUFFER, bufferP_);
        glVertexAttribPointer(0, 3, LM_DOUBLE_PRECISION ? GL_DOUBLE : GL_FLOAT, GL_FALSE, 0, nullptr);
        glEnableVertexAttribArray(0);
        if (!ns.empty()) {
            glBindBuffer(GL_ARRAY_BUFFER, bufferN_);
            glVertexAttribPointer(1, 3, LM_DOUBLE_PRECISION ? GL_DOUBLE : GL_FLOAT, GL_FALSE, 0, nullptr);
            glEnableVertexAttribArray(1);
        }
        if (!ts.empty()) {
            glBindBuffer(GL_ARRAY_BUFFER, bufferT_);
            glVertexAttribPointer(2, 2, LM_DOUBLE_PRECISION ? GL_DOUBLE : GL_FLOAT, GL_FALSE, 0, nullptr);
            glEnableVertexAttribArray(2);
        }
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glBindVertexArray(0);
        CHECK_GL_ERROR();
    }

public:
    // Dispatch rendering
    void render() const {
        glBindVertexArray(vertexArray_);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, bufferI_);
        if ((type_ & MeshType::Triangles) > 0) {
            glDrawElements(GL_TRIANGLES, count_, GL_UNSIGNED_INT, nullptr);
        }
        if ((type_ & MeshType::LineStrip) > 0) {
            glDrawElements(GL_LINE_STRIP, count_, GL_UNSIGNED_INT, nullptr);
        }
        if ((type_ & MeshType::Lines) > 0) {
            glDrawElements(GL_LINES, count_, GL_UNSIGNED_INT, nullptr);
        }
        if ((type_ & MeshType::Points) > 0) {
            glDrawElements(GL_POINTS, count_, GL_UNSIGNED_INT, nullptr);
        }
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
        glBindVertexArray(0);
    }
};

// ----------------------------------------------------------------------------

// OpenGL scene
struct GLPrimitive {
    std::string name;       // This can be empty
    lm::Mat4 transform;     // Transformation
    int mesh;               // Mesh index
    int material;           // Material index
};
class GLScene {
public:
    std::vector<std::unique_ptr<GLMesh>> meshes_;
    std::vector<std::unique_ptr<GLMaterial>> materials_;
    std::unordered_map<std::string, int> materialMap_;
    std::vector<GLPrimitive> primitives_;
    std::unordered_map<std::string, int> namedPrimitiveMap_;

public:
    // Reset the internal state
    void reset() {
        meshes_.clear();
        materials_.clear();
        materialMap_.clear();
        primitives_.clear();
        namedPrimitiveMap_.clear();
    }

    // Add mesh and material pair
    void add(const lm::Mat4& transform, lm::Mesh* mesh, lm::Material* material) {
        LM_INFO("Creating GL primitive [#{}]", primitives_.size());

        // Mesh
        int meshidx = [&]() {
            int idx = int(meshes_.size());
            meshes_.emplace_back(new GLMesh(mesh));
            return idx;
        }();

        // Material
        int materialidx = [&]() {
            if (auto it = materialMap_.find(material->name()); it != materialMap_.end()) {
                return it->second;
            }
            int idx = int(materials_.size());
            materialMap_[material->name()] = idx;
            materials_.emplace_back(new GLMaterial(material, true, true));
            return idx;
        }();
        
        // Primitive
        primitives_.push_back({ "", transform, meshidx, materialidx });
    }

    void add(int type, lm::Vec3 color, float lineWidth, const std::vector<lm::Vec3>& vs) {
        LM_INFO("Creating GL primitive [#{}]", primitives_.size());
        int meshidx = int(meshes_.size());
        int materialidx = int(materials_.size());
        meshes_.emplace_back(new GLMesh(type, vs));
        materials_.emplace_back(new GLMaterial(color, lineWidth, true, false));
        primitives_.push_back({ "", lm::Mat4(1_f), meshidx, materialidx });
    }

    int addByName(const std::string& name, int type, lm::Vec3 color, float lineWidth, const std::vector<lm::Vec3>& vs) {
        auto* mesh = new GLMesh(type, vs);
        auto* material = new GLMaterial(color, lineWidth, true, false);
        const int index = int(primitives_.size());
        if (namedPrimitiveMap_.find(name) != namedPrimitiveMap_.end()) {
            const auto& p = primitives_[namedPrimitiveMap_[name]];
            meshes_[p.mesh].reset(mesh);
            materials_[p.material].reset(material);
        }
        else {
            int meshidx = int(meshes_.size());
            int materialidx = int(materials_.size());
            meshes_.emplace_back(mesh);
            materials_.emplace_back(material);
            namedPrimitiveMap_[name] = index;
            primitives_.push_back({ name, lm::Mat4(1_f), meshidx, materialidx });
        }
        return index;
    }

    GLPrimitive& primitiveAt(int i) {
        return primitives_.at(i);
    }

    GLMaterial& materialAt(int i) {
        return *materials_.at(i).get();
    }

    GLPrimitive& primitiveByName(const std::string& name) {
        return primitives_.at(namedPrimitiveMap_.at(name));
    }

    // Iterate primitives
    using ProcessPrimitiveFunc = std::function<void(const GLPrimitive& primitive)>;
    void foreachPrimitive(const ProcessPrimitiveFunc& processPrimitive) const {
        for (const auto& primitive : primitives_) {
            processPrimitive(primitive);
        }
    }

    void updateGUI() {
        ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x - 400, 0), ImGuiCond_Once);
        ImGui::SetNextWindowSize(ImVec2(400, 600), ImGuiCond_Once);
        ImGui::Begin("OpenGL scene");

        // Selected primitive index
        static int selectedNodeIndex = -1;

        // List of primitives
        if (ImGui::CollapsingHeader("Primitives", ImGuiTreeNodeFlags_DefaultOpen)) {
            for (int i = 0; i < (int)(primitives_.size()); i++) {
                // Current primitive
                const auto& primitive = primitives_[i];

                // Primitive name
                const std::string name = primitive.name.empty() ? "<empty>" : primitive.name;

                // Flag for tree node
                auto treeNodeFlag = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick;
                if (selectedNodeIndex == i) {
                    treeNodeFlag |= ImGuiTreeNodeFlags_Selected;
                }

                // Primitive information
                const bool open = ImGui::TreeNodeEx((void*)(intptr_t)i, treeNodeFlag, fmt::format("Primitive [name={}]", name).c_str());

                // Change appearance according to hovering/clicking
                if (ImGui::IsItemClicked()) {
                    // Deselect if the node is already selected
                    selectedNodeIndex = selectedNodeIndex == i ? -1 : i;
                }
                if (selectedNodeIndex == i || ImGui::IsItemHovered()) {
                    auto* material = materials_[primitive.material].get();
                    material->wireframe_ = false;
                }
                else {
                    auto* material = materials_[primitive.material].get();
                    material->wireframe_ = true;
                }

                if (open) {
                    // Mesh information
                    if (ImGui::TreeNode(fmt::format("Mesh [id={}]", primitive.mesh).c_str())) {
                        // Current mesh
                        const auto* mesh = meshes_[primitive.mesh].get();

                        // Mesh type
                        if ((mesh->type_ & MeshType::Triangles) > 0) {
                            ImGui::Text("Triangles");
                        }
                        if ((mesh->type_ & MeshType::LineStrip) > 0) {
                            ImGui::Text("LineStrip");
                        }
                        if ((mesh->type_ & MeshType::Lines) > 0) {
                            ImGui::Text("Lines");
                        }
                        if ((mesh->type_ & MeshType::Points) > 0) {
                            ImGui::Text("Points");
                        }

                        ImGui::TreePop();
                    }

                    // Material information
                    if (ImGui::TreeNode(fmt::format("Material [id={}]", primitive.material).c_str())) {
                        // Current material
                        auto* material = materials_[primitive.material].get();

                        // Wireframe and shaded flag
                        ImGui::Checkbox("Enable wireframe", &material->wireframe_);
                        ImGui::Checkbox("Enable shade", &material->shade_);

                        // Texture or color
                        if (material->texture_) {
                            ImGui::Text("Texture");
                            #pragma warning(push)
                            #pragma warning(disable:4312)
                            const auto aspect = float(material->h_) / material->w_;
                            ImGui::Image(
                                reinterpret_cast<ImTextureID*>(*material->texture_),
                                ImVec2(200.f, 200.f*aspect),
                                ImVec2(0, 1),
                                ImVec2(1, 0),
                                ImColor(255, 255, 255, 255),
                                ImColor(255, 255, 255, 128));
                                #pragma warning(pop)
                        }
                        else {
                            ImGui::ColorEdit3("Color", &material->color_.x);
                        }

                        ImGui::TreePop();
                    }

                    ImGui::TreePop();
                }
            }
        }

        ImGui::End();
    }
};

// ----------------------------------------------------------------------------

class GLDisplayCamera {
private:
    lm::Float aspect_;
    lm::Float fov_;
    lm::Vec3 eye_;
    lm::Vec3 up_;
    lm::Vec3 forward_;
    lm::Float pitch_;
    lm::Float yaw_;

public:
    void reset(lm::Vec3 eye, lm::Vec3 center, lm::Vec3 up, lm::Float fov) {
        eye_ = eye;
        up_ = up;
        forward_ = glm::normalize(center - eye);
        fov_ = fov;
        pitch_ = glm::degrees(std::asin(forward_.y));
        yaw_ = glm::degrees(std::atan2(forward_.z, forward_.x));
    }

    lm::Vec3 eye() { return eye_; }
    lm::Vec3 center() { return eye_ + forward_; }
    lm::Float fov() { return fov_; }

    lm::Mat4 viewMatrix() const {
        return glm::lookAt(eye_, eye_ + forward_, up_);
    }
    
    lm::Mat4 projectionMatrix() const {
        return glm::perspective(glm::radians(fov_), aspect_, 0.01_f, 10000_f);
    }

    // True if the camera is updated
    void update(GLFWwindow* window) {
        // Update aspect ratio
        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        aspect_ = lm::Float(display_w) / display_h;

        // Update forward vector
        {
            static auto prevMousePos = ImGui::GetMousePos();
            const auto mousePos = ImGui::GetMousePos();
            const bool rotating = ImGui::IsMouseDown(GLFW_MOUSE_BUTTON_RIGHT);
            if (rotating) {
                int w, h;
                glfwGetFramebufferSize(window, &w, &h);
                const float sensitivity = 0.1f;
                const float dx = float(prevMousePos.x - mousePos.x) * sensitivity;
                const float dy = float(prevMousePos.y - mousePos.y) * sensitivity;
                yaw_ += dx;
                pitch_ = glm::clamp(pitch_ - dy, -89_f, 89_f);
            }
            prevMousePos = mousePos;
            forward_ = glm::vec3(
                cos(glm::radians(pitch_)) * cos(glm::radians(yaw_)),
                sin(glm::radians(pitch_)),
                cos(glm::radians(pitch_)) * sin(glm::radians(yaw_)));
        }

        // Update camera position
        {
            const auto w = -forward_;
            const auto u = glm::normalize(glm::cross(up_, w));
            const auto v = glm::cross(w, u);
            const auto factor = ImGui::GetIO().KeyShift ? 10.0_f : 1_f;
            const auto speed = ImGui::GetIO().DeltaTime * factor;
            if (ImGui::IsKeyDown('W')) { eye_ += forward_ * speed; }
            if (ImGui::IsKeyDown('S')) { eye_ -= forward_ * speed; }
            if (ImGui::IsKeyDown('A')) { eye_ -= u * speed; }
            if (ImGui::IsKeyDown('D')) { eye_ += u * speed; }
        }
    }
};

// ----------------------------------------------------------------------------

// Interactive visualizer using OpenGL
class GLRenderer {
private:
    GLuint pipeline_;
    GLuint progV_;
    GLuint progF_;

public:
    bool setup() {
        // Load shaders
        const std::string vscode = R"x(
            #version 330 core
            layout (location = 0) in vec3 position_;
            layout (location = 1) in vec3 normal_;
            layout (location = 2) in vec2 uv_;
            out gl_PerVertex {
                vec4 gl_Position;
            };
            out vec3 normal;
            out vec2 uv;
            uniform mat4 ModelMatrix;
            uniform mat4 ViewMatrix;
            uniform mat4 ProjectionMatrix;
            void main() {
                mat4 mvMatrix = ViewMatrix * ModelMatrix;
                mat4 mvpMatrix = ProjectionMatrix * mvMatrix;
                mat3 normalMatrix = mat3(transpose(inverse(mvMatrix)));
                normal = normalMatrix * normal_;
                uv = uv_;
                gl_Position = mvpMatrix * vec4(position_, 1);
            }
        )x";
        const std::string fscode = R"x(
            #version 330 core
            in vec3 normal;
            in vec2 uv;
            out vec4 fragColor;
            uniform sampler2D tex;
            uniform vec3 Color;
            uniform int UseTexture;
            uniform int Shade;
            void main() {
                fragColor.rgb = Color;
                if (UseTexture == 0) {
                    fragColor.rgb = Color;
                }
                else {
                    fragColor.rgb = texture(tex, uv).rgb;
                }
                if (Shade == 1) {
                    fragColor.rgb *= .2+.8*max(0, dot(normal, vec3(0,0,1)));
                }
                fragColor.a = 1;
            }
        )x";

        const auto createProgram = [](GLenum shaderType, const std::string& code) -> std::optional<GLuint> {
            GLuint program = glCreateProgram();
            GLuint shader = glCreateShader(shaderType);
            const auto* codeptr = code.c_str();
            glShaderSource(shader, 1, &codeptr, nullptr);
            glCompileShader(shader);
            GLint ret;
            glGetShaderiv(shader, GL_COMPILE_STATUS, &ret);
            if (!ret) {
                int length;
                glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
                std::vector<char> v(length);
                glGetShaderInfoLog(shader, length, nullptr, &v[0]);
                glDeleteShader(shader);
                LM_ERROR("{}", v.data());
                return {};
            }
            glAttachShader(program, shader);
            glProgramParameteri(program, GL_PROGRAM_SEPARABLE, GL_TRUE);
            glDeleteShader(shader);
            glLinkProgram(program);
            glGetProgramiv(program, GL_LINK_STATUS, &ret);
            if (!ret) {
                GLint length;
                glGetProgramiv(program, GL_INFO_LOG_LENGTH, &length);
                std::vector<char> v(length);
                glGetProgramInfoLog(program, length, nullptr, &v[0]);
                LM_ERROR("{}", v.data());
                return {};
            }
            return program;
        };

        if (auto p = createProgram(GL_VERTEX_SHADER, vscode); p) {
            progV_ = *p;
        }
        else {
            return false;
        }
        if (auto p = createProgram(GL_FRAGMENT_SHADER, fscode); p) {
            progF_ = *p;
        }
        else {
            return false;
        }
        glGenProgramPipelines(1, &pipeline_);
        glUseProgramStages(pipeline_, GL_VERTEX_SHADER_BIT, progV_);
        glUseProgramStages(pipeline_, GL_FRAGMENT_SHADER_BIT, progF_);

        CHECK_GL_ERROR();
        return true;
    }
    
    // This function is called one per frame
    void render(const GLScene& scene, const GLDisplayCamera& camera) const {
        // State
        glEnable(GL_DEPTH_TEST);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        
        // Camera
        const auto viewM = glm::mat4(camera.viewMatrix());
        glProgramUniformMatrix4fv(progV_, glGetUniformLocation(progV_, "ViewMatrix"), 1, GL_FALSE, glm::value_ptr(viewM));
        const auto projM = glm::mat4(camera.projectionMatrix());
        glProgramUniformMatrix4fv(progV_, glGetUniformLocation(progV_, "ProjectionMatrix"), 1, GL_FALSE, glm::value_ptr(projM));

        // Render meshes
        glBindProgramPipeline(pipeline_);
        scene.foreachPrimitive([&](const GLPrimitive& p) {
            glProgramUniformMatrix4fv(progV_, glGetUniformLocation(progV_, "ModelMatrix"), 1, GL_FALSE, glm::value_ptr(glm::mat4(p.transform)));
             scene.materials_[p.material]->apply(progF_, [&]() {
                scene.meshes_[p.mesh]->render();
            });
        });
        glBindProgramPipeline(0);

        // Restore
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        glDisable(GL_BLEND);

        CHECK_GL_ERROR();
    }
};

// ----------------------------------------------------------------------------

// Base class for interactive examples
class InteractiveApp {
public:
    GLFWwindow* window;
    GLScene glscene;
    GLRenderer glrenderer;
    GLDisplayCamera glcamera;

public:
    bool setup(const std::string& title, const lm::Json& opt) {
        // Init GLFW
        if (!glfwInit()) {
            return false;
        }

        // Error callback
        glfwSetErrorCallback([](int error, const char* desc) {
            LM_ERROR("[GLFW error #{}] {}", error, desc);
        });

        // Craete GLFW window
        window = [&]() -> GLFWwindow* {
            // GLFW window
            glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
            glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
            glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
            #ifdef LM_DEBUG_MODE
            glfwWindowHint(GLFW_OPENGL_DEBUG_CONTEXT, GLFW_TRUE);
            #endif
            GLFWwindow* window = glfwCreateWindow(opt["w"], opt["h"], title.c_str(), nullptr, nullptr);
            if (!window) {
                return nullptr;
            }
            glfwMakeContextCurrent(window);
            glfwSwapInterval(0);
            if (glewInit() != GLEW_OK) {
                glfwDestroyWindow(window);
                return nullptr;
            }
            // ImGui context
            IMGUI_CHECKVERSION();
            ImGui::CreateContext();
            ImGui_ImplGlfw_InitForOpenGL(window, true);
            ImGui_ImplOpenGL3_Init();
            ImGui::StyleColorsDark();
            return window;
        }();
        if (!window) {
            glfwTerminate();
            return false;
        }

        #if LM_DEBUG_MODE && 0
        // Debug output of OpenGL
        glEnable(GL_DEBUG_OUTPUT);
        glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
        glDebugMessageCallback([](GLenum source, GLenum type, GLuint, GLenum severity, GLsizei, const GLchar* message, void*) {
            const auto str = fmt::format("GL callback: {} [source={}, type={}, severity={}]", message, source, type, severity);
            if (type == GL_DEBUG_TYPE_ERROR) {
                LM_ERROR(str);
            }
            else {
                LM_INFO(str);
            }
        }, nullptr);
        glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE, GL_DONT_CARE, 0, nullptr, true);
        #endif

        // GL renderer
        if (!glrenderer.setup()) {
            return false;
        }

        // GL camera
        glcamera.reset(lm::Vec3(1,1,1), lm::Vec3(0), lm::Vec3(0,1,0), 30);

        return true;
    }

    void run(const std::function<void(int, int)>& updateFunc) {
        while (!glfwWindowShouldClose(window)) {
            // Setup new frame
            glfwPollEvents();
            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();

            // ----------------------------------------------------------------

            // Update camera
            glcamera.update(window);

            // Windows position and size
            ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Once);
            ImGui::SetNextWindowSize(ImVec2(400, 200), ImGuiCond_Once);

            // ----------------------------------------------------------------

            // General information
            ImGui::Begin("Information / Control");
            int display_w, display_h;
            {
                // FPS, framebuffer
                ImGui::Text("%.3f ms/frame (%.1f FPS)", 1000.0f / ImGui::GetIO().Framerate, ImGui::GetIO().Framerate);
                glfwGetFramebufferSize(window, &display_w, &display_h);
                ImGui::Text("Framebuffer size: (%d, %d)", display_w, display_h);

                // Demo window
                static bool showDemoWindow = false;
                ImGui::Checkbox("Demo Window", &showDemoWindow);
                if (showDemoWindow) {
                    ImGui::ShowDemoWindow(&showDemoWindow);
                }
            }
            ImGui::End();

            // ----------------------------------------------------------------

            // Scene window
            glscene.updateGUI();

            // ----------------------------------------------------------------

            // User-defined update function
            ImGui::SetNextWindowPos(ImVec2(0, 200), ImGuiCond_Once);
            updateFunc(display_w, display_h);

            // ----------------------------------------------------------------

            // Rendering
            ImGui::Render();
            glViewport(0, 0, display_w, display_h);
            glClearDepthf(1.f);
            glClear(GL_DEPTH_BUFFER_BIT);
            glClearColor(.45f, .55f, .6f, 1.f);
            glClear(GL_COLOR_BUFFER_BIT);
            glrenderer.render(glscene, glcamera);
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
            glfwSwapBuffers(window);
        };

        // --------------------------------------------------------------------

        // Shutdown the framework
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        glfwDestroyWindow(window);
        glfwTerminate();
    }
};