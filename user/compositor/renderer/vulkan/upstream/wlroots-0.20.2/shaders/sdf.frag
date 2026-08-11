#version 450

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 out_color;

layout(push_constant) uniform UBO {
  layout(offset = 80) vec4 fill;
  vec4 border;
  vec4 geometry; // width, height, radius, border width
} data;

void main() {
  vec2 half_size = data.geometry.xy * 0.5;
  float radius = clamp(data.geometry.z, 0.0, min(half_size.x, half_size.y));
  vec2 point = abs((uv - vec2(0.5)) * data.geometry.xy);
  vec2 q = point - (half_size - vec2(radius));
  float distance = length(max(q, vec2(0.0))) + min(max(q.x, q.y), 0.0) - radius;
  float aa = max(fwidth(distance), 0.5);
  float outer = 1.0 - smoothstep(-aa, aa, distance);
  float inner = 1.0 - smoothstep(-aa, aa, distance + data.geometry.w);
  vec4 color = mix(data.border, data.fill, inner);
  // This pipeline uses premultiplied-alpha blending.
  out_color = vec4(color.rgb * color.a, color.a) * outer;
}
