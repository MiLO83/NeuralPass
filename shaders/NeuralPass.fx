// NeuralPass.fx -- final compositor for NeuralPass.addon64.
// Keep this technique last in the ReShade technique order.

#include "ReShade.fxh"

uniform float NeuralPassStrength <
    ui_label = "Style strength";
    ui_type = "slider";
    ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
> = 1.0;

// Set by the add-on after validating the backbuffer format and swapchain color
// space together: 0 = SDR sRGB, 1 = linear scRGB, 2 = HDR10 PQ/BT.2020.
uniform int NeuralPassColorMode = 0;

uniform int NeuralPassDebug <
    ui_label = "Debug view";
    ui_type = "combo";
    ui_items = "Final\0Styled cache\0Validity\0";
> = 0;

uniform bool NeuralPassDiagonalSplit <
    ui_label = "Debug: diagonal before / after split";
    ui_tooltip = "Original in the upper-left; NeuralPass enhancement in the lower-right.";
> = false;

uniform float4 NeuralPassHudRect <
    ui_label = "HUD exclusion (left, top, right, bottom)";
    ui_type = "drag";
    ui_min = 0.0; ui_max = 1.0; ui_step = 0.001;
> = float4(0.0, 0.0, 0.0, 0.0);

texture NeuralPassStyled {
    Width = BUFFER_WIDTH;
    Height = BUFFER_HEIGHT;
    Format = RGBA8;
};
sampler NeuralPassStyledSampler {
    Texture = NeuralPassStyled;
    AddressU = CLAMP;
    AddressV = CLAMP;
};

texture NeuralPassValid {
    Width = BUFFER_WIDTH;
    Height = BUFFER_HEIGHT;
    Format = RGBA8;
};
sampler NeuralPassValidSampler {
    Texture = NeuralPassValid;
    AddressU = CLAMP;
    AddressV = CLAMP;
};

float in_hud_rect(float2 uv) {
    const bool enabled = NeuralPassHudRect.z > NeuralPassHudRect.x &&
                         NeuralPassHudRect.w > NeuralPassHudRect.y;
    return (enabled && uv.x >= NeuralPassHudRect.x && uv.x <= NeuralPassHudRect.z &&
                       uv.y >= NeuralPassHudRect.y && uv.y <= NeuralPassHudRect.w) ? 1.0 : 0.0;
}

float3 srgb_to_linear(float3 value) {
    value = saturate(value);
    return lerp(value / 12.92, pow((value + 0.055) / 1.055, 2.4),
                step(0.04045, value));
}

float3 linear_to_srgb(float3 value) {
    value = saturate(value);
    return lerp(value * 12.92, 1.055 * pow(value, 1.0 / 2.4) - 0.055,
                step(0.0031308, value));
}

float3 aces_fitted(float3 value) {
    value = max(value, 0.0);
    return saturate((value * (2.51 * value + 0.03)) /
                    (value * (2.43 * value + 0.59) + 0.14));
}

float3 pq_to_linear(float3 value) {
    const float m1 = 2610.0 / 16384.0;
    const float m2 = 2523.0 / 32.0;
    const float c1 = 3424.0 / 4096.0;
    const float c2 = 2413.0 / 128.0;
    const float c3 = 2392.0 / 128.0;
    const float3 p = pow(saturate(value), 1.0 / m2);
    return pow(max(p - c1, 0.0) / max(c2 - c3 * p, 1.0e-6), 1.0 / m1);
}

float3 linear_to_pq(float3 value) {
    const float m1 = 2610.0 / 16384.0;
    const float m2 = 2523.0 / 32.0;
    const float c1 = 3424.0 / 4096.0;
    const float c2 = 2413.0 / 128.0;
    const float c3 = 2392.0 / 128.0;
    const float3 p = pow(saturate(value), m1);
    return pow((c1 + c2 * p) / (1.0 + c3 * p), m2);
}

