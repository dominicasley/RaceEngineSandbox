#version 450
// **The eye at night**, which is the one thing a physically correct night sky cannot supply.
//
// Scattering is linear in its source, so a moonlit world and a dawn have exactly the same ratios
// inside them — sky to ground, disc to sky, fog to light. A meter then removes the only thing that
// told them apart, which is the absolute level, and what arrives at the tone curve is a dawn. That
// is not a defect in the sky; it is why "day for night" works in film, and no amount of further work
// on the atmosphere can undo it.
//
// What separates the two is the observer. **Below about 5 cd/m² the cones stop carrying the image
// and the rods take it over**, and rods are one receptor, not three: there is no colour in a rod
// image, its spectral sensitivity peaks 48 nm bluer than the cones', and a scene painted in it is
// desaturated and re-weighted rather than merely darker. That re-weighting is the Purkinje shift,
// and it is the whole reason a red tail light is the first thing to vanish at dusk while a blue sign
// stays legible.
//
// So this pass converts, per pixel, by how far into the rods that pixel actually is. It runs on
// scene-referred radiance before bloom and long before the tone curve, because the threshold it
// tests is an absolute luminance and nothing downstream still knows one.

layout(location = 0) out vec4 fragColor;
layout(location = 0) in vec2 textureCoordinates;

// Element 0 is the composited frame. The array is declared whole because the set carries it whole.
layout(set = SET_POST_PROCESS, binding = POST_INPUT_BINDING) uniform sampler2D inputs[POST_INPUTS];

layout(push_constant) uniform PassParameters {
    vec4 tone;      // unread here: this pass works in radiance, ahead of the tone map
    vec4 pass;      // x target level, y target levels, zw target size
    vec4 view;      // unread
    vec4 effect;    // x cd/m² per engine radiance unit, y strength 0..1, zw reserved
} params;

// **The mesopic range, CIE 191:2010.** Above the upper bound the image is the cones' alone and this
// pass is the identity; below the lower bound it is the rods' alone. Between them both receptors
// contribute, which is where almost every night scene that is not pitch dark actually sits. These
// are a standard's numbers and not a look, which is why they are spelled here and not passed in.
const float photopicFloor = 5.0;     // cd/m²
const float scotopicCeiling = 0.005; // cd/m²

// **The rods' response, from Kirk and O'Brien's fit in CIE XYZ.** A rod does not see R, G and B; it
// sees one quantity, and this is the published statement of that quantity in terms of a colour the
// renderer already has. Its output is in scotopic units, which stand at a different scale from
// photopic luminance, so it is normalised by what it answers for a neutral — worked below — and what
// survives the normalisation is exactly the part that matters: the shift, not the level.
//
// For a neutral grey, XYZ is v·(0.9505, 1.0, 1.0890), so the bracket is
// 1.33·(1 + 2.0932) − 1.68 = 2.5729. That is the divisor, and it is a derivation rather than a fit:
// with it a grey keeps its brightness exactly, pure blue comes out 2.8 times brighter than its
// photopic luminance and pure red 0.15 times — a red 2.7 stops down, which is the Purkinje shift
// stated in numbers.
const float scotopicNeutral = 2.5729;

float scotopicLuminance(vec3 rgb)
{
    const vec3 xyz = vec3(dot(rgb, vec3(0.4124, 0.3576, 0.1805)),
                          dot(rgb, vec3(0.2126, 0.7152, 0.0722)),
                          dot(rgb, vec3(0.0193, 0.1192, 0.9505)));

    // X floors rather than divides by zero: a colour with no long-wavelength content at all is not
    // a colour any surface in this engine returns, and the floor is far below the smallest one that
    // is. Clamped at zero because the fit is a fit and goes slightly negative off the spectrum.
    const float v = xyz.y * (1.33 * (1.0 + (xyz.y + xyz.z) / max(xyz.x, 1e-5)) - 1.68);

    return max(v, 0.0) / scotopicNeutral;
}

// **What a rod image looks like, which is the one convention in this file.** There is no physical
// colour to a single-receptor signal — the percept is reported as a desaturated blue-grey, and
// cinema has painted night blue for a century on the strength of it. Kept deliberately gentle,
// because the Purkinje shift above already carries most of the blue *by derivation* and this would
// otherwise count it twice. Normalised to unit Rec.709 luminance, so it moves the hue and never the
// level the rods just decided.
const vec3 rodTint = vec3(0.944, 0.996, 1.206);

void main()
{
    const vec4 source = texture(inputs[0], textureCoordinates);

    // Where this pixel stands, in cd/m². The scale comes from the rig, anchored on the one night
    // measurement anybody publishes — a full moon delivers 0.25 lux — so this is a real luminance
    // and not a number that happens to have the right size.
    const float luminance = max(dot(source.rgb, vec3(0.2126, 0.7152, 0.0722)), 0.0);
    const float candelas = luminance * params.effect.x;

    // How far into the rods, over the mesopic decades. In the log, because the range is three of
    // them and a linear ramp would spend almost all of itself in the top tenth of a stop.
    const float rods = 1.0 - smoothstep(log2(scotopicCeiling), log2(photopicFloor), log2(max(candelas, 1e-12)));

    const vec3 rodImage = scotopicLuminance(source.rgb) * rodTint;

    // The alpha is carried untouched: the frame's coverage is what the meter weights by and what the
    // composite masks with, and neither is a question about the eye.
    fragColor = vec4(mix(source.rgb, rodImage, rods * params.effect.y), source.a);
}
