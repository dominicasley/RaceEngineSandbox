#version 450
// The occlusion prepass's other half: view-space normal in rgb, distance in front of the eye in a.
//
// Distance rather than the depth buffer's own value, and stored in a float attachment rather than
// read back out of D32: the gather works in world units — a radius in metres around the pixel — and
// a hyperbolic 0..1 depth would have to be un-projected before any of that arithmetic meant
// anything. The depth attachment beside this one is still what makes the nearest surface win.
//
// Zero in a is what the clear leaves, and it is the value the gather reads as "no geometry here":
// nothing the camera can see sits at zero distance, because nothing survives the near plane.

layout(location = 0) in vec3 normalInViewSpace;
layout(location = 1) in vec3 positionInViewSpace;

layout(location = 0) out vec4 fragColor;

void main()
{
    // The view looks down negative z, so the distance in front of the eye is the negated one. Both
    // sides of the reconstruction in the gather agree on that sign, and it is the only place the
    // convention is stated.
    fragColor = vec4(normalize(normalInViewSpace), -positionInViewSpace.z);
}
