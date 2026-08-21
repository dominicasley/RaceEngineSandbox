#version 450
// Deliberately declaring no output: a cascade's VkRenderingInfo records colorAttachmentCount 0 and
// the pipeline is built with no colour blend attachment, so a location written here would have no
// attachment to be written to. What it does carry is the alpha test — a masked material's holes
// must be holes in its *shadow* too, or every tree casts the quad it is drawn on. Materials with
// no cutoff never reach the sampler, so glass still casts an opaque shadow (recorded, unchanged:
// BLEND has no single coverage a depth map could honour).
//
// SET_MATERIAL and TEXTURE_DIFFUSE are defined by the renderer from
// Graphics/Api/RenderContract.cppm; this file must not spell one of those numbers.

layout(location = 0) in vec2 textureCoordinates;

layout(set = SET_MATERIAL, binding = 0) uniform MaterialData {
    vec4 baseColour;
    vec4 roughMetal;       // x roughness, y metalness, z alpha cutoff (0 = no test)
    ivec4 useTextures;     // x diffuse, y normal, z specular, w emissive
    ivec4 useTextures2;    // x occlusion
    mat4 textureTransform; // KHR_texture_transform, upper 3x3
} material;
layout(set = SET_MATERIAL, binding = TEXTURE_DIFFUSE) uniform sampler2D diffuseTexture;

void main()
{
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
}
