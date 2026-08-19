#version 450
// Ground-truth ambient occlusion: the horizon around each pixel, gathered from the view's own
// geometry, integrated the way the reference solution integrates it.
//
// What makes it "ground truth" rather than another hemisphere-sampling heuristic is the last step.
// A slice through the pixel meets the surrounding geometry at two horizon angles; the visible arc
// between them is closed-form under a cosine-weighted normal, so the integral is *evaluated* rather
// than sampled. The sampling that remains is only in finding those two angles, which is what the
// steps below march for — and an error there moves a horizon slightly, where an error in a sampled
// integral is noise.
//
// GTAO_SLICES, GTAO_STEPS, SET_POST_PROCESS, POST_INPUT_BINDING and POST_INPUTS are defined by the
// renderer from Graphics/Api/RenderContract.cppm; this file must not spell one of those numbers.

layout(location = 0) out vec4 fragColor;
layout(location = 0) in vec2 textureCoordinates;

// One input: the prepass buffer, view-space normal in rgb and distance in front of the eye in a.
// The array is declared whole because the set carries it whole.
layout(set = SET_POST_PROCESS, binding = POST_INPUT_BINDING) uniform sampler2D inputs[POST_INPUTS];

layout(push_constant) uniform PassParameters {
    vec4 tone;   // unread here: the gather runs long before anything is tone mapped
    vec4 pass;   // x target level, y target levels, zw target size
    vec4 view;   // x tan(fovX/2), y tan(fovY/2), z near, w far
    vec4 effect; // x occlusion radius in world units, y strength
} params;

const float pi = 3.14159265359;

// How far above its own tangent plane a sample has to be before it counts as an occluder, as a sine
// of the angle. A surface seen at a grazing angle has its neighbours a long way off in world units
// and a few pixels away on screen, so the drift between where the depth says they are and where the
// plane says they should be is enough to lift them over the horizon: the surface occludes itself,
// and what that looks like on this scene's asphalt is a set of dark bands running along the ground
// at the angle the camera happens to be holding. Zero here brings them straight back.
//
// A tenth is about six degrees — far below any real crease and far above the drift.
const float surfaceBias = 0.1;

// The pixel a texture coordinate names, and the position the camera saw there. Screen space to view
// space is the projection run backwards, and the two half-angle tangents are the whole of it: the
// ndc y is flipped against the coordinate because the scene pass rasterises through a negative
// viewport height, which is where the engine's GL-side-up convention comes from.
vec3 viewPosition(vec2 coordinates, float distanceInFront)
{
    vec2 ndc = vec2(coordinates.x * 2.0 - 1.0, 1.0 - coordinates.y * 2.0);

    return vec3(ndc * params.view.xy * distanceInFront, -distanceInFront);
}

// Interleaved gradient noise: one rotation per pixel that never repeats over a 3x3 and never
// correlates with the geometry, which turns the banding of too few slices into a dither the blur
// after this pass can resolve. A function of the pixel and of nothing else, so it is the same noise
// on every machine and in every run — a frame gate compares exact images, and a jitter seeded by
// time would be the one thing in this pass that could not survive that.
float gradientNoise(vec2 pixel)
{
    return fract(52.9829189 * fract(dot(pixel, vec2(0.06711056, 0.00583715))));
}

