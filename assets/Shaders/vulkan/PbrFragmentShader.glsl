#version 450

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

// Set 0: per camera pass. Set 1: per material. Set 2: per draw (dynamic offset).
layout(set = 0, binding = 0) uniform FrameData {
    mat4 viewMatrix;
    vec4 cameraPosition;
    vec4 lightPosition;
    vec4 lightDiffuse;
    vec4 lightSpecular;
    vec4 lightAmbientAttenuation; // xyz ambient, w attenuation
} frame;

// textureTransform is a 3x3 UV transform in a mat4 slot: std140 pads a mat3's columns to 16
// bytes each, which the C++ glm::mat3 does not, so the ABI carries it as a mat4.
layout(set = 1, binding = 0) uniform MaterialData {
    vec4 baseColour;
    vec4 roughMetal;       // x roughness, y metalness
    ivec4 useTextures;     // x diffuse, y normal, z specular, w emissive
    ivec4 useTextures2;    // x occlusion
    mat4 textureTransform; // KHR_texture_transform, upper 3x3
} material;
layout(set = 1, binding = 1) uniform sampler2D diffuseTexture;
layout(set = 1, binding = 2) uniform sampler2D normalTexture;
layout(set = 1, binding = 3) uniform sampler2D specularTexture;
layout(set = 1, binding = 4) uniform sampler2D emissiveTexture;
layout(set = 1, binding = 5) uniform sampler2D occlusionTexture;
layout(set = 1, binding = 6) uniform samplerCube environmentMap;


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
    vec3 local_light_pos = (mat3(frame.viewMatrix) * frame.lightPosition.xyz);

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

    vec3 incident_eye = normalize(vec3(positionInWorldSpace) - frame.cameraPosition.xyz);
    vec3 reflection_vector = -reflect(incident_eye, normalize(normalsInWorldSpace));
    vec3 envdiff = texture(environmentMap, reflection_vector, 10).xyz;

    incident_eye = normalize(vec3(positionInViewSpace));
    normalMap.z	= sqrt(1.0 - dot(normalMap.xy, normalMap.xy));
    vec3 worldNormal = normalize((tangentInNormalSpace * normalMap.x) + (bitangentInNormalSpace * normalMap.y) + (normalsInNormalSpace * normalMap.z));
    vec3 reflected_diff	= reflect(incident_eye, worldNormal);
    reflected_diff = inverse(mat3(frame.viewMatrix)) * reflected_diff;
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

    vec3 light_color = frame.lightDiffuse.xyz * frame.lightAmbientAttenuation.w;
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
    vec2 transformedTextureCoordinates = (mat3(material.textureTransform) * vec3(textureCoordinates, 1.0)).xy;

    vec4 albedo = (material.useTextures.x != 0) ? texture(diffuseTexture, transformedTextureCoordinates) : material.baseColour;
    vec3 normalMap = (material.useTextures.y != 0) ? normalize(texture(normalTexture, transformedTextureCoordinates).xyz * 2.0 - 1.0) : normalize(vec3(0.0, 0.0, 1.0));
    vec4 specularMap = (material.useTextures.z != 0) ? texture(specularTexture, transformedTextureCoordinates) : vec4(1.0, material.roughMetal.x, material.roughMetal.y, 1.0);
    vec3 colour = ads(0, albedo, specularMap, normalMap);

    fragColor = vec4(colour, albedo.a);
}