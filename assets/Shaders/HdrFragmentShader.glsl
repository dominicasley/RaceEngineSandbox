#version 450
// The tone mapping stage: exposed scene radiance to a display value.

layout(location = 0) out vec4 fragColor;
layout(location = 0) in vec2 textureCoordinates;

// Every input the pass declared, in the order it declared them: the scene colour, the camera's
// depth attachment beside it (unread here, for the passes that want it), and the top of the bloom
// chain. The array is declared whole because the set carries it whole — the engine writes every
// element whatever this reads.
layout(set = SET_POST_PROCESS, binding = POST_INPUT_BINDING) uniform sampler2D inputs[POST_INPUTS];

// The camera's exposure and the shape of the curve that follows it, pushed by the renderer for
// every fullscreen pass. The scene is rendered in relative radiance — a sun of 3.2 is a number the
// level chose, not a measurement — so something has to say what that maps to, and something has to
// say what is done with it once it is said. See ToneCurve, and evaluateToneCurve, which is this
// function written again in C++.
layout(push_constant) uniform PassParameters {
    vec4 tone;   // x exposure, y contrast, z toe, w shoulder
    vec4 pass;   // x target level, y target levels, zw target size
    vec4 view;   // unread here
    vec4 effect; // x bloom intensity
} params;

// Narkowicz's fit of the ACES reference curve. Display-referred on the way out — the sRGB transfer
// is folded into the fit — which is why nothing encodes after it.
vec3 acesFilm(const vec3 x) {
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d ) + e), 0.0, 1.0);
}

void main ()
{
    const float exposure = params.tone.x;
    const float contrast = params.tone.y;
    const float toe = params.tone.z;
    const float shoulder = params.tone.w;

    // Exposure first, then the spill, then the curve. The bloom chain was thresholded in the
    // exposed domain and holds exposed radiance already, so it is added rather than exposed again —
    // which is what keeps the amount of spill fixed while the meter moves the shutter under it.
    vec3 exposed = max(texture(inputs[0], textureCoordinates).rgb * exposure, vec3(0.0));
    exposed += max(texture(inputs[2], textureCoordinates).rgb, vec3(0.0)) * params.effect.x;

    // Contrast is a power law about middle grey in the exposed domain, which is a straight line in
    // log-log: it moves nothing at the pivot and moves everything else symmetrically about it. The
    // floor keeps pow() off zero, where a contrast below one has an infinite derivative.
    vec3 shaped = TONE_GREY_PIVOT * pow(max(exposed / TONE_GREY_PIVOT, vec3(1e-6)), vec3(contrast));

    vec3 mapped = acesFilm(shaped);

    // Toe and shoulder are one-sided gammas: the exponent runs from 1 + toe at black to 1 at white,
    // and from 1 at black to 1 + shoulder at white. Both leave 0 and 1 where they are and both only
    // darken, so raising either deepens one end of the range instead of shifting the picture.
    mapped = pow(mapped, vec3(1.0) + toe * (vec3(1.0) - mapped));
    mapped = pow(mapped, vec3(1.0) + shoulder * mapped);

    fragColor = vec4(clamp(mapped, 0.0, 1.0), 1.0);
}
