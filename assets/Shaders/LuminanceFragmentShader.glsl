#version 450
// The exposure meter's reduction: a frame of scene radiance down to one texel holding the weighted
// average of its log luminance, and the weight it was averaged with. One shader for every level of
// the chain, because a level is told which one it is.
//
// Two channels rather than one, and that is the whole of what centre weighting costs. The top of the
// chain writes `weight * log2(luminance)` beside `weight`; every level below averages both; the
// ratio at the end is the weighted mean, because a mean of sums over a mean of the same weights is
// the weighted mean whatever tree of averages sat under it. A weight of one everywhere recovers the
// flat average exactly, which is what `centreWeighting = 0` is.

layout(location = 0) out vec4 fragColor;
layout(location = 0) in vec2 textureCoordinates;

// The pass's single input: the camera's colour attachment at level zero, and the level above this
// one everywhere below that. The array is declared whole because the set carries it whole.
layout(set = SET_POST_PROCESS, binding = POST_INPUT_BINDING) uniform sampler2D inputs[POST_INPUTS];

// The fullscreen block, declared as the layout carries it. `pass` says which level this is and
// `effect` how much the middle of the frame is worth; the meter runs before the tone map and has no
// use for the curve it is going to feed, but the block is the layout's and the member offsets are
// part of it, so it is declared as it is rather than trimmed to what this stage happens to want.
layout(push_constant) uniform PassParameters {
    vec4 tone;   // unread here
    vec4 pass;   // x target level, y target levels, zw target size
    vec4 view;   // x tan(fovX/2), y tan(fovY/2), z near, w far
    vec4 effect; // x centre weighting, 0 flat and 1 strongly centred; y coverage weighting,
                 // 0 every pixel counts and 1 a pixel counts by its alpha
} params;

// Where the centre stops and the falloff begins, as a fraction of the half-diagonal, and what the
// very corner is worth at full weighting. A quarter and a tenth is about what a camera's
// centre-weighted average meter does: the middle of the frame decides and the edges are consulted.
const float centreRadius = 0.25;
const float cornerWeight = 0.1;

void main()
{
    // The target level's texel, which is the only thing that says how far apart the taps below go.
    // A reduction that assumed a resolution would be a reduction that stopped working the moment
    // the chain was built at another one.
    const vec2 texel = 1.0 / max(params.pass.zw, vec2(1.0));

    if (params.pass.x < 0.5)
    {
        // The top of the chain. Scene radiance in, its logarithm out: the levels below take a plain
        // mean, so metering in the log domain makes the whole reduction a geometric mean — the
        // average an exposure meter takes, and the one a handful of specular pinpricks cannot drag
        // the picture off. The floor is what keeps a black pixel from being minus infinity, and
        // C++ floors at the same place so both sides meter a black frame as the same number.
        const vec4 sampled = texture(inputs[0], textureCoordinates);
        const float luminance = dot(max(sampled.rgb, vec3(0.0)), vec3(LUMINANCE_R, LUMINANCE_G, LUMINANCE_B));

        // Distance from the centre of the *frame*, not of this square buffer: the reduction target
        // is 512 on both sides and the frame is not, so a radius measured in this buffer's own
        // coordinates would be an ellipse on screen. The two half-angle tangents are the view's
        // aspect, which is exactly the correction — and they are already in the push constant
        // because a different pass needed them.
        const float aspect = params.view.y > 0.0 ? params.view.x / params.view.y : 1.0;
        const vec2 fromCentre = (textureCoordinates - vec2(0.5)) * vec2(aspect, 1.0);
        const float radius = length(fromCentre) / max(length(vec2(aspect, 1.0)) * 0.5, 1e-4);

        // One at the centre, `cornerWeight` at the corner, smooth in between — and mixed by how much
        // weighting was asked for, so zero is the flat average this used to be.
        const float centred = mix(1.0, cornerWeight, smoothstep(centreRadius, 1.0, radius));

        // Coverage weighting, for a meter over one layer's own buffer: where nothing drew, the
        // alpha is zero and the pixel must not meter as black — a cabin's reading would otherwise
        // be dragged down by however much of the frame the world outside occupies. At zero this
        // multiplies by exactly one, which is every meter before the layered frame existed, and a
        // frame with no coverage at all sums to zero weight, which the readback holds the previous
        // reading through.
        const float weight = mix(1.0, centred, clamp(params.effect.x, 0.0, 1.0)) *
                             mix(1.0, clamp(sampled.a, 0.0, 1.0), clamp(params.effect.y, 0.0, 1.0));

        fragColor = vec4(weight * log2(max(luminance, LUMINANCE_FLOOR)), weight, 0.0, 1.0);

        return;
    }

    // Four taps at a quarter of this level's texel, which lands each of them on the centre of one
    // texel of the level above when the halving is exact and spreads them across the level's
    // footprint when it is not. Bilinear filtering makes each tap an average in its own right, so
    // an odd level loses no row rather than dropping one. Both channels ride together: the weighted
    // logarithm and the weight are averaged by the same tree, which is what keeps their ratio the
    // weighted mean.
    const vec2 offset = 0.25 * texel;
    const vec2 sum = textureLod(inputs[0], textureCoordinates + vec2(-offset.x, -offset.y), 0.0).rg +
                     textureLod(inputs[0], textureCoordinates + vec2(offset.x, -offset.y), 0.0).rg +
                     textureLod(inputs[0], textureCoordinates + vec2(-offset.x, offset.y), 0.0).rg +
                     textureLod(inputs[0], textureCoordinates + vec2(offset.x, offset.y), 0.0).rg;

    fragColor = vec4(sum * 0.25, 0.0, 1.0);
}
