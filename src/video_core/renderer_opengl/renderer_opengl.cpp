// Copyright 2014 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <glad/glad.h>
#include "common/assert.h"
#include "common/logging/log.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/frontend/emu_window.h"
#include "core/memory.h"
#include "core/settings.h"
#include "core/tracer/recorder.h"
#include "video_core/renderer_opengl/renderer_opengl.h"
#include "video_core/utils.h"
#include "video_core/video_core.h"

static const char vertex_shader[] = R"(
#version 150 core

in vec2 vert_position;
in vec2 vert_tex_coord;
out vec2 frag_tex_coord;

// This is a truncated 3x3 matrix for 2D transformations:
// The upper-left 2x2 submatrix performs scaling/rotation/mirroring.
// The third column performs translation.
// The third row could be used for projection, which we don't need in 2D. It hence is assumed to
// implicitly be [0, 0, 1]
uniform mat3x2 modelview_matrix;

void main() {
    // Multiply input position by the rotscale part of the matrix and then manually translate by
    // the last column. This is equivalent to using a full 3x3 matrix and expanding the vector
    // to `vec3(vert_position.xy, 1.0)`
    gl_Position = vec4(mat2(modelview_matrix) * vert_position + modelview_matrix[2], 0.0, 1.0);
    frag_tex_coord = vert_tex_coord;
}
)";

static const char fragment_shader[] = R"(
#version 150 core

in vec2 frag_tex_coord;
out vec4 color;

uniform sampler2D color_texture;

void main() {
    // Swap RGBA -> ABGR so we don't have to do this on the CPU. This needs to change if we have to
    // support more framebuffer pixel formats.
    color = texture(color_texture, frag_tex_coord);
}
)";

/**
 * Vertex structure that the drawn screen rectangles are composed of.
 */
struct ScreenRectVertex {
    ScreenRectVertex(GLfloat x, GLfloat y, GLfloat u, GLfloat v) {
        position[0] = x;
        position[1] = y;
        tex_coord[0] = u;
        tex_coord[1] = v;
    }

    GLfloat position[2];
    GLfloat tex_coord[2];
};

/**
 * Defines a 1:1 pixel ortographic projection matrix with (0,0) on the top-left
 * corner and (width, height) on the lower-bottom.
 *
 * The projection part of the matrix is trivial, hence these operations are represented
 * by a 3x2 matrix.
 */
static std::array<GLfloat, 3 * 2> MakeOrthographicMatrix(const float width, const float height) {
    std::array<GLfloat, 3 * 2> matrix; // Laid out in column-major order

    // clang-format off
    matrix[0] = 2.f / width; matrix[2] = 0.f;           matrix[4] = -1.f;
    matrix[1] = 0.f;         matrix[3] = -2.f / height; matrix[5] = 1.f;
    // Last matrix row is implicitly assumed to be [0, 0, 1].
    // clang-format on

    return matrix;
}

RendererOpenGL::RendererOpenGL() = default;
RendererOpenGL::~RendererOpenGL() = default;

/// Swap buffers (render frame)
void RendererOpenGL::SwapBuffers(boost::optional<const Tegra::FramebufferConfig&> framebuffer) {
    Core::System::GetInstance().perf_stats.EndSystemFrame();

    // Maintain the rasterizer's state as a priority
    OpenGLState prev_state = OpenGLState::GetCurState();
    state.Apply();

    if (framebuffer != boost::none) {
        // If framebuffer is provided, reload it from memory to a texture
        if (screen_info.texture.width != (GLsizei)framebuffer->width ||
            screen_info.texture.height != (GLsizei)framebuffer->height ||
            screen_info.texture.pixel_format != framebuffer->pixel_format) {
            // Reallocate texture if the framebuffer size has changed.
            // This is expected to not happen very often and hence should not be a
            // performance problem.
            ConfigureFramebufferTexture(screen_info.texture, *framebuffer);
        }

        // Load the framebuffer from memory, draw it to the screen, and swap buffers
        LoadFBToScreenInfo(*framebuffer, screen_info);
        DrawScreen();
        render_window->SwapBuffers();
    }

    render_window->PollEvents();

    Core::System::GetInstance().frame_limiter.DoFrameLimiting(CoreTiming::GetGlobalTimeUs());
    Core::System::GetInstance().perf_stats.BeginSystemFrame();

    // Restore the rasterizer state
    prev_state.Apply();
    RefreshRasterizerSetting();
}

