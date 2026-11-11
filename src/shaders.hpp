// SPDX-License-Identifier: GPL-3.0-only
#pragma once
namespace xr {
inline constexpr const char *vertexShader = R"(#version 300 es
precision highp float;
out vec2 uv;
void main() {
  vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
  uv = p;
  gl_Position = vec4(p * 2. - 1., 0., 1.);
})";

inline constexpr const char *fragmentShader = R"(#version 300 es
precision highp float;
in vec2 uv;
out vec4 color;
uniform sampler2D desktop;
uniform vec4 rotation;
uniform vec4 planeRotation;
uniform vec3 planeCenter;
uniform vec2 fov;
uniform vec2 halfSize;
uniform float eyeOffset;
uniform bool flatView;
uniform bool flipY;
uniform bool swapRB;
uniform bool opaque;
vec3 rotateQ(vec4 q, vec3 v) {
  return v + 2. * cross(q.xyz, cross(q.xyz, v) + q.w * v);
}
void main() {
  vec2 tc = uv;
  float depth = .5;
  if (!flatView) {
    vec3 origin = rotateQ(rotation, vec3(eyeOffset, 0., 0.));
    vec3 ray = rotateQ(rotation, vec3((uv * 2. - 1.) * fov, -1.));
    vec4 inversePlane = vec4(-planeRotation.xyz, planeRotation.w);
    vec3 localOrigin = rotateQ(inversePlane, origin - planeCenter);
    vec3 localRay = rotateQ(inversePlane, ray);
    if (localRay.z >= -.0001) discard;
    float t = -localOrigin.z / localRay.z;
    if (t <= 0.) discard;
    tc = (localOrigin.xy + t * localRay.xy) / (2. * halfSize) + .5;
    depth = t / (t + 1.);
  }
  if (any(lessThan(tc, vec2(0.))) || any(greaterThan(tc, vec2(1.)))) discard;
  if (!flipY) tc.y = 1. - tc.y;
  vec4 sampled = texture(desktop, tc);
  if (opaque) sampled.a = 1.;
  if (sampled.a < .01) discard;
  if (swapRB) sampled.rgb = sampled.bgr;
  color = sampled;
  gl_FragDepth = depth;
})";
} // namespace xr
