#version 450
// The bloom chain on the way down: the frame thresholded into the top level, then halved into every
// level below it. One shader for both, because a level is told which one it is — the same shape the
// exposure meter's reduction has, and for the same reason.
//
// The filter is Jimenez's thirteen taps rather than a box: four inner samples in a square at half a
// texel, four outer at a full texel, and a centre, weighted so that the kernel is the one that does
// not flicker. A box filter over a chain like this is where a single bright pixel becomes a square
// that pops in and out between frames as the camera moves, which is the artefact bloom is famous
// for and which is entirely a choice of kernel.
//
// SET_POST_PROCESS, POST_INPUT_BINDING and POST_INPUTS are defined by the renderer from
// Graphics/Api/RenderContract.cppm; this file must not spell one of those numbers.

layout(location = 0) out vec4 fragColor;
layout(location = 0) in vec2 textureCoordinates;

// The pass's single input: the camera's colour attachment at level zero, and the level above this
// one everywhere below that. The array is declared whole because the set carries it whole.
layout(set = SET_POST_PROCESS, binding = POST_INPUT_BINDING) uniform sampler2D inputs[POST_INPUTS];

layout(push_constant) uniform PassParameters {
    vec4 tone;   // x exposure, y contrast, z toe, w shoulder
    vec4 pass;   // x target level, y target levels, zw target size
    vec4 view;   // unread here
    vec4 effect; // x threshold, y knee, z the most one texel may contribute
} params;

// The thirteen-tap kernel's own weights: the four inner samples carry half the result between them,
// and the centre and the two outer rings share the rest. They sum to one.
const float innerWeight = 0.125;   // x4
const float outerWeight = 0.03125; // x8, in two overlapping squares
const float centreWeight = 0.125;

vec3 sampleAt(vec2 coordinates)
{
    return texture(inputs[0], coordinates).rgb;
}

void main()
{
    // The *source* texel, which is twice this one: every offset below is measured in the level being
    // read, not the level being written.
    const vec2 texel = 2.0 / max(params.pass.zw, vec2(1.0));

    const vec3 centre = sampleAt(textureCoordinates);

    const vec3 innerTopLeft = sampleAt(textureCoordinates + vec2(-texel.x, texel.y) * 0.5);
    const vec3 innerTopRight = sampleAt(textureCoordinates + vec2(texel.x, texel.y) * 0.5);
    const vec3 innerBottomLeft = sampleAt(textureCoordinates + vec2(-texel.x, -texel.y) * 0.5);
    const vec3 innerBottomRight = sampleAt(textureCoordinates + vec2(texel.x, -texel.y) * 0.5);

    const vec3 left = sampleAt(textureCoordinates + vec2(-texel.x, 0.0));
    const vec3 right = sampleAt(textureCoordinates + vec2(texel.x, 0.0));
    const vec3 top = sampleAt(textureCoordinates + vec2(0.0, texel.y));
    const vec3 bottom = sampleAt(textureCoordinates + vec2(0.0, -texel.y));

    const vec3 topLeft = sampleAt(textureCoordinates + vec2(-texel.x, texel.y));
    const vec3 topRight = sampleAt(textureCoordinates + vec2(texel.x, texel.y));
    const vec3 bottomLeft = sampleAt(textureCoordinates + vec2(-texel.x, -texel.y));
    const vec3 bottomRight = sampleAt(textureCoordinates + vec2(texel.x, -texel.y));

    vec3 result = centre * centreWeight +
                  (innerTopLeft + innerTopRight + innerBottomLeft + innerBottomRight) * innerWeight +
                  (left + right + top + bottom + topLeft + topRight + bottomLeft + bottomRight) * outerWeight;

    if (params.pass.x < 0.5)
    {
        // The top of the chain, and the only level that decides *what* blooms.
        //
        // Thresholded in the exposed domain — the frame times the camera's own exposure — so that a
        // camera metering itself does not change how much of the scene spills as it opens up. The
        // knee is Karis's soft curve: a hard threshold makes a highlight drifting across it pop in
        // and out at its edge, and the quadratic below fades the first `knee` of radiance in
        // instead. The maximum channel decides, not the luminance, because what blows out on a
        // display is a channel.
        const float threshold = params.effect.x;
        const float knee = max(params.effect.y, 1e-4);

        result = max(result * params.tone.x, vec3(0.0));

        const float brightest = max(result.r, max(result.g, result.b));
        const float overKnee = clamp(brightest - threshold + knee, 0.0, 2.0 * knee);
        const float contribution = max(overKnee * overKnee / (4.0 * knee), brightest - threshold);

        result *= contribution / max(brightest, 1e-4);

        // Capped before it goes down the chain. The sun's disc carries the irradiance of the scene
        // light divided by the solid angle it subtends — four orders of magnitude over the sky —
        // and this buffer is half floats: unclamped, the sum on the way down reaches infinity and
        // every level below it is white. See Bloom::maximum.
        result = min(result, vec3(params.effect.z));
    }

    fragColor = vec4(max(result, vec3(0.0)), 1.0);
}
