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
uniform vec4 u_baseColour;

uniform bool u_useDiffuseTexture;
uniform bool u_useNormalTexture;
uniform bool u_useSpecularTexture;
uniform bool u_useEmissiveTexture;
uniform bool u_useOcclusionTexture;

layout(binding = 0) uniform sampler2D diffuseTexture;
layout(binding = 1) uniform sampler2D normalTexture;
layout(binding = 2) uniform sampler2D specularTexture;
layout(binding = 3) uniform sampler2D emissiveTexture;
layout(binding = 4) uniform sampler2D occlusionTexture;
layout(binding = 5) uniform samplerCube environmentMap;

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

vec3 ads(int lightIndex, vec4 albedo, vec4 metallicRoughness, vec3 normalMap)
{
    vec3 local_light_pos = (modelView3x3Matrix * lights.position.xyz);

    float A = 20.0 / dot(local_light_pos - positionInViewSpace, local_light_pos - positionInViewSpace);

    vec3 L = normalize(lightDirectionWorldSpace);
    vec3 V = normalize(viewDirectionWorldSpace);
    vec3 H = normalize(L + V);
    vec3 nn = normalize(normalsInNormalSpace);
    vec3 N = normalMap;

    float occlusion = metallicRoughness.r;
    float roughness = metallicRoughness.g;
    float metallic = metallicRoughness.b;
    float alpha = albedo.a;

    vec3 specular = mix(vec3(0.04), albedo.rgb, metallic);

    vec3 nm = tangentBinormalNormalMatrix * normalMap;
    mat3x3 tnrm = transpose(normalMatrix);
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

    float NdL = max(0.001, dot(N, L));
    float NdV = max(0.001, dot(N, V));
    float NdH = max(0.001, dot(N, H));
    float HdV = max(0.001, dot(H, V));
    float LdV = max(0.001, dot(L, V));

    float bias = 0.005 * tan(acos(NdL));
    bias = clamp(bias, 0.0, 0.01);

    vec3 specfresnel = fresnel_factor(specular, HdV);
    vec3 specref = cooktorrance_specular(NdL, NdV, NdH, specfresnel, roughness);

    specref *= vec3(NdL);

    vec3 diffref = (vec3(1.0) - specfresnel) * phong_diffuse() * NdL;

    vec3 reflected_light = vec3(0);
    vec3 diffuse_light = vec3(0);

    vec3 light_color = lights.diffuse * lights.attenuation;
    reflected_light += specref * light_color;
    diffuse_light += diffref * light_color;

    // IBL
    //vec2 brdf = texture2D(iblbrdf, vec2(roughness, 1.0 - NdV)).xy;
    vec3 iblspec = min(vec3(0.99), fresnel_factor(specular, NdV) * 0.8);
    reflected_light += iblspec * envspec;
    diffuse_light += envdiff * (1.0 / M_PI);
    diffuse_light = max(diffuse_light, vec3(0.29859, 0.29973, 0.3));

    return occlusion * diffuse_light * mix(albedo.rgb, vec3(0.0), metallic) + max(reflected_light, 0.0);
}

void main()
{
    vec4 albedo = u_useDiffuseTexture ? texture(diffuseTexture, textureCoordinates * textureRepeat) : u_baseColour;
    vec3 normalMap = u_useNormalTexture ? normalize(texture(normalTexture, textureCoordinates * textureRepeat).xyz * 2.0 - 1.0) : normalize(vec3(0.0, 0.0, 1.0));
    vec4 specularMap = u_useSpecularTexture ? texture(specularTexture, textureCoordinates * textureRepeat) : vec4(1.0, u_roughness, u_metalness, 1.0);
    vec3 colour = ads(0, albedo, specularMap, normalMap);

    fragColor = vec4(colour, albedo.a);
}