#version 450
// The occlusion culler's reduction: the prepass buffer's distance channel down to one coarse grid,
// each cell holding the **farthest** distance anywhere inside its own footprint — or zero if any
// pixel of that footprint saw nothing at all.
//
// Both rules are what make the result an occluder rather than a picture of depth, and both point
// the same way: a cell may only ever under-state what it covers.
//
//   - **Farthest.** A cell whose farthest surface is 40 units away is a cell every pixel of which is
//     covered by something within 40 units, so anything behind 40 units there is hidden. Nearest
//     would claim a cell of open sky with one lamp post through it as a solid wall at the lamp post.
//   - **Zero wins outright.** The prepass clears its distance to zero and writes one only where
//     something drew, so a zero anywhere in the footprint is sky — or geometry not yet uploaded —
//     and the cell can prove nothing. That is the same convention the gather reads, stated once in
//     PrepassFragmentShader and inherited here rather than restated as a number.
//
// One pass and no mip chain. Each output texel walks its own footprint of the source, so the source
// is read exactly once in total, which is what a chain's *first* level costs on its own before any
// of the levels below it. A chain would also have to reduce with texelFetch to keep the maximum
// honest — bilinear taps average, and an average is not a maximum — so it would buy nothing here.

layout(location = 0) out vec4 fragColor;
layout(location = 0) in vec2 textureCoordinates;

// The prepass buffer: view-space normal in rgb, distance in front of the eye in a.
layout(set = SET_POST_PROCESS, binding = POST_INPUT_BINDING) uniform sampler2D inputs[POST_INPUTS];

layout(push_constant) uniform PassParameters {
    vec4 tone;   // unread here
    vec4 pass;   // x target level, y target levels, zw target size
    vec4 view;   // x tan(fovX/2), y tan(fovY/2), z near, w far
    vec4 effect; // unread here
} params;

void main()
{
    const ivec2 sourceSize = textureSize(inputs[0], 0);
    const ivec2 targetSize = ivec2(max(params.pass.zw, vec2(1.0)));

    // This cell, in the target's own texels. Off the varying rather than off gl_FragCoord, and that
    // is the contract rather than a preference: a fullscreen fragment stage that declares an input
    // it never reads has that input eliminated by spirv-opt, and the vertex stage is then writing an
    // output no fragment stage declares — which validation reports on every pipeline built from the
    // pair. The two are the same number anyway, because a fullscreen pass runs through a positive
    // viewport and the varying is the interpolated pixel centre.
    //
    // That positive viewport is also why the source's rows and this target's rows agree one for one:
    // the reduction inherits the scene pass's orientation rather than flipping it, which is what
    // lets the CPU side state the row mapping once (Graphics/Api/Occlusion.cppm) instead of
    // guessing it.
    const ivec2 cell = ivec2(textureCoordinates * vec2(targetSize));

    // The half-open footprint, taken by integer division so that the cells tile the source exactly
    // whatever the two sizes are. `max` is what keeps a cell from being empty when the target is
    // somehow larger than the source, which would leave it holding the clear rather than a distance.
    const ivec2 first = (cell * sourceSize) / targetSize;
    const ivec2 last = max(((cell + ivec2(1)) * sourceSize) / targetSize, first + ivec2(1));

    float farthest = 0.0;
    bool open = false;

    for (int y = first.y; y < last.y && !open; y++)
    {
        for (int x = first.x; x < last.x; x++)
        {
            // texelFetch and not texture(): the maximum of a set of samples is not the maximum of
            // their bilinear average, and a filtered tap here would quietly report a cell as solid
            // at a distance no surface in it actually stands at.
            const float distanceInFront = texelFetch(inputs[0], ivec2(x, y), 0).a;

            if (distanceInFront <= 0.0)
            {
                open = true;
                break;
            }

            farthest = max(farthest, distanceInFront);
        }
    }

    fragColor = vec4(open ? 0.0 : farthest, 0.0, 0.0, 1.0);
}
