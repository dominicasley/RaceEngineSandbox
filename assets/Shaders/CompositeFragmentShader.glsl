#version 450
// The layered frame's join: the car buffer laid over the world buffer, in linear radiance, before
// anything blended and before the lens. The car pass rendered into a transparent buffer against the
// world's own depth, so its alpha already answers occlusion — where the world stood in front the
// car fragment failed the depth test and the pixel stayed empty — and this pass is nothing but the
// premultiplied "over" the blending hardware would have performed had the two layers shared one
// attachment.
//
// The one thing it adds is the world layer's exposure, as a ratio against the frame's. The lens is
// not per layer — one bloom, one tone curve, one grade, all applied downstream by the frame
// camera's own exposure — so a world metered by its own meter arrives here multiplied by
// worldExposure / frameExposure and leaves the tone map having seen exactly its own. With the
// meters linked the ratio is one, and one times the world is the world to the bit, which is what
// keeps the dry gates byte-identical.

layout(location = 0) out vec4 fragColor;
layout(location = 0) in vec2 textureCoordinates;

// Element 0 the world layer, element 1 the car layer. The array is declared whole because the set
// carries it whole.
layout(set = SET_POST_PROCESS, binding = POST_INPUT_BINDING) uniform sampler2D inputs[POST_INPUTS];

layout(push_constant) uniform PassParameters {
    vec4 tone;   // unread here
    vec4 pass;   // x target level, y target levels, zw target size
    vec4 view;   // unread here
    vec4 effect; // x the world layer's exposure over the frame's; 1 when the meters are linked
} params;

void main()
{
    // texelFetch, not a filtered sample: the composite is one-to-one by construction, and an exact
    // read is what makes "the split changed nothing" a provable claim rather than a filtered one.
    const ivec2 texel = ivec2(gl_FragCoord.xy);
    const vec4 world = texelFetch(inputs[0], texel, 0);
    const vec4 car = texelFetch(inputs[1], texel, 0);

    fragColor = vec4(car.rgb + world.rgb * params.effect.x * (1.0 - car.a), 1.0);
}
