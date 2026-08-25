#version 450
// Falling rain, drawn over the composited frame and under everything blended.
//
// Where this pass sits is most of what makes it correct (docs/world-rain-brief.md): it rides the
// car camera's chain immediately after the composite, so it reads a frame that already holds the
// world *and* the car's opaque geometry, and the frame camera draws every blended surface — the
// windscreen first among them — over its output afterwards. The rain is therefore behind the glass,
// behind its grime and behind its drops by construction, which is the whole objection that killed
// the naive post-pass design, dissolved by the layered frame rather than worked around.
//
// The model is world-space vertical streaks, not a scrolling screen texture. Each of a handful of
// depth layers samples the pixel's view ray at a fixed view depth, finds the drop column of the
// world-space cell it landed in, and asks two questions: is the ray close enough to that vertical
// line, and is a drop passing this height right now. Anchoring the columns in world XZ is what
// makes camera translation move the rain correctly — a screen-anchored layer reads as rain painted
// on the lens the moment the car moves. Occlusion is one comparison per layer against the
// occlusion prepass, whose alpha already carries the view depth of the nearest opaque surface of
// **every** layer (the prepass mask is forced wide for the ambient occlusion seam, and this pass
// inherits that decision for free): a streak deeper than the surface at this pixel is behind it,
// and a prepass alpha of zero is the sky, which occludes nothing.
//
// Determinism is the house rule: the clock is the simulated instant the push constant carries —
// ticks through the fixed step, never a wall reading — and every random number is an integer hash
// of a cell and a cycle index, so a captured frame N is the same image on every machine.

layout(location = 0) out vec4 fragColor;
layout(location = 0) in vec2 textureCoordinates;

// Element 0 the composited frame, element 1 the occlusion prepass (view-space normal in rgb, view
// depth in a). The array is declared whole because the set carries it whole.
layout(set = SET_POST_PROCESS, binding = POST_INPUT_BINDING) uniform sampler2D inputs[POST_INPUTS];

layout(push_constant) uniform PassParameters {
    vec4 tone;      // unread here: this pass works in radiance, long before the tone map
    vec4 pass;      // x target level, y target levels, zw target size
    vec4 view;      // x tan(fovX/2), y tan(fovY/2), z near, w far
    vec4 effect;    // x density, y streak length, z brightness, w fall speed — multipliers, 0 = 1
    vec4 viewRight; // xyz the view's +x in world space, w the camera's world x
    vec4 viewUp;    // xyz the view's +y in world space, w the camera's world y
    vec4 viewBack;  // xyz the view's +z in world space (the view looks down -z), w the world z
    vec4 weather;   // x the simulated instant in seconds, y the scene's rain intensity,
                    // zw the camera's world velocity, x and z, in world units per second
} params;

// How fast a drop falls, in world units per second — a large drop's real 9.5 m/s. It went to 55
// after the first seat report ("falling very fast": at 95 the then-nearest layer, two metres out,
// strobed rather than fell) and came back up in Dominic's own tuning session once the nearest
// layer moved to 4.5 m and the field densified — at that distance the real speed reads as rain
// again. The constants in this file are the seat's; the sequence is in docs/world-rain-brief.md.
// What this feeds is the streak's motion and its length, so the rain falls at the same speed
// whatever the frame rate; `effect.w` scales it for a session without touching this file.
const float fallSpeed = 95.0;

// The five slabs of air the rain is sampled in, as view depths in world units: 2 m to 48 m,
// roughly doubling. Nearer than the first is inside the cabin, which has a roof; past the last a
// streak is thinner than a pixel and the layer would be paying for haze.
const int layerCount = 5;
const float layerDepth[layerCount] = float[](45.0, 85.0, 100.0, 220.0, 480.0);

// The same integer hash the wet-surface mask uses, the house rule for anything procedural.
float rainHashCell(ivec3 cell)
{
    uint hashed = uint(cell.x) * 374761393u + uint(cell.y) * 668265263u + uint(cell.z) * 2246822519u;
    hashed = (hashed ^ (hashed >> 13u)) * 1274126177u;
    hashed = hashed ^ (hashed >> 16u);

    return float(hashed & 0x00FFFFFFu) / 16777215.0;
}

