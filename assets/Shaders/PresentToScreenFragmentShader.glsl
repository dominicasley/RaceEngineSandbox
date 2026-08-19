#version 450
// The present pass: an image the post chain has already exposed and tone mapped, put on the screen
// through a lens and a grade.
//
// This is the only pass in the frame whose input is display-referred, and that is what decides what
// belongs here. Everything before it works in radiance, where "more contrast" and "more saturation"
// are statements about light and are wrong: the tone curve's contrast pivots at middle grey in the
// exposed domain because that is where a photographic curve pivots. Here the numbers are the ones a
// grading suite has, applied to the picture rather than to the scene — and a lens defect, which is a
// property of the glass and not of anything in front of it.
//
// The grade itself is a 3D lookup table — what a colourist's tool exports — so tuning the look is
// dropping in a `.cube` file rather than editing this shader and rebuilding. What stays here is the
// lens, which is geometry rather than colour and cannot be baked into a table.
//
// LUT_BINDING and SET_POST_PROCESS are defined by the renderer from Graphics/Api/RenderContract.cppm;
// this file must not spell one of those numbers.

layout(location = 0) out vec4 fragColor;
layout(location = 0) in vec2 textureCoordinates;

// Declared whole because the fullscreen set carries it whole; this pass reads element 0 and the
// engine fills the rest with the 1x1 fallback.
layout(set = SET_POST_PROCESS, binding = POST_INPUT_BINDING) uniform sampler2D inputs[POST_INPUTS];

// The colour grade. Every fullscreen pass is written one — the identity when it has none — so this
// is read unconditionally and the neutral case costs one trilinear fetch.
layout(set = SET_POST_PROCESS, binding = LUT_BINDING) uniform sampler3D colourGrade;

layout(push_constant) uniform PassParameters {
    vec4 tone;   // neutral here: the image arrives tone mapped
    vec4 pass;   // x target level, y target levels, zw target size
    vec4 view;   // unread here
    vec4 effect; // x aberration, y how much of the grade to apply
} params;

void main ()
{
    const float aberration = params.effect.x;
    const float gradeWeight = params.effect.y;

    // Transverse chromatic aberration: a real lens does not focus every wavelength at the same
    // distance from the axis, so the three channels are sampled along the radius at slightly
    // different scales. It goes as the square of the distance from the centre — zero in the middle
    // of the frame and most visible in the corners — which is what makes it read as glass rather
    // than as a filter laid over the picture. It happens *before* the grade because that is the
    // order the physical world puts them in: the lens is in front of the film.
    const vec2 fromCentre = textureCoordinates - vec2(0.5);
    const vec2 offset = fromCentre * dot(fromCentre, fromCentre) * aberration;

    vec3 colour = clamp(vec3(texture(inputs[0], textureCoordinates + offset).r,
                             texture(inputs[0], textureCoordinates).g,
                             texture(inputs[0], textureCoordinates - offset).b),
                        0.0, 1.0);

    // The half-texel inset every 3D lookup table needs: a table of N entries has its first and last
    // *centres* at 1/2N and 1 - 1/2N, so feeding it a raw 0..1 coordinate would sample half a texel
    // outside the table at both ends and the clamp would flatten the darkest and lightest of the
    // picture. The size comes from the image rather than from a constant, so a 17-cube and a
    // 65-cube are both simply dropped in.
    const float size = float(textureSize(colourGrade, 0).x);
    const vec3 coordinate = colour * ((size - 1.0) / size) + (0.5 / size);

    fragColor = vec4(mix(colour, texture(colourGrade, coordinate).rgb, gradeWeight), 1.0);
}