/**
 * Loads framebuffer from emulated memory into the active OpenGL texture.
 */
void RendererOpenGL::LoadFBToScreenInfo(const Tegra::FramebufferConfig& framebuffer,
                                        ScreenInfo& screen_info) {
    const u32 bytes_per_pixel{Tegra::FramebufferConfig::BytesPerPixel(framebuffer.pixel_format)};
    const u64 size_in_bytes{framebuffer.stride * framebuffer.height * bytes_per_pixel};
    const VAddr framebuffer_addr{framebuffer.address + framebuffer.offset};

    // Framebuffer orientation handling
    framebuffer_transform_flags = framebuffer.transform_flags;

    // Ensure no bad interactions with GL_UNPACK_ALIGNMENT, which by default
    // only allows rows to have a memory alignement of 4.
    ASSERT(framebuffer.stride % 4 == 0);

    if (!Rasterizer()->AccelerateDisplay(framebuffer, framebuffer_addr, framebuffer.stride,
                                         screen_info)) {
        // Reset the screen info's display texture to its own permanent texture
        screen_info.display_texture = screen_info.texture.resource.handle;
        screen_info.display_texcoords = MathUtil::Rectangle<float>(0.f, 0.f, 1.f, 1.f);

        Memory::RasterizerFlushVirtualRegion(framebuffer_addr, size_in_bytes,
                                             Memory::FlushMode::Flush);

        VideoCore::MortonCopyPixels128(framebuffer.width, framebuffer.height, bytes_per_pixel, 4,
                                       Memory::GetPointer(framebuffer_addr),
                                       gl_framebuffer_data.data(), true);

        state.texture_units[0].texture_2d = screen_info.texture.resource.handle;
        state.Apply();

        glActiveTexture(GL_TEXTURE0);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, static_cast<GLint>(framebuffer.stride));

        // Update existing texture
        // TODO: Test what happens on hardware when you change the framebuffer dimensions so that
        //       they differ from the LCD resolution.
        // TODO: Applications could theoretically crash yuzu here by specifying too large
        //       framebuffer sizes. We should make sure that this cannot happen.
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, framebuffer.width, framebuffer.height,
                        screen_info.texture.gl_format, screen_info.texture.gl_type,
                        gl_framebuffer_data.data());

        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

        state.texture_units[0].texture_2d = 0;
        state.Apply();
    }
}

/**
 * Fills active OpenGL texture with the given RGB color. Since the color is solid, the texture can
 * be 1x1 but will stretch across whatever it's rendered on.
 */
void RendererOpenGL::LoadColorToActiveGLTexture(u8 color_r, u8 color_g, u8 color_b, u8 color_a,
                                                const TextureInfo& texture) {
    state.texture_units[0].texture_2d = texture.resource.handle;
    state.Apply();

    glActiveTexture(GL_TEXTURE0);
    u8 framebuffer_data[4] = {color_a, color_b, color_g, color_r};

    // Update existing texture
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, framebuffer_data);

    state.texture_units[0].texture_2d = 0;
    state.Apply();
}

/**
 * Initializes the OpenGL state and creates persistent objects.
 */
