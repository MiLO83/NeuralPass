#version 450

layout(constant_id = 0) const uint material_low = 0u;
layout(constant_id = 1) const uint material_high = 0u;

// Location 31 is a sentinel patched to the application's inferred UV input
// location before the companion graphics pipeline is created.
layout(location = 31) in vec2 neuralpass_uv;

layout(location = 0) out uvec4 surface_out;
layout(location = 1) out vec4 gradients_out;
layout(location = 2) out vec4 source_out;
layout(location = 3) out float depth_out;

void main()
{
    vec2 dx = dFdx(neuralpass_uv);
    vec2 dy = dFdy(neuralpass_uv);
    surface_out = uvec4(material_low, material_high,
                        floatBitsToUint(neuralpass_uv.x),
                        floatBitsToUint(neuralpass_uv.y));
    gradients_out = vec4(dx.x, dy.x, dx.y, dy.y);
    source_out = vec4(uintBitsToFloat(0x7fc00000u));
    depth_out = gl_FragCoord.z;
}