void main()
{
    vec4 prepass = texture(inputs[0], textureCoordinates);
    float distanceInFront = prepass.a;

    // Nothing was drawn here: the sky, which is behind everything and occludes nothing. It is also
    // shaded by a pass that never reads this buffer, so the value is only what the blur beside it
    // sees at a silhouette.
    if (distanceInFront <= 0.0)
    {
        fragColor = vec4(1.0);

        return;
    }

    vec3 position = viewPosition(textureCoordinates, distanceInFront);
    vec3 normal = normalize(prepass.rgb);
    vec3 towardsEye = normalize(-position);

    // The world radius as it lands on this pixel's depth. Occlusion is a property of the geometry,
    // so the search is a fixed distance in the world and a shrinking one on screen — which is what
    // makes a corner darken by the same amount from across the street as from beside it.
    float pixelsPerUnit = 0.5 * params.pass.w / max(params.view.y * distanceInFront, 1e-4);
    float radiusInPixels = clamp(params.effect.x * pixelsPerUnit, 2.0, 0.25 * params.pass.w);
    vec2 texel = 1.0 / max(params.pass.zw, vec2(1.0));

    float noise = gradientNoise(gl_FragCoord.xy);
    float visibility = 0.0;
    // Every slice is worth what its share of the normal is, so the average is over that and not over
    // the slice count: a slice the normal barely leans into says almost nothing about this pixel,
    // and counting it as a whole opinion darkens flat ground for no reason but arithmetic.
    float sliceWeight = 0.0;

    for (int slice = 0; slice < GTAO_SLICES; slice++)
    {
        float sliceAngle = (float(slice) + noise) * pi / float(GTAO_SLICES);
        vec2 direction = vec2(cos(sliceAngle), sin(sliceAngle));

        // The slice is the plane through the eye containing this direction. Everything below happens
        // inside it, so the normal has to be projected into it first — and how much of the normal
        // survives that projection is how much this slice is worth to the average.
        vec3 sliceDirection = vec3(direction, 0.0);
        vec3 planeNormal = normalize(cross(sliceDirection, towardsEye));
        vec3 projectedNormal = normal - planeNormal * dot(normal, planeNormal);
        float projectedLength = length(projectedNormal);

        if (projectedLength < 1e-4)
        {
            continue;
        }

        vec3 unitProjected = projectedNormal / projectedLength;
        // Where the projected normal sits relative to the eye, signed inside the slice: the angle
        // the two horizons below are measured against.
        vec3 sliceTangent = cross(towardsEye, planeNormal);
        float normalAngle = sign(dot(unitProjected, sliceTangent)) * acos(clamp(dot(unitProjected, towardsEye), -1.0, 1.0));

        // One horizon each way. Cosines while they are being found, because a maximum of cosines is
        // a minimum of angles and needs no inverse trigonometry per step.
        //
        // Zero, not minus one, is what "nothing in the way" is: a horizon is measured from the view
        // direction and the furthest one that can be seen at all is the quarter turn perpendicular
        // to it. Starting at minus one would open the arc to a half turn — geometry behind the
        // camera — and the closed form below would return more than full visibility for any surface
        // not facing the eye squarely, which reads as flat ground that will not brighten.
        float cosHorizon[2] = float[2](0.0, 0.0);

        for (int side = 0; side < 2; side++)
        {
            // Side 0 is the one the slice's tangent points away from, and it is the horizon the
            // integral below takes as negative. The two have to agree: normalAngle is signed against
            // that same tangent, and marching the sides the other way round tilts every arc by the
            // angle of the surface — which is a flat plane that will not read as unoccluded.
            vec2 marchDirection = (side == 0 ? -direction : direction) * texel;

            for (int step = 0; step < GTAO_STEPS; step++)
            {
                // Jittered so that two neighbouring pixels do not march to the same places: the
                // fixed pattern is what shows up as rings around a corner.
                float distanceInPixels = (float(step) + 0.5 + noise) / float(GTAO_STEPS) * radiusInPixels;
                vec2 sampleCoordinates = textureCoordinates + marchDirection * distanceInPixels;

                if (any(lessThan(sampleCoordinates, vec2(0.0))) || any(greaterThan(sampleCoordinates, vec2(1.0))))
                {
                    break;
                }

                float sampleDistance = texture(inputs[0], sampleCoordinates).a;
                if (sampleDistance <= 0.0)
                {
                    continue;
                }

                vec3 toSample = viewPosition(sampleCoordinates, sampleDistance) - position;
                float sampleLength = length(toSample);
                if (sampleLength < 1e-4)
                {
                    continue;
                }

                // In or below this surface's own plane: not an occluder, whatever the arithmetic
                // says about it. See surfaceBias.
                if (dot(toSample / sampleLength, normal) <= surfaceBias)
                {
                    continue;
                }

                // Falls off with distance rather than stopping at the radius: a hard cutoff puts a
                // ring on the ground wherever the radius crosses a wall. Past the radius the sample
                // is ignored entirely, which is the thickness heuristic doing its other job —
                // something far behind the pixel is not occluding it, it is merely nearer the eye.
                float falloff = clamp(1.0 - sampleLength / params.effect.x, 0.0, 1.0);
                float cosAngle = dot(toSample / sampleLength, towardsEye);

                // Faded towards the open horizon rather than towards minus one, for the same reason
                // the search starts there: zero is what no occluder means.
                cosHorizon[side] = max(cosHorizon[side], mix(0.0, cosAngle, falloff));
            }
        }

        // The visible arc, clamped into the hemisphere the normal actually faces: a horizon behind
        // the surface is not a horizon, it is geometry this pixel cannot see.
        float horizon0 = normalAngle + max(-acos(clamp(cosHorizon[0], -1.0, 1.0)) - normalAngle, -0.5 * pi);
        float horizon1 = normalAngle + min(acos(clamp(cosHorizon[1], -1.0, 1.0)) - normalAngle, 0.5 * pi);

        // The closed form: the cosine-weighted integral of visibility over that arc. This is the
        // step the whole method is named for, and it is why three slices are enough to be smooth
        // where three sampled directions would be three bands.
        float arc = 0.25 * (-cos(2.0 * horizon0 - normalAngle) + cos(normalAngle) + 2.0 * horizon0 * sin(normalAngle)) +
                    0.25 * (-cos(2.0 * horizon1 - normalAngle) + cos(normalAngle) + 2.0 * horizon1 * sin(normalAngle));

        visibility += projectedLength * arc;
        sliceWeight += projectedLength;
    }

    // No slice held any of the normal — a surface exactly edge-on to every one of them, which is a
    // pixel the search had nothing to say about.
    visibility = sliceWeight > 0.0 ? clamp(visibility / sliceWeight, 0.0, 1.0) : 1.0;

    // Strength as a power rather than a multiplier: one is what the integral said, and above one
    // deepens what is already occluded while leaving everything the light reaches untouched. A
    // multiplier would darken the open ground too, which is the opposite of what a visibility term
    // is for.
    fragColor = vec4(pow(visibility, max(params.effect.y, 0.0)), 0.0, 0.0, 1.0);
}
