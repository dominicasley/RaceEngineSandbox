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

// The cascaded shadow map. shadowMatrices take world space straight to a shadow-map lookup — the
// backend's depth convention and its texture origin are folded in by shadowLookupCorrection — so
// nothing below holds a convention of its own and the Vulkan dialect is the same arithmetic.
// shadowCascadeCount is 0 when this view has no cascades bound, which is the only case in which
// the samplers below must not be read.
uniform mat4 shadowMatrices[SHADOW_CASCADES];
uniform vec4 shadowSplits;          // where each cascade ends, along the view axis
uniform vec4 shadowTexelWorldSize;  // world units one texel of each cascade covers
uniform vec4 shadowDepthScale;      // normalised depth per world unit along the light, per cascade
uniform int shadowCascadeCount;
uniform int shadowLightIndex;
layout(binding = SHADOW_MAP_BINDING) uniform sampler2DShadow shadowMaps[SHADOW_CASCADES];

const float M_PI = 3.141592653;

// One cascade's percentage-closer average.
float shadowInCascade(int cascade, vec3 coordinate)
{
    float result = 1.0;

    // An array of samplers may only be indexed by a dynamically uniform expression, and a cascade
    // chosen from a fragment's own view depth is not one — fragments of a single triangle straddle
    // a split. The loop counter is dynamically uniform because its bounds are compile-time
    // constants, so this unrolls into a chain of constant-index branches: the form that is defined
    // on desktop GL without ARB_gpu_shader5 and on Vulkan without
    // shaderSampledImageArrayNonUniformIndexing, neither of which this engine asks for.
    for (int index = 0; index < SHADOW_CASCADES; index++)
    {
        if (index != cascade)
        {
            continue;
        }

        // Each tap is already a 2x2 percentage-closer average, because the comparison sampler
        // filters linearly; a (2r+1)^2 grid one texel apart therefore covers a (2r+2)^2
        // neighbourhood with tent weighting.
        float texel = 1.0 / float(textureSize(shadowMaps[index], 0).x);
        float total = 0.0;

        for (int y = -SHADOW_PCF_RADIUS; y <= SHADOW_PCF_RADIUS; y++)
        {
            for (int x = -SHADOW_PCF_RADIUS; x <= SHADOW_PCF_RADIUS; x++)
            {
                total += texture(shadowMaps[index], vec3(coordinate.xy + vec2(x, y) * texel, coordinate.z));
            }
        }

        result = total / float((2 * SHADOW_PCF_RADIUS + 1) * (2 * SHADOW_PCF_RADIUS + 1));
    }

    return result;
}

// How much of the shadow-casting light reaches this fragment, according to one cascade.
float shadowSample(int cascade, vec3 worldPosition, vec3 worldNormal, float NdL)
{
    float texelWorld = shadowTexelWorldSize[cascade];
    float sinTheta = sqrt(max(0.0, 1.0 - NdL * NdL));
    float tanTheta = min(sinTheta / max(NdL, 1.0 / float(SHADOW_MAX_SLOPE)), float(SHADOW_MAX_SLOPE));

    // Normal offset first. Moving the sample point off the surface sideways is what clears acne on
    // a slope without detaching the contact shadow, which a depth bias large enough to do the same
    // job alone would.
    vec4 lightSpace = shadowMatrices[cascade]
        * vec4(worldPosition + worldNormal * (texelWorld * float(SHADOW_NORMAL_OFFSET_TEXELS) * sinTheta), 1.0);
    vec3 coordinate = lightSpace.xyz / lightSpace.w;

    // Past the cascade's far plane nothing was stored to compare against, and the comparison would
    // read "occluded" for every fragment behind the map. Outside it laterally needs no test: the
    // sampler clamps to an opaque white border, which compares as lit.
    if (coordinate.z >= 1.0)
    {
        return 1.0;
    }

    // What the offset does not cover: the depth a surface at this slope crosses over the texels the
    // filter spans, expressed in the cascade's own normalised depth.
    coordinate.z -= texelWorld * (float(SHADOW_CONSTANT_BIAS_TEXELS) + float(SHADOW_SLOPE_BIAS_TEXELS) * tanTheta)
        * shadowDepthScale[cascade];

    return shadowInCascade(cascade, coordinate);
}

float shadowFactor(vec3 worldPosition, vec3 worldNormal, vec3 lightDirection, float viewDepth)
{
    if (shadowCascadeCount <= 0)
    {
        return 1.0;
    }

    float NdL = dot(worldNormal, lightDirection);
    if (NdL <= 0.0)
    {
        // Facing away from the light. The diffuse term is already zero here, and testing would
        // sample the far side of this very surface and shadow it a second time.
        return 1.0;
    }

    float lastSplit = shadowSplits[shadowCascadeCount - 1];
    if (viewDepth >= lastSplit)
    {
        return 1.0;
    }

    int cascade = shadowCascadeCount - 1;
    for (int index = 0; index < SHADOW_CASCADES; index++)
    {
        if (index < shadowCascadeCount && viewDepth < shadowSplits[index])
        {
            cascade = index;
            break;
        }
    }

    float shadow = shadowSample(cascade, worldPosition, worldNormal, NdL);

    // The seam. Filter width and bias both change at a split, which reads as a line ruled across
    // the ground; the last SHADOW_BLEND_PERCENT of a cascade cross-fades into the next.
    float split = shadowSplits[cascade];
    float blendStart = split * (1.0 - float(SHADOW_BLEND_PERCENT) / 100.0);
    if (cascade + 1 < shadowCascadeCount && viewDepth > blendStart)
    {
        float blend = clamp((viewDepth - blendStart) / max(split - blendStart, 0.0001), 0.0, 1.0);
        shadow = mix(shadow, shadowSample(cascade + 1, worldPosition, worldNormal, NdL), blend);
    }

    // And the far end, for the same reason: past the last cascade every fragment is lit, so the
    // last SHADOW_FADE_PERCENT of the shadow distance fades to lit rather than stopping dead.
    float fadeStart = lastSplit * (1.0 - float(SHADOW_FADE_PERCENT) / 100.0);

    return mix(shadow, 1.0, clamp((viewDepth - fadeStart) / max(lastSplit - fadeStart, 0.0001), 0.0, 1.0));
}

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

    // Once, outside the loop: one light casts the cascades, and the test is the same wherever in
    // the loop that light turns up. The geometric world normal, not the normal-mapped one — the
    // bias is a property of the surface the depth map recorded, not of its texture. Lights carry
    // the direction *towards* the light in `position`, which is what the loop below reads too.
    int shadowLight = clamp(shadowLightIndex, 0, MAX_LIGHTS - 1);
    float shadow = shadowFactor(positionInWorldSpace, normalize(normalsInWorldSpace),
                                normalize(lights[shadowLight].position), -positionInViewSpace.z);

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

        // Direct light only. Ambient is a floor rather than a contribution and the image-based
        // terms below come from every direction, so neither is something a shadow map can occlude.
        float occlusion_from_shadow = (lightIndex == shadowLight) ? shadow : 1.0;

        vec3 light_color = lights[lightIndex].diffuse * lights[lightIndex].attenuation * occlusion_from_shadow;
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
