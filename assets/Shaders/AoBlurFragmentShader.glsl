#version 450
// The occlusion term smoothed along surfaces and not across them.
//
// The gather takes three slices per pixel and rotates them by a per-pixel angle, which trades
// banding for dither — a good trade only if something resolves the dither afterwards. This is that
// something: a box over the neighbourhood, weighted by how much like this pixel each neighbour is.
// Plain blurring would be enough for the noise and wrong for the picture, because the one place
// occlusion changes fastest is exactly where two surfaces meet, and an edge blurred across leaves a
// building's shadow leaking onto the ground in front of it.
//
// SET_POST_PROCESS, POST_INPUT_BINDING and POST_INPUTS are defined by the renderer from
// Graphics/Api/RenderContract.cppm; this file must not spell one of those numbers.

layout(location = 0) out vec4 fragColor;
layout(location = 0) in vec2 textureCoordinates;

// Two inputs, in the order the pass declared them: the gathered occlusion, and the prepass buffer
// that says which neighbours belong to the same surface.
layout(set = SET_POST_PROCESS, binding = POST_INPUT_BINDING) uniform sampler2D inputs[POST_INPUTS];

layout(push_constant) uniform PassParameters {
    vec4 tone;
    vec4 pass;   // x target level, y target levels, zw target size
    vec4 view;
    vec4 effect;
} params;

// How far the box reaches. Two either way covers the five pixels the gather's rotation repeats
// over, which is what makes the dither average out rather than survive at a longer wavelength.
const int radius = 2;

void main()
{
    vec2 texel = 1.0 / max(params.pass.zw, vec2(1.0));

    vec4 centre = texture(inputs[1], textureCoordinates);
    float centreDistance = centre.a;
    vec3 centreNormal = normalize(centre.rgb);

    // Nothing was drawn here, so there is nothing to smooth and nothing to smooth it with.
    if (centreDistance <= 0.0)
    {
        fragColor = vec4(1.0);

        return;
    }

    float total = 0.0;
    float weightSum = 0.0;

    for (int y = -radius; y <= radius; y++)
    {
        for (int x = -radius; x <= radius; x++)
        {
            vec2 sampleCoordinates = textureCoordinates + vec2(x, y) * texel;
            vec4 neighbour = texture(inputs[1], sampleCoordinates);

            if (neighbour.a <= 0.0)
            {
                continue;
            }

            // Two ways of being a different surface, and both have to be tested: a neighbour can be
            // at the same distance on the other side of a crease, or facing the same way from much
            // further off. The depth tolerance is relative to the distance because a pixel a hundred
            // units away covers far more world than one at arm's length.
            float depthWeight = max(0.0, 1.0 - abs(neighbour.a - centreDistance) / (0.02 * centreDistance));
            float normalWeight = max(0.0, dot(normalize(neighbour.rgb), centreNormal));
            float weight = depthWeight * normalWeight * normalWeight;

            total += texture(inputs[0], sampleCoordinates).r * weight;
            weightSum += weight;
        }
    }

    // The centre always qualifies as its own neighbour, so a zero sum means every other pixel was
    // rejected — a one-pixel sliver — and the unsmoothed value is the honest answer there.
    fragColor = vec4(weightSum > 0.0 ? total / weightSum : texture(inputs[0], textureCoordinates).r, 0.0, 0.0, 1.0);
}
