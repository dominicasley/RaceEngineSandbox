#version 450
// Vulkan variant of ColourFragmentShader.glsl (only baseColour is consumed).

layout(location = 0) out vec4 fragColor;

// No stage inputs: this shader consumes only the material UBO. Declaring the
// PassThroughVertexShader interpolants it never reads would leave them in the SPIR-V
// only until optimization stripped them, so they are absent from the source instead.

// Only baseColour is read; the rest of the block is declared so the offsets match the ABI.
layout(set = SET_MATERIAL, binding = 0) uniform MaterialData {
    vec4 baseColour;
    vec4 roughMetal;
    ivec4 useTextures;
    ivec4 useTextures2;
    mat4 textureTransform;
} material;

void main()
{
    vec4 albedo = material.baseColour;

    if (albedo.a < 0.01)
    {
        discard;
    }

    fragColor = albedo;
}
