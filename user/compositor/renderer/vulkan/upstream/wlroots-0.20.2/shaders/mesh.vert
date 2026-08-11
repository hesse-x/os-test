#version 450

layout(push_constant, row_major) uniform UBO {
  mat4 projection;
  vec2 uv_offset;
  vec2 uv_size;
} data;

layout(location = 0) in vec2 position;
layout(location = 1) in vec2 texcoord;
layout(location = 0) out vec2 uv;

void main() {
  uv = data.uv_offset + texcoord * data.uv_size;
  gl_Position = data.projection * vec4(position, 0.0, 1.0);
}