void RendererOpenGL::InitOpenGLObjects() {
    glClearColor(Settings::values.bg_red, Settings::values.bg_green, Settings::values.bg_blue,
                 0.0f);

    // Link shaders and get variable locations
    shader.CreateFromSource(vertex_shader, nullptr, fragment_shader);
    state.draw.shader_program = shader.handle;
    state.Apply();
    uniform_modelview_matrix = glGetUniformLocation(shader.handle, "modelview_matrix");
    uniform_color_texture = glGetUniformLocation(shader.handle, "color_texture");
    attrib_position = glGetAttribLocation(shader.handle, "vert_position");
    attrib_tex_coord = glGetAttribLocation(shader.handle, "vert_tex_coord");

    // Generate VBO handle for drawing
    vertex_buffer.Create();

    // Generate VAO
    vertex_array.Create();

    state.draw.vertex_array = vertex_array.handle;
    state.draw.vertex_buffer = vertex_buffer.handle;
    state.draw.uniform_buffer = 0;
    state.Apply();

    // Attach vertex data to VAO
    glBufferData(GL_ARRAY_BUFFER, sizeof(ScreenRectVertex) * 4, nullptr, GL_STREAM_DRAW);
    glVertexAttribPointer(attrib_position, 2, GL_FLOAT, GL_FALSE, sizeof(ScreenRectVertex),
                          (GLvoid*)offsetof(ScreenRectVertex, position));
    glVertexAttribPointer(attrib_tex_coord, 2, GL_FLOAT, GL_FALSE, sizeof(ScreenRectVertex),
                          (GLvoid*)offsetof(ScreenRectVertex, tex_coord));
    glEnableVertexAttribArray(attrib_position);
    glEnableVertexAttribArray(attrib_tex_coord);

    // Allocate textures for the screen
    screen_info.texture.resource.Create();

    // Allocation of storage is deferred until the first frame, when we
    // know the framebuffer size.

    state.texture_units[0].texture_2d = screen_info.texture.resource.handle;
    state.Apply();

    glActiveTexture(GL_TEXTURE0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    screen_info.display_texture = screen_info.texture.resource.handle;

    state.texture_units[0].texture_2d = 0;
    state.Apply();

    // Clear screen to black
    LoadColorToActiveGLTexture(0, 0, 0, 0, screen_info.texture);
}

void RendererOpenGL::ConfigureFramebufferTexture(TextureInfo& texture,
                                                 const Tegra::FramebufferConfig& framebuffer) {

    texture.width = framebuffer.width;
    texture.height = framebuffer.height;

    GLint internal_format;
    switch (framebuffer.pixel_format) {
    case Tegra::FramebufferConfig::PixelFormat::ABGR8:
        internal_format = GL_RGBA;
        texture.gl_format = GL_RGBA;
        texture.gl_type = GL_UNSIGNED_INT_8_8_8_8_REV;
        gl_framebuffer_data.resize(texture.width * texture.height * 4);
        break;
    default:
        UNREACHABLE();
    }

    state.texture_units[0].texture_2d = texture.resource.handle;
    state.Apply();

    glActiveTexture(GL_TEXTURE0);
    glTexImage2D(GL_TEXTURE_2D, 0, internal_format, texture.width, texture.height, 0,
                 texture.gl_format, texture.gl_type, nullptr);

    state.texture_units[0].texture_2d = 0;
    state.Apply();
}

void RendererOpenGL::DrawScreenTriangles(const ScreenInfo& screen_info, float x, float y, float w,
                                         float h) {
    const auto& texcoords = screen_info.display_texcoords;
    auto left = texcoords.left;
    auto right = texcoords.right;
    if (framebuffer_transform_flags != Tegra::FramebufferConfig::TransformFlags::Unset) {
        if (framebuffer_transform_flags == Tegra::FramebufferConfig::TransformFlags::FlipV) {
            // Flip the framebuffer vertically
            left = texcoords.right;
            right = texcoords.left;
        } else {
            // Other transformations are unsupported
            NGLOG_CRITICAL(Render_OpenGL, "Unsupported framebuffer_transform_flags={}",
                           static_cast<u32>(framebuffer_transform_flags));
            UNIMPLEMENTED();
        }
    }

    std::array<ScreenRectVertex, 4> vertices = {{
        ScreenRectVertex(x, y, texcoords.top, left),
        ScreenRectVertex(x + w, y, texcoords.bottom, left),
        ScreenRectVertex(x, y + h, texcoords.top, right),
        ScreenRectVertex(x + w, y + h, texcoords.bottom, right),
    }};

    state.texture_units[0].texture_2d = screen_info.display_texture;
    state.Apply();

    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(vertices), vertices.data());
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    state.texture_units[0].texture_2d = 0;
    state.Apply();
}

/**
 * Draws the emulated screens to the emulator window.
 */
