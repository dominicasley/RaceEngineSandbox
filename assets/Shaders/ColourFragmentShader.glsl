#version 420 core

layout(location = 0) out vec4 fragColor;

in vec3 positionInViewSpace;
in vec3 positionInWorldSpace;
in vec2 textureCoordinates;
in vec3 viewDirectionWorldSpace;
in vec3 lightDirectionWorldSpace;
in vec3 normalsInNormalSpace;
in vec3 tangentInNormalSpace;
in vec3 bitangentInNormalSpace;
in vec3 normalsInWorldSpace;
in mat3 tangentBinormalNormalMatrix;

struct light {
    vec3 position;
    vec3 diffuse;
    vec3 specular;
    vec3 ambient;
    float attenuation;
};

uniform light lights;
uniform vec3 cameraPosition;
uniform vec2 textureRepeat;
uniform mat3 modelView3x3Matrix;
uniform mat3 normalMatrix;
uniform mat4 localToWorld4x4Matrix;
uniform float u_roughness;
uniform float u_metalness;
uniform vec4 baseColour;

layout(binding = 5) uniform samplerCube environmentMap;

void main()
{
    vec4 albedo = baseColour;

    if (albedo.a < 0.01)
    {
        discard;
    }


    fragColor = albedo;
}