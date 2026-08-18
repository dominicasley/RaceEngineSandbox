#version 420 core
// MAX_LIGHTS and every TEXTURE_* binding are defined by the renderer from
// Graphics/Api/RenderContract.cppm; this file must not spell one of those numbers.

layout(location = 0) out vec4 fragColor;

in vec3 positionInViewSpace;
in vec3 positionInWorldSpace;
in vec2 textureCoordinates;
in vec3 viewDirectionWorldSpace;
in vec3 lightDirectionWorldSpace[MAX_LIGHTS];
in vec3 normalsInNormalSpace;
in vec3 tangentInNormalSpace;
in vec3 bitangentInNormalSpace;
in vec3 normalsInWorldSpace;

struct light {
    vec3 position;
    vec3 diffuse;
    vec3 specular;
    vec3 ambient;
    float attenuation;
};

uniform light lights[MAX_LIGHTS];
uniform int lightCount;
uniform vec3 cameraPosition;
uniform mat3 textureTransform;
uniform mat3 modelView3x3Matrix;
uniform mat4 localToWorld4x4Matrix;
uniform float u_roughness;
uniform float u_metalness;
uniform vec4 u_baseColour;

uniform bool u_useDiffuseTexture;
uniform bool u_useNormalTexture;
uniform bool u_useSpecularTexture;
uniform bool u_useEmissiveTexture;
uniform bool u_useOcclusionTexture;

layout(binding = TEXTURE_DIFFUSE) uniform sampler2D diffuseTexture;
layout(binding = TEXTURE_NORMAL) uniform sampler2D normalTexture;
layout(binding = TEXTURE_SPECULAR) uniform sampler2D specularTexture;
layout(binding = TEXTURE_EMISSIVE) uniform sampler2D emissiveTexture;
layout(binding = TEXTURE_OCCLUSION) uniform sampler2D occlusionTexture;
layout(binding = TEXTURE_ENVIRONMENT) uniform samplerCube environmentMap;

const float M_PI = 3.141592653;

float phong_diffuse()
{
    return (1.0 / M_PI);
}

float D_GGX(in float roughness, in float NdH)
{
    float m = roughness * roughness;
    float m2 = m * m;
    float d = (NdH * m2 - NdH) * NdH + 1.0;
    return m2 / (M_PI * d * d);
}

vec3 fresnel_factor(in vec3 f0, in float product)
{
    return mix(f0, vec3(1.0), pow(1.01 - product, 5.0));
}

float G_schlick(in float roughness, in float NdV, in float NdL)
{
    float k = roughness * roughness * 0.5;
    float V = NdV * (1.0 - k) + k;
    float L = NdL * (1.0 - k) + k;
    return 0.25 / (V * L);
}

vec3 cooktorrance_specular(in float NdL, in float NdV, in float NdH, in vec3 specular, in float roughness)
{
    float D = D_GGX(roughness, NdH);
    float G = G_schlick(roughness, NdV, NdL);
    float rim = mix(1.0 - roughness * 0.6 * 0.9, 1.0, NdV);
    return (1.0 / rim) * specular * G * D;
}

vec3 ads(vec4 albedo, vec4 metallicRoughness, vec3 normalMap)
{
    vec3 V = normalize(viewDirectionWorldSpace);
    vec3 N = normalMap;

    float occlusion = metallicRoughness.r;
    float roughness = metallicRoughness.g;
    float metallic = metallicRoughness.b;

    vec3 specular = mix(vec3(0.04), albedo.rgb, metallic);

    vec3 incident_eye = normalize(vec3(positionInWorldSpace) - cameraPosition);
    vec3 reflection_vector = -reflect(incident_eye, normalize(normalsInWorldSpace));
    vec3 envdiff = texture(environmentMap, reflection_vector, 10).xyz;

    incident_eye = normalize(vec3(positionInViewSpace));
    normalMap.z	= sqrt(1.0 - dot(normalMap.xy, normalMap.xy));
    vec3 worldNormal = normalize((tangentInNormalSpace * normalMap.x) + (bitangentInNormalSpace * normalMap.y) + (normalsInNormalSpace * normalMap.z));
    vec3 reflected_diff	= reflect(incident_eye, worldNormal);
    reflected_diff = inverse(modelView3x3Matrix) * reflected_diff;
    reflected_diff = normalize(reflected_diff);
    reflected_diff *= vec3(-1.0f);

    vec3 envspec = textureLod(environmentMap, reflected_diff, max(roughness * 11.0, textureQueryLod(environmentMap, reflected_diff).y)).xyz;

    float NdV = max(0.001, dot(N, V));

    vec3 reflected_light = vec3(0);
    vec3 diffuse_light = vec3(0);
    // Ambient is a per-light property that floors the diffuse term; the lights sum into one
    // floor so a single light reproduces the term exactly.
    vec3 ambient_light = vec3(0);

    for (int lightIndex = 0; lightIndex < lightCount; lightIndex++)
    {
        vec3 L = normalize(lightDirectionWorldSpace[lightIndex]);
        vec3 H = normalize(L + V);

        float NdL = max(0.001, dot(N, L));
        float NdH = max(0.001, dot(N, H));
        float HdV = max(0.001, dot(H, V));

        vec3 specfresnel = fresnel_factor(specular, HdV);
        vec3 specref = cooktorrance_specular(NdL, NdV, NdH, specfresnel, roughness);

        specref *= vec3(NdL);

        vec3 diffref = (vec3(1.0) - specfresnel) * phong_diffuse() * NdL;

        vec3 light_color = lights[lightIndex].diffuse * lights[lightIndex].attenuation;
        reflected_light += specref * light_color;
        diffuse_light += diffref * light_color;
        ambient_light += lights[lightIndex].ambient;
    }

    // IBL
    //vec2 brdf = texture2D(iblbrdf, vec2(roughness, 1.0 - NdV)).xy;
    vec3 iblspec = min(vec3(0.99), fresnel_factor(specular, NdV) * 0.8);
    reflected_light += iblspec * envspec;
    diffuse_light += envdiff * (1.0 / M_PI);
    diffuse_light = max(diffuse_light, ambient_light);

    return occlusion * diffuse_light * mix(albedo.rgb, vec3(0.0), metallic) + max(reflected_light, 0.0);
}

void main()
{
    vec2 transformedTextureCoordinates = (textureTransform * vec3(textureCoordinates, 1.0)).xy;

    vec4 albedo = u_useDiffuseTexture ? texture(diffuseTexture, transformedTextureCoordinates) : u_baseColour;
    vec3 normalMap = u_useNormalTexture ? normalize(texture(normalTexture, transformedTextureCoordinates).xyz * 2.0 - 1.0) : normalize(vec3(0.0, 0.0, 1.0));
    vec4 specularMap = u_useSpecularTexture ? texture(specularTexture, transformedTextureCoordinates) : vec4(1.0, u_roughness, u_metalness, 1.0);
    vec3 colour = ads(albedo, specularMap, normalMap);

    fragColor = vec4(colour, albedo.a);
}