void RendererOpenGL::DrawScreen() {
    const auto& layout = render_window->GetFramebufferLayout();
    const auto& screen = layout.screen;

    glViewport(0, 0, layout.width, layout.height);
    glClear(GL_COLOR_BUFFER_BIT);

    // Set projection matrix
    std::array<GLfloat, 3 * 2> ortho_matrix =
        MakeOrthographicMatrix((float)layout.width, (float)layout.height);
    glUniformMatrix3x2fv(uniform_modelview_matrix, 1, GL_FALSE, ortho_matrix.data());

    // Bind texture in Texture Unit 0
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(uniform_color_texture, 0);

    DrawScreenTriangles(screen_info, (float)screen.left, (float)screen.top,
                        (float)screen.GetWidth(), (float)screen.GetHeight());

    m_current_frame++;
}

/// Updates the framerate
void RendererOpenGL::UpdateFramerate() {}

/**
 * Set the emulator window to use for renderer
 * @param window EmuWindow handle to emulator window to use for rendering
 */
void RendererOpenGL::SetWindow(EmuWindow* window) {
    render_window = window;
}

static const char* GetSource(GLenum source) {
#define RET(s)                                                                                     \
    case GL_DEBUG_SOURCE_##s:                                                                      \
        return #s
    switch (source) {
        RET(API);
        RET(WINDOW_SYSTEM);
        RET(SHADER_COMPILER);
        RET(THIRD_PARTY);
        RET(APPLICATION);
        RET(OTHER);
    default:
        UNREACHABLE();
    }
#undef RET
}

static const char* GetType(GLenum type) {
#define RET(t)                                                                                     \
    case GL_DEBUG_TYPE_##t:                                                                        \
        return #t
    switch (type) {
        RET(ERROR);
        RET(DEPRECATED_BEHAVIOR);
        RET(UNDEFINED_BEHAVIOR);
        RET(PORTABILITY);
        RET(PERFORMANCE);
        RET(OTHER);
        RET(MARKER);
    default:
        UNREACHABLE();
    }
#undef RET
}

static void APIENTRY DebugHandler(GLenum source, GLenum type, GLuint id, GLenum severity,
                                  GLsizei length, const GLchar* message, const void* user_param) {
    const char format[] = "{} {} {}: {}";
    const char* const str_source = GetSource(source);
    const char* const str_type = GetType(type);

    switch (severity) {
    case GL_DEBUG_SEVERITY_HIGH:
        NGLOG_ERROR(Render_OpenGL, format, str_source, str_type, id, message);
        break;
    case GL_DEBUG_SEVERITY_MEDIUM:
        NGLOG_WARNING(Render_OpenGL, format, str_source, str_type, id, message);
        break;
    case GL_DEBUG_SEVERITY_NOTIFICATION:
    case GL_DEBUG_SEVERITY_LOW:
        NGLOG_DEBUG(Render_OpenGL, format, str_source, str_type, id, message);
        break;
    }
}

/// Initialize the renderer
bool RendererOpenGL::Init() {
    render_window->MakeCurrent();

    if (GLAD_GL_KHR_debug) {
        glEnable(GL_DEBUG_OUTPUT);
        glDebugMessageCallback(DebugHandler, nullptr);
    }

    const char* gl_version{reinterpret_cast<char const*>(glGetString(GL_VERSION))};
    const char* gpu_vendor{reinterpret_cast<char const*>(glGetString(GL_VENDOR))};
    const char* gpu_model{reinterpret_cast<char const*>(glGetString(GL_RENDERER))};

    NGLOG_INFO(Render_OpenGL, "GL_VERSION: {}", gl_version);
    NGLOG_INFO(Render_OpenGL, "GL_VENDOR: {}", gpu_vendor);
    NGLOG_INFO(Render_OpenGL, "GL_RENDERER: {}", gpu_model);

    Core::Telemetry().AddField(Telemetry::FieldType::UserSystem, "GPU_Vendor", gpu_vendor);
    Core::Telemetry().AddField(Telemetry::FieldType::UserSystem, "GPU_Model", gpu_model);
    Core::Telemetry().AddField(Telemetry::FieldType::UserSystem, "GPU_OpenGL_Version", gl_version);

    if (!GLAD_GL_VERSION_3_3) {
        return false;
    }

    InitOpenGLObjects();

    RefreshRasterizerSetting();

    return true;
}

/// Shutdown the renderer
void RendererOpenGL::ShutDown() {}
