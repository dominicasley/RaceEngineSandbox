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
layout(location = 2) in vec2 textureCoordinates;

layout(set = SET_MATERIAL, binding = 0) uniform MaterialData {
    vec4 baseColour;
    vec4 roughMetal;       // x roughness, y metalness, z alpha cutoff (0 = no test)
    ivec4 useTextures;     // x diffuse, y normal, z specular, w emissive
    ivec4 useTextures2;    // x occlusion
    mat4 textureTransform; // KHR_texture_transform, upper 3x3
} material;
layout(set = SET_MATERIAL, binding = TEXTURE_DIFFUSE) uniform sampler2D diffuseTexture;

layout(location = 0) out vec4 fragColor;

void main()
{
    // A masked material's holes are holes in its occlusion too, or the gather reads every tree as
    // the quad it is drawn on and prints that quad's shadow onto whatever stands behind it.
    if (material.roughMetal.z > 0.0) {
        vec2 transformed = (mat3(material.textureTransform) * vec3(textureCoordinates, 1.0)).xy;
        // Mip 0, and it is the difference between a fence and no fence. A chain-link texture's
        // alpha averages toward its open fraction as the mips go down — under half for any mesh
        // that is mostly hole — so a coverage test against the mip the sample density picks
        // deletes the whole surface exactly where this pass samples coarsely. That is a fence
        // whose shadow lands on the car (cascade 0, mip 0) and not on the road (cascades 1-3,
        // mipped alpha under the cutoff): the least diagnosable half-presence a caster can have.
        // The aliasing mip 0 buys is in a depth map a 3x3 PCF reads; it never reaches the eye.
        float coverage = material.baseColour.a
            * ((material.useTextures.x != 0) ? textureLod(diffuseTexture, transformed, 0.0).a : 1.0);

        if (coverage < material.roughMetal.z) {
            discard;
        }
    }

    // The view looks down negative z, so the distance in front of the eye is the negated one. Both
    // sides of the reconstruction in the gather agree on that sign, and it is the only place the
    // convention is stated.
    // The back of a double-sided surface is a surface facing the camera, and the gather must be
    // told so: an unflipped normal there reads as geometry facing away, which the integral counts
    // as a fully closed horizon.
    fragColor = vec4(normalize(gl_FrontFacing ? normalInViewSpace : -normalInViewSpace), -positionInViewSpace.z);
}