void main()
{
    vec3 background = texture(inputs[0], textureCoordinates).rgb;

    float intensity = clamp(params.weather.y, 0.0, 1.0);
    if (intensity <= 0.0)
    {
        fragColor = vec4(background, 1.0);

        return;
    }

    // The pixel's ray, reconstructed exactly as the occlusion gather reconstructs it — same ndc
    // flip, same half-angle tangents — then carried to world space by the view basis the push
    // constant states. dirWorld is scaled so that marching it by a *view depth* lands on the world
    // position the camera saw at that depth, which is also the parametrisation the prepass's alpha
    // is stated in, so the occlusion test below is one comparison with no conversion.
    vec2 ndc = vec2(textureCoordinates.x * 2.0 - 1.0, 1.0 - textureCoordinates.y * 2.0);
    vec3 dirView = vec3(ndc * params.view.xy, -1.0);
    vec3 dirWorld = params.viewRight.xyz * dirView.x + params.viewUp.xyz * dirView.y + params.viewBack.xyz * dirView.z;
    vec3 cameraPosition = vec3(params.viewRight.w, params.viewUp.w, params.viewBack.w);

    // The nearest opaque surface on this ray, in view depth. Zero is "the prepass saw nothing
    // here" — the sky — which occludes nothing and must not read as a surface at the eye.
    float surfaceDepth = texture(inputs[1], textureCoordinates).a;
    if (surfaceDepth <= 0.0)
    {
        surfaceDepth = 1.0e8;
    }

    // How wide one pixel is in world units per unit of view depth, for the clamp below: a streak
    // is a couple of centimetres of water, which at twenty metres is far under the sampling rate,
    // and detail below the pixel is invisible however carefully it is placed.
    float pixelPerDepth = 2.0 * params.view.y / max(params.pass.w, 1.0);

    float time = params.weather.x;
    float density = (0.30 + 0.55 * intensity) * clamp(params.effect.x, 0.0, 4.0);
    float fall = fallSpeed * (params.effect.w <= 0.0 ? 1.0 : clamp(params.effect.w, 0.25, 4.0));

    // The streaks' tilt. A streak is the drop's fall blurred over a shutter, and a moving camera
    // smears it along the *apparent* fall — the drop's velocity minus the camera's — so at speed
    // the rain leans towards the car and at rest it hangs vertical. Shearing the sampling space by
    // camera-velocity-over-fall-speed turns every slanted track back into a vertical line, so the
    // whole untouched model below runs in sheared coordinates and tilts for free. The camera's
    // vertical motion is deliberately dropped (the push constant carries x and z), and the shear
    // is capped so a very fast car keeps streaks rather than a horizontal wash.
    vec2 shear = clamp(vec2(params.weather.z, params.weather.w) / fall, vec2(-3.0), vec2(3.0));

    vec3 colour = background;

    // Far to near, each layer composited over the last, which is the order "over" needs.
    for (int layer = layerCount - 1; layer >= 0; layer--)
    {
        float depth = layerDepth[layer];
        if (depth >= surfaceDepth)
        {
            continue;
        }

        vec3 P = cameraPosition + dirWorld * depth;

        // The sheared plane the vertical-line model runs in; P.y stays true for the fall phase, so
        // a drop's height and its tilt agree by construction.
        vec2 sheared = P.xz - P.y * shear;

        // Cells sized with the layer's depth, so every layer's columns stand a similar angle apart
        // on screen and no layer is either empty or a wall of water. The first cut used 0.20 and
        // put roughly seven streaks on the whole screen; rain that sparse reads as dust.
        float cellSize = depth * 0.06;
        vec2 cell = floor(sheared / cellSize);
        ivec2 cellIndex = ivec2(cell);

        // Where along the fall a drop is at this height and this instant, asked before the drop's
        // position because the position depends on the answer. `u` advances with time at the fall
        // speed, so the pattern slides down the column; the integer part is the fall cycle, and
        // the cycle re-rolls both whether this cell rains and *where in the cell* the drop falls.
        // Two and a half cells of height between successive drops in a column: high against real
        // rain — which runs centimetres apart and would be a wall of water drawn one drop per
        // column — and what puts tens of live streaks on screen per layer.
        float period = cellSize * 1.2;
        float phase = rainHashCell(ivec3(cellIndex, layer * 4 + 3));
        float u = (P.y + fall * time) / period + phase;
        float cycle = floor(u);
        float along = u - cycle;

        if (rainHashCell(ivec3(cellIndex, int(cycle) * 8 + layer)) > density)
        {
            continue;
        }

        // The drop's own line, jittered inside its cell **per fall cycle, not per cell** — a new
        // drop is a new drop and lands somewhere new. Keyed by the cell alone this was Dominic's
        // report "the same drops appear in the same place unless I move the camera": one fixed
        // column per cell replays identical streaks at identical screen positions forever from a
        // standing camera. The 32/4 packing keeps every (cycle, layer) pair a distinct hash input.
        int cycleKey = int(cycle) * 32 + layer * 4;
        float jitterU = rainHashCell(ivec3(cellIndex, cycleKey + 1));
        float jitterV = rainHashCell(ivec3(cellIndex, cycleKey + 2));
        vec2 column = (cell + vec2(0.15) + 0.70 * vec2(jitterU, jitterV)) * cellSize;

        float across = length(sheared - column);

        // As wide as a real streak or as wide as a couple of pixels, whichever is more: the clamp
        // is what keeps the drop visible at every layer instead of aliasing away. A centimetre,
        // up from half — the seat asked for slightly bigger, and a bigger drop is also what makes
        // the refraction below readable rather than a one-pixel shimmer.
        float width = max(0.10, pixelPerDepth * depth * 1.8);
        float coverage = 1.0 - smoothstep(width * 0.35, width, across);
        if (coverage <= 0.0)
        {
            continue;
        }

        // The streak: the drop's fall blurred over roughly a shutter's worth of travel, soft at the
        // head and tail. The factor rose with the fall-speed correction so the streaks kept their
        // length — the seat report was about the motion, not the shape.
        float streakLength = fall * 0.075 * clamp(params.effect.y <= 0.0 ? 1.0 : params.effect.y, 0.25, 4.0);
        float share = streakLength / period;
        float body = smoothstep(0.0, share * 0.25, along) * (1.0 - smoothstep(share * 0.75, share, along));
        if (body <= 0.0)
        {
            continue;
        }

        // A drop refracts the world rather than tinting its own pixel — the same statement the
        // windscreen's droplets make against sceneBehind, one buffer later. A falling streak is
        // close to a vertical cylinder of water, and a cylinder lens mirrors and compresses what
        // stands behind it: the sample runs to the far side of the streak's axis and several
        // widths out, so the drop carries a squeezed, flipped strip of the world beside it, plus a
        // small pull toward the sky, which is what fills a real drop. The lift and desaturation on
        // top are the drop's own scatter. textureLod rather than texture, because this read sits
        // under divergent control flow, where an implicit-derivative sample is undefined.
        vec2 axis = normalize(params.viewRight.xz);
        float side = clamp(dot(sheared - column, axis) / width, -1.0, 1.0);
        float widthU = width / (depth * 2.0 * params.view.x);
        vec2 refractedUv =
            clamp(textureCoordinates + vec2(-side * widthU * 6.0, -widthU * 3.0), vec2(0.0), vec2(1.0));
        vec3 seen = textureLod(inputs[0], refractedUv, 0.0).rgb;

        float brightness = params.effect.z <= 0.0 ? 1.0 : params.effect.z;
        float alpha = coverage * body * 0.60 * exp(-depth * 0.002);
        float luminance = dot(seen, vec3(0.2126, 0.7152, 0.0722));
        vec3 streak = (seen * 0.85 + vec3(luminance) * 0.45) * brightness;

        colour = mix(colour, streak, clamp(alpha, 0.0, 1.0));
    }

    fragColor = vec4(colour, 1.0);
}
