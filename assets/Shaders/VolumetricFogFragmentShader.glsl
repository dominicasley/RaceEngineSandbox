#version 450
// The volumetric fog's apply: the half-resolution march lifted to the view's size and laid over
// the world (2026-08-26; the integration itself is FogMarchFragmentShader's since the split).
//
// The march writes (in-scattered radiance, transmittance) per half-resolution pixel with its
// blending off, so both arrive here as data. What this pass owes the picture is the lift: a plain
// bilinear read blends the four march texels under a pixel whatever surface each integrated to,
// and a building's fog then bleeds onto the sky beside it. So each of the four is weighted by its
// bilinear share *and* by how much its depth — read from the full-resolution prepass at that
// texel's own centre — looks like this pixel's, which is the same joint-bilateral test the
// occlusion upsample applies, keyed on depth alone because the medium does not care which way a
// surface faces.
//
// SET_POST_PROCESS, POST_INPUT_BINDING and POST_INPUTS are defined by the renderer from
// Graphics/Api/RenderContract.cppm; this file must not spell one of those numbers.

layout(location = 0) out vec4 fragColor;
layout(location = 0) in vec2 textureCoordinates;

// Three inputs, in the order the pass declared them: the world buffer, the march's
// (in-scatter, transmittance) at half resolution, and the occlusion prepass whose alpha is the
// view depth the march integrated each texel to.
layout(set = SET_POST_PROCESS, binding = POST_INPUT_BINDING) uniform sampler2D inputs[POST_INPUTS];

layout(push_constant) uniform PassParameters {
    vec4 tone;   // unread here: the fog works in radiance, long before the tone map
    vec4 pass;   // x target level, y target levels, zw target size
    vec4 view;
    vec4 effect;
} params;

// Whether two view depths belong to surfaces the medium would fog alike. Zero depth is the sky,
// which the march integrates at the medium's own reach — compatible only with itself.
float fogDepthWeight(float centreDepth, float sampleDepth)
{
    if (centreDepth <= 0.0 || sampleDepth <= 0.0)
    {
        return centreDepth <= 0.0 && sampleDepth <= 0.0 ? 1.0 : 0.0;
    }

    return max(0.0, 1.0 - abs(sampleDepth - centreDepth) / (0.05 * centreDepth));
}

void main()
{
    vec3 colour = texture(inputs[0], textureCoordinates).rgb;
    float centreDepth = texture(inputs[2], textureCoordinates).a;

    // The four march texels this pixel sits between, addressed in the march's own grid — the
    // target size in params.pass.zw is the full view and says nothing about the source.
    vec2 sourceSize = vec2(textureSize(inputs[1], 0));
    vec2 sourcePosition = textureCoordinates * sourceSize - 0.5;
    vec2 base = floor(sourcePosition);
    vec2 fraction = sourcePosition - base;

    vec4 total = vec4(0.0);
    float weightSum = 0.0;

    for (int y = 0; y <= 1; y++)
    {
        for (int x = 0; x <= 1; x++)
        {
            vec2 texelCentre = (base + vec2(x, y) + 0.5) / sourceSize;
            float sampleDepth = texture(inputs[2], texelCentre).a;

            float bilinear = (x == 1 ? fraction.x : 1.0 - fraction.x) * (y == 1 ? fraction.y : 1.0 - fraction.y);
            float weight = bilinear * fogDepthWeight(centreDepth, sampleDepth);

            total += texture(inputs[1], texelCentre) * weight;
            weightSum += weight;
        }
    }

    // Every candidate rejected means this pixel's surface fell between the march's samples — a
    // sliver — and the plain bilinear read is the honest answer there.
    vec4 fog = weightSum > 0.0 ? total / weightSum : texture(inputs[1], textureCoordinates);

    fragColor = vec4(colour * fog.a + fog.rgb, 1.0);
}
