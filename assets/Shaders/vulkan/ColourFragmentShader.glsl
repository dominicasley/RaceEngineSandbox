#version 450
// Vulkan variant of ColourFragmentShader.glsl (only baseColour is consumed).

layout(location = 0) out vec4 fragColor;

layout(location = 0) in vec2 textureCoordinates;
layout(location = 1) in vec3 positionInWorldSpace;
layout(location = 2) in vec3 positionInViewSpace;
layout(location = 3) in vec3 normalsInNormalSpace;
layout(location = 4) in vec3 tangentInNormalSpace;
layout(location = 5) in vec3 bitangentInNormalSpace;
layout(location = 6) in vec3 normalsInWorldSpace;
layout(location = 7) in vec3 viewDirectionWorldSpace;
layout(location = 8) in vec3 lightDirectionWorldSpace;
layout(location = 9) in mat3 tangentBinormalNormalMatrix;

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
