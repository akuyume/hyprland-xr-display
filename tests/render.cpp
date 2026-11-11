// SPDX-License-Identifier: GPL-3.0-only
#include "math.hpp"
#include "shaders.hpp"
#include "stereo.hpp"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <iostream>
#include <vector>

static void check(bool valid, const char *message) {
  if (!valid)
    throw std::runtime_error(message);
}

static GLuint compile(GLenum type, const char *source) {
  GLuint shader = glCreateShader(type);
  glShaderSource(shader, 1, &source, nullptr);
  glCompileShader(shader);
  GLint success;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
  check(success, "production shader did not compile");
  return shader;
}

int main() {
  EGLDisplay display = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA,
                                             EGL_DEFAULT_DISPLAY, nullptr);
  if (display == EGL_NO_DISPLAY || !eglInitialize(display, nullptr, nullptr)) {
    std::cout << "SKIP: surfaceless EGL unavailable\n";
    return 77;
  }
  EGLConfig config;
  EGLint count;
  const EGLint attributes[] = {EGL_SURFACE_TYPE,
                               EGL_PBUFFER_BIT,
                               EGL_RENDERABLE_TYPE,
                               EGL_OPENGL_ES3_BIT,
                               EGL_RED_SIZE,
                               8,
                               EGL_GREEN_SIZE,
                               8,
                               EGL_BLUE_SIZE,
                               8,
                               EGL_DEPTH_SIZE,
                               24,
                               EGL_NONE};
  eglChooseConfig(display, attributes, &config, 1, &count);
  if (!count)
    return 77;
  eglBindAPI(EGL_OPENGL_ES_API);
  const EGLint contextAttributes[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
  EGLContext context =
      eglCreateContext(display, config, EGL_NO_CONTEXT, contextAttributes);
  const EGLint size[] = {EGL_WIDTH, 512, EGL_HEIGHT, 256, EGL_NONE};
  EGLSurface surface = eglCreatePbufferSurface(display, config, size);
  if (!eglMakeCurrent(display, surface, surface, context))
    return 77;
  try {
    GLuint program = glCreateProgram();
    GLuint vertex = compile(GL_VERTEX_SHADER, xr::vertexShader);
    GLuint fragment = compile(GL_FRAGMENT_SHADER, xr::fragmentShader);
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glLinkProgram(program);
    GLint linked;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    check(linked, "production shader did not link");
    glUseProgram(program);
    GLuint vao, texture;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    std::vector<unsigned char> image(128 * 128 * 4, 0);
    for (int y = 0; y < 128; ++y)
      for (int x = 0; x < 128; ++x) {
        auto at = (y * 128 + x) * 4;
        image[at] = x >= 59 && x < 69 && y >= 59 && y < 69 ? 255 : 0;
        image[at + 3] = 255;
      }
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 128, 128, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, image.data());
    auto uniform = [&](const char *name) {
      return glGetUniformLocation(program, name);
    };
    glUniform1i(uniform("desktop"), 0);
    glUniform1i(uniform("flatView"), false);
    glUniform1i(uniform("flipY"), true);
    glUniform1i(uniform("opaque"), true);
    glUniform2f(uniform("fov"), std::tan(xr::rad(20)), std::tan(xr::rad(11.5)));
    glUniform2f(uniform("halfSize"), .5, .5);
    glUniform4f(uniform("planeRotation"), 0, 0, 0, 1);
    auto disparity = [&](double distance, double ipd, xr::Quat pose) {
      glClearColor(0, 0, 0, 1);
      glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
      glUniform3f(uniform("planeCenter"), 0, 0, -distance);
      glUniform4f(uniform("rotation"), pose.x, pose.y, pose.z, pose.w);
      glEnable(GL_SCISSOR_TEST);
      for (int eye = 0; eye < 2; ++eye) {
        glViewport(eye * 256, 0, 256, 256);
        glScissor(eye * 256, 0, 256, 256);
        glUniform1f(uniform("eyeOffset"), xr::eyeOffset(eye, ipd));
        glDrawArrays(GL_TRIANGLES, 0, 3);
      }
      glDisable(GL_SCISSOR_TEST);
      std::vector<unsigned char> pixels(512 * 256 * 4);
      glReadPixels(0, 0, 512, 256, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
      double x[2]{}, y[2]{}, samples[2]{};
      for (int row = 0; row < 256; ++row)
        for (int column = 0; column < 512; ++column) {
          if (pixels[(row * 512 + column) * 4] < 200)
            continue;
          int eye = column / 256;
          x[eye] += column % 256;
          y[eye] += row;
          ++samples[eye];
        }
      check(samples[0] > 0 && samples[1] > 0, "target missing from an eye");
      check(std::abs(y[0] / samples[0] - y[1] / samples[1]) < 1,
            "parallel eyes produced vertical disparity");
      return x[0] / samples[0] - x[1] / samples[1];
    };
    const double near = disparity(1, 64, {});
    const double far = disparity(3, 64, {});
    check(near > 20 && far > 5 && near > far * 2.5,
          "stereo disparity does not follow physical depth");
    check(disparity(1, 80, {}) > disparity(1, 45, {}),
          "IPD does not change disparity");
    auto tilted = xr::Quat::axis({0, 1, 0}, xr::rad(7)) *
                  xr::Quat::axis({1, 0, 0}, xr::rad(3)) *
                  xr::Quat::axis({0, 0, 1}, xr::rad(20));
    check(disparity(2, 64, tilted) > 8,
          "rotated eye baseline lost depth disparity");
    check(glGetError() == GL_NO_ERROR, "OpenGL test error");
    glDeleteTextures(1, &texture);
    glDeleteVertexArrays(1, &vao);
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    glDeleteProgram(program);
    std::cout << "PASS: production stereo shader, depth, IPD, rotated eyes and "
                 "viewport separation\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  eglDestroySurface(display, surface);
  eglDestroyContext(display, context);
  eglTerminate(display);
}
