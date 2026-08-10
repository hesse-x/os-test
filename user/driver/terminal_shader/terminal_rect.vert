#version 450
layout(location = 0) in vec2 in_pos;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec4 in_color;
layout(push_constant) uniform Projection { vec2 extent; } projection;
layout(location = 0) out vec4 color;
void main() {
  vec2 ndc = vec2(in_pos.x * 2.0 / projection.extent.x - 1.0,
                  in_pos.y * 2.0 / projection.extent.y - 1.0);
  gl_Position = vec4(ndc, 0.0, 1.0);
  color = in_color;
}
