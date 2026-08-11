#version 450

layout(set = 0, binding = 0) uniform sampler2D blurred_image;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 out_color;

layout(push_constant) uniform CompositeData {
  layout(offset = 80) vec4 tint;
  vec4 geometry; // width, height, corner radius, opacity
  vec4 sample_region; // output-normalized x, y, width, height
} data;

void main() {
  vec2 local_uv = (uv - data.sample_region.xy) / data.sample_region.zw;
  vec2 half_size = data.geometry.xy * 0.5;
  float radius = clamp(data.geometry.z, 0.0, min(half_size.x, half_size.y));
  vec2 point = abs((local_uv - vec2(0.5)) * data.geometry.xy);
  vec2 q = point - (half_size - vec2(radius));
  float distance = length(max(q, vec2(0.0))) +
                   min(max(q.x, q.y), 0.0) - radius;
  float coverage = 1.0 - smoothstep(-max(fwidth(distance), 0.5),
                                    max(fwidth(distance), 0.5), distance);
  vec4 blurred = texture(blurred_image, uv);
  vec3 color = mix(blurred.rgb, data.tint.rgb, data.tint.a);
  out_color = vec4(color, 1.0) * (coverage * data.geometry.w);
}
