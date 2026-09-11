#if defined(REDCLAW_ENABLE_QT_OPENGL)
#include "playback_backends.h"
#include <QOpenGLBuffer>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>
#include <QOpenGLWidget>
#include <QSurfaceFormat>
#include <algorithm>
namespace redclaw::ui {
namespace {
class OpenGLPlaybackWidget final : public QOpenGLWidget,
                                   protected QOpenGLFunctions,
                                   public PlaybackCanvas {
 public:
  explicit OpenGLPlaybackWidget(QWidget* parent = nullptr)
      : QOpenGLWidget(parent) {
    // Request a 3.3 core profile for the texture-blit shaders.
    QSurfaceFormat fmt;
    fmt.setVersion(3, 3);
    fmt.setProfile(QSurfaceFormat::CoreProfile);
    setFormat(fmt);
    setMinimumSize(320, 180);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
  }

  ~OpenGLPlaybackWidget() override {
    makeCurrent();
    if (texture_id_ != 0) {
      glDeleteTextures(1, &texture_id_);
      texture_id_ = 0;
    }
    vbo_.destroy();
    vao_.destroy();
    doneCurrent();
  }

  [[nodiscard]] QString backend_name() const override { return "opengl"; }

  void clear_frame() override {
    pending_frame_ = DirectFrameData{};
    has_pending_frame_ = false;
    if (isValid()) {
      makeCurrent();
      if (texture_id_ != 0) {
        glDeleteTextures(1, &texture_id_);
        texture_id_ = 0;
      }
      doneCurrent();
    }
    tex_width_ = 0;
    tex_height_ = 0;
    update();
  }

  PresentOutcome present_frame(const DirectFrameData& frame, QString* error_detail) override {
    if (frame.width == 0 || frame.height == 0 || frame.pixels.empty()) {
      if (error_detail != nullptr) {
        *error_detail = "OpenGL renderer: empty frame data";
      }
      return PresentOutcome::kFailed;
    }
    pending_frame_ = frame;
    has_pending_frame_ = true;
    update();
    return PresentOutcome::kPresented;
  }

  PresentOutcome present_image(const QImage& image, QString* error_detail) override {
    if (image.isNull()) {
      if (error_detail != nullptr) {
        *error_detail = "OpenGL renderer: null image";
      }
      return PresentOutcome::kFailed;
    }
    const QImage bgra = image.convertToFormat(QImage::Format_RGB32);
    DirectFrameData frame;
    frame.width      = static_cast<quint32>(bgra.width());
    frame.height     = static_cast<quint32>(bgra.height());
    frame.row_pitch  = static_cast<quint32>(bgra.bytesPerLine());
    frame.timestamp_ms = 0;
    frame.pixels.assign(bgra.constBits(),
                        bgra.constBits() + static_cast<std::size_t>(bgra.sizeInBytes()));
    return present_frame(frame, error_detail);
  }

 protected:
  void initializeGL() override {
    initializeOpenGLFunctions();

    // GLSL 3.30 core – position at location 0, texcoord at location 1.
    static const char kVS[] = R"glsl(
#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUV;
out vec2 vUV;
void main() { gl_Position = vec4(aPos, 0.0, 1.0); vUV = aUV; }
)glsl";

    // GL_BGRA upload maps bytes to internal RGBA correctly: no shader swizzle
    // needed.  The texture() call returns (R, G, B, A) with the correct values.
    static const char kFS[] = R"glsl(
#version 330 core
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uTex;
void main() { fragColor = texture(uTex, vUV); }
)glsl";

    if (!shader_.addShaderFromSourceCode(QOpenGLShader::Vertex, kVS) ||
        !shader_.addShaderFromSourceCode(QOpenGLShader::Fragment, kFS) ||
        !shader_.link()) {
      gl_init_ok_ = false;
      return;
    }

    vao_.create();
    vbo_.create();
    vbo_.setUsagePattern(QOpenGLBuffer::DynamicDraw);
    vbo_.bind();
    vbo_.allocate(sizeof(float) * 24);  // 6 vertices × 4 floats each
    vbo_.release();

    vao_.bind();
    vbo_.bind();
    const int stride = static_cast<int>(4 * sizeof(float));
    shader_.enableAttributeArray(0);
    shader_.setAttributeBuffer(0, GL_FLOAT, 0,               2, stride);
    shader_.enableAttributeArray(1);
    shader_.setAttributeBuffer(1, GL_FLOAT, 2 * sizeof(float), 2, stride);
    vao_.release();
    vbo_.release();

