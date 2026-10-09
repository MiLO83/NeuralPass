// NeuralPass.fx -- final compositor for NeuralPass.addon64.
// Keep this technique last in the ReShade technique order.

#include "ReShade.fxh"

uniform float NeuralPassStrength <
    ui_label = "Style strength";
    ui_type = "slider";
    ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
> = 1.0;

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

float4 NeuralPassPS(float4 position : SV_Position, float2 uv : TEXCOORD) : SV_Target {
    const float4 source = tex2D(ReShade::BackBuffer, uv);
    const float4 styled = tex2D(NeuralPassStyledSampler, uv);
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
