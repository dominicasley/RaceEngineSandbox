#version 450
// The half-resolution occlusion term lifted back to the view's own size.
//
// The gather and its blur run at half resolution because occlusion is low-frequency and the
// horizon search is the frame's most expensive fullscreen pass. What must not come back up with
// it is a depth edge averaged across: a plain bilinear read blends the four half-resolution
// texels under a pixel whatever they belong to, and a building's occlusion then leaks onto the
// sky beside it. So each of the four is weighted by its bilinear share *and* by how much its
// geometry — read from the full-resolution prepass at that texel's own centre — looks like this
// pixel's, which is the same similarity test the blur uses on its neighbours.
//
// SET_POST_PROCESS, POST_INPUT_BINDING and POST_INPUTS are defined by the renderer from
// Graphics/Api/RenderContract.cppm; this file must not spell one of those numbers.

layout(location = 0) out vec4 fragColor;
layout(location = 0) in vec2 textureCoordinates;

// Two inputs, in the order the pass declared them: the blurred half-resolution occlusion, and the
// full-resolution prepass that says which of the four texels under this pixel share its surface.
layout(set = SET_POST_PROCESS, binding = POST_INPUT_BINDING) uniform sampler2D inputs[POST_INPUTS];

layout(push_constant) uniform PassParameters {
    vec4 tone;
    vec4 pass;   // x target level, y target levels, zw target size
    vec4 view;
    vec4 effect;
} params;

void main()
{
    vec4 centre = texture(inputs[1], textureCoordinates);

    // Nothing was drawn here, so there is nothing to lift.
    if (centre.a <= 0.0)
    {
        fragColor = vec4(1.0);

        return;
    }

    vec3 centreNormal = normalize(centre.rgb);

    // The four source texels this pixel sits between, addressed in the source's own grid — the
    // target size in params.pass.zw is the full view and says nothing about the source.
    vec2 sourceSize = vec2(textureSize(inputs[0], 0));
    vec2 sourcePosition = textureCoordinates * sourceSize - 0.5;
    vec2 base = floor(sourcePosition);
    vec2 fraction = sourcePosition - base;

    float total = 0.0;
    float weightSum = 0.0;

    for (int y = 0; y <= 1; y++)
    {
        for (int x = 0; x <= 1; x++)
        {
            vec2 texelCentre = (base + vec2(x, y) + 0.5) / sourceSize;
            vec4 neighbour = texture(inputs[1], texelCentre);

            if (neighbour.a <= 0.0)
            {
                continue;
            }

            // The same two tests the blur applies, for the same reason: same distance across a
            // crease and same facing from further off are both different surfaces.
            float bilinear = (x == 1 ? fraction.x : 1.0 - fraction.x) * (y == 1 ? fraction.y : 1.0 - fraction.y);
            float depthWeight = max(0.0, 1.0 - abs(neighbour.a - centre.a) / (0.02 * centre.a));
            float normalWeight = max(0.0, dot(normalize(neighbour.rgb), centreNormal));
            float weight = bilinear * depthWeight * normalWeight * normalWeight;

            total += texture(inputs[0], texelCentre).r * weight;
            weightSum += weight;
        }
    }

    // Every candidate rejected means this pixel's surface fell between the half-resolution
    // samples — a sliver — and the plain bilinear read is the honest answer there.
    fragColor = vec4(weightSum > 0.0 ? total / weightSum : texture(inputs[0], textureCoordinates).r, 0.0, 0.0, 1.0);
}