float3 source_scene_709(float3 source) {
    if (NeuralPassColorMode == 1) return max(source, 0.0);
    if (NeuralPassColorMode == 2) {
        const float3 bt2020 = pq_to_linear(source) * 125.0;
        return mul(float3x3(1.660491, -0.587641, -0.072850,
                           -0.124550, 1.132900, -0.008349,
                           -0.018151, -0.100579, 1.118730), bt2020);
    }
    return srgb_to_linear(source);
}

float3 styled_to_output(float3 source, float3 styled) {
    if (NeuralPassColorMode == 0) return styled;
    const float3 scene = source_scene_709(source);
    const float3 inference_source = linear_to_srgb(aces_fitted(scene));
    const float3 inference_linear = srgb_to_linear(inference_source);
    const float3 styled_linear = srgb_to_linear(styled);
    const float3 luma = float3(0.2126, 0.7152, 0.0722);
    const float ratio = clamp(dot(styled_linear, luma) /
                              max(dot(inference_linear, luma), 1.0e-4), 0.0, 4.0);
    const float target_luma = max(dot(scene, luma), 0.0) * ratio;
    const float styled_luma = max(dot(styled_linear, luma), 0.0);
    float3 output_scene = styled_luma > 1.0e-5
        ? styled_linear * (target_luma / styled_luma) : 0.0;
    if (NeuralPassColorMode == 1) return output_scene;
    const float3 bt2020 = mul(float3x3(0.627404, 0.329283, 0.043313,
                                      0.069097, 0.919540, 0.011362,
                                      0.016391, 0.088013, 0.895595), output_scene);
    return linear_to_pq(bt2020 / 125.0);
}

float4 NeuralPassPS(float4 position : SV_Position, float2 uv : TEXCOORD) : SV_Target {
    const float4 source = tex2D(ReShade::BackBuffer, uv);
    const float4 styled_srgb = tex2D(NeuralPassStyledSampler, uv);
    const float4 styled = float4(styled_to_output(source.rgb, styled_srgb.rgb),
                                 styled_srgb.a);
    // A small neighborhood feathers progressive tile boundaries and masks one-pixel holes.
    const float2 px = ReShade::PixelSize;
    float validity = tex2D(NeuralPassValidSampler, uv).r;
    validity = min(validity, tex2D(NeuralPassValidSampler, uv + float2(px.x, 0)).r);
    validity = min(validity, tex2D(NeuralPassValidSampler, uv - float2(px.x, 0)).r);
    validity = min(validity, tex2D(NeuralPassValidSampler, uv + float2(0, px.y)).r);
    validity = min(validity, tex2D(NeuralPassValidSampler, uv - float2(0, px.y)).r);
    validity *= 1.0 - in_hud_rect(uv);
    if (NeuralPassDebug == 1) return styled;
    if (NeuralPassDebug == 2) return float4(validity, validity, validity, 1.0);
    const float4 enhanced = lerp(source, styled, saturate(validity * NeuralPassStrength));
    if (NeuralPassDiagonalSplit) {
        // A top-right to bottom-left divider keeps equal screen area on each side.
        // Scale by pixel size so the divider stays visually consistent at any resolution.
        const float diagonal = uv.x + uv.y - 1.0;
        const float diagonal_pixel_width = max(ReShade::PixelSize.x, ReShade::PixelSize.y);
        const float divider = 1.0 - smoothstep(diagonal_pixel_width,
                                                diagonal_pixel_width * 3.0,
                                                abs(diagonal));
        const float4 comparison = diagonal >= 0.0 ? enhanced : source;
        const float4 divider_color = float4(1.0, 0.72, 0.18, 1.0);
        return lerp(comparison, divider_color, divider * 0.9);
    }
    return enhanced;
}

technique NeuralPass < ui_label = "NeuralPass (keep last)"; > {
    pass Present {
        VertexShader = PostProcessVS;
        PixelShader = NeuralPassPS;
    }
}
