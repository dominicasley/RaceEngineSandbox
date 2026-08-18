#version 450
// Vulkan variant of ColourFragmentShader.glsl (only baseColour is consumed).

layout(location = 0) out vec4 fragColor;

// No stage inputs: this shader consumes only the material UBO. Declaring the
// PassThroughVertexShader interpolants it never reads would leave them in the SPIR-V
// only until optimization stripped them, so they are absent from the source instead.

layout(set = 1, binding = 0) uniform MaterialData {
    vec4 baseColour;
    vec4 repeatRoughMetal;
    ivec4 useTextures;
    ivec4 useTextures2;
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