    gl_init_ok_ = true;
  }

  void resizeGL(int w, int h) override {
    glViewport(0, 0, w, h);
  }

  void paintGL() override {
    glClearColor(0.008f, 0.024f, 0.090f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    if (!gl_init_ok_) return;

    // Upload any pending frame.
    if (has_pending_frame_) {
      upload_texture(pending_frame_);
      has_pending_frame_ = false;
    }
    if (texture_id_ == 0) return;

    // Update aspect-ratio vertices.
    update_quad_vertices();

    shader_.bind();
    shader_.setUniformValue("uTex", 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture_id_);
    vao_.bind();
    glDrawArrays(GL_TRIANGLES, 0, 6);
    vao_.release();
    glBindTexture(GL_TEXTURE_2D, 0);
    shader_.release();
  }

 private:
  void upload_texture(const DirectFrameData& frame) {
    const quint32 content_x = frame.content_rect_x;
    const quint32 content_y = frame.content_rect_y;
    const quint32 content_width = frame.content_rect_width == 0
        ? frame.width : frame.content_rect_width;
    const quint32 content_height = frame.content_rect_height == 0
        ? frame.height : frame.content_rect_height;
    const auto* content_pixels = frame.pixels.data()
        + static_cast<std::size_t>(content_y) * frame.row_pitch
        + static_cast<std::size_t>(content_x) * 4U;
    if (texture_id_ != 0 &&
        (tex_width_ != content_width || tex_height_ != content_height)) {
      glDeleteTextures(1, &texture_id_);
      texture_id_ = 0;
    }
    if (texture_id_ == 0) {
      glGenTextures(1, &texture_id_);
      glBindTexture(GL_TEXTURE_2D, texture_id_);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      glPixelStorei(GL_UNPACK_ROW_LENGTH, static_cast<GLint>(frame.row_pitch / 4U));
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8,
                   static_cast<GLsizei>(content_width),
                   static_cast<GLsizei>(content_height),
                   0, GL_BGRA, GL_UNSIGNED_BYTE, content_pixels);
      glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
      tex_width_  = content_width;
      tex_height_ = content_height;
    } else {
      glBindTexture(GL_TEXTURE_2D, texture_id_);
      glPixelStorei(GL_UNPACK_ROW_LENGTH, static_cast<GLint>(frame.row_pitch / 4U));
      glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                      static_cast<GLsizei>(content_width),
                      static_cast<GLsizei>(content_height),
                      GL_BGRA, GL_UNSIGNED_BYTE, content_pixels);
      glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    }
  }

  void update_quad_vertices() {
    if (!vbo_.isCreated() || tex_width_ == 0 || tex_height_ == 0 ||
        width() == 0 || height() == 0) {
      return;
    }
    const float wa = static_cast<float>(width())  / static_cast<float>(height());
    const float fa = static_cast<float>(tex_width_) / static_cast<float>(tex_height_);
    float x = 1.0f, y = 1.0f;
    if (fa > wa) { y = wa / fa; } else { x = fa / wa; }
    // 6 vertices (two triangles), each: pos.xy  uv.xy
    const float v[] = {
      -x, -y,  0.f, 1.f,
      -x,  y,  0.f, 0.f,
       x, -y,  1.f, 1.f,
       x, -y,  1.f, 1.f,
      -x,  y,  0.f, 0.f,
       x,  y,  1.f, 0.f,
    };
    vbo_.bind();
    vbo_.write(0, v, static_cast<int>(sizeof(v)));
    vbo_.release();
  }

  QOpenGLShaderProgram shader_;
  QOpenGLBuffer vbo_{ QOpenGLBuffer::VertexBuffer };
  QOpenGLVertexArrayObject vao_;
  GLuint   texture_id_  = 0;
  quint32  tex_width_   = 0;
  quint32  tex_height_  = 0;
  bool     gl_init_ok_  = false;

  DirectFrameData pending_frame_;
  bool            has_pending_frame_ = false;
};
}  // namespace
PlaybackWidgetResult create_opengl_playback_renderer(QWidget* parent) {
    auto* widget = new OpenGLPlaybackWidget(parent);
    return {widget, widget};
}
}  // namespace redclaw::ui
#endif
