#version 450
// The bloom chain on the way back up: the level below, spread with a nine-tap tent, plus the
// matching level of the downsample chain.
//
// Two chains and an addition rather than one chain and additive blending. The usual formulation
// blends each upsample into the level it is reading from, which is a pass sampling the image it is
// rendering into — the one thing the per-level layout tracking cannot express, because a level is in
// one layout at a time. Reading two inputs and writing a third says the same thing, and the
// multi-input fullscreen pass already existed for it.
//
// A tent rather than a box because the sum of tents across the chain is what makes the spill fall
// off smoothly instead of in visible steps at each level's footprint.
//
// SET_POST_PROCESS, POST_INPUT_BINDING and POST_INPUTS are defined by the renderer from
// Graphics/Api/RenderContract.cppm; this file must not spell one of those numbers.

layout(location = 0) out vec4 fragColor;
layout(location = 0) in vec2 textureCoordinates;

// Two inputs, in the order the pass declared them: the level below in the chain being built, and
// the matching level of the chain it is being built from.
layout(set = SET_POST_PROCESS, binding = POST_INPUT_BINDING) uniform sampler2D inputs[POST_INPUTS];

layout(push_constant) uniform PassParameters {
    vec4 tone;
    vec4 pass;   // x target level, y target levels, zw target size
    vec4 view;
    vec4 effect; // x spread, in texels of the level being read
} params;

void main()
{
    // Measured in this level's own texels: the level below is half the size, so a tap a texel wide
    // here is half a texel there, which is what keeps the tent the same width in the picture at
    // every level of the chain.
    const vec2 texel = params.effect.x / max(params.pass.zw, vec2(1.0));

    const vec3 centre = texture(inputs[0], textureCoordinates).rgb;

    const vec3 left = texture(inputs[0], textureCoordinates + vec2(-texel.x, 0.0)).rgb;
    const vec3 right = texture(inputs[0], textureCoordinates + vec2(texel.x, 0.0)).rgb;
    const vec3 top = texture(inputs[0], textureCoordinates + vec2(0.0, texel.y)).rgb;
    const vec3 bottom = texture(inputs[0], textureCoordinates + vec2(0.0, -texel.y)).rgb;

    const vec3 topLeft = texture(inputs[0], textureCoordinates + vec2(-texel.x, texel.y)).rgb;
    const vec3 topRight = texture(inputs[0], textureCoordinates + vec2(texel.x, texel.y)).rgb;
    const vec3 bottomLeft = texture(inputs[0], textureCoordinates + vec2(-texel.x, -texel.y)).rgb;
    const vec3 bottomRight = texture(inputs[0], textureCoordinates + vec2(texel.x, -texel.y)).rgb;

    // 1 2 1 / 2 4 2 / 1 2 1, over sixteen.
    const vec3 spread = (centre * 4.0 + (left + right + top + bottom) * 2.0 +
                         (topLeft + topRight + bottomLeft + bottomRight)) /
                        16.0;

    fragColor = vec4(spread + texture(inputs[1], textureCoordinates).rgb, 1.0);
}
