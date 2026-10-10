#version 450

layout(constant_id = 0) const uint material_low = 0u;
layout(constant_id = 1) const uint material_high = 0u;

// Image and sampler use distinct sentinels so both application descriptor
// locations can be patched without guessing a linkage between their bindings.
layout(location = 31) in vec2 neuralpass_uv;
layout(set = 30, binding = 30) uniform texture2D neuralpass_source_image;
layout(set = 29, binding = 29) uniform sampler neuralpass_source_sampler;

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
    vec4 source = textureGrad(sampler2D(neuralpass_source_image,
                                        neuralpass_source_sampler),
                              neuralpass_uv, dx, dy);
    if (source.a <= 0.000001)
        discard;
    source_out = source;
    depth_out = gl_FragCoord.z;
}
