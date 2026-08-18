#version 420 core

// The TEXTURE_* bindings are defined by the renderer from Graphics/Api/RenderContract.cppm;
// this file must not spell one of those numbers.

layout(location = 0) out vec4 fragColor;
layout(binding = TEXTURE_DIFFUSE) uniform sampler2D diffuseTexture;
layout(binding = TEXTURE_NORMAL) uniform sampler2D normalTexture;
layout(binding = TEXTURE_SPECULAR) uniform sampler2D roughnessTexture;
layout(binding = TEXTURE_EMISSIVE) uniform sampler2D emissiveTexture;
layout(binding = TEXTURE_OCCLUSION) uniform sampler2D occlusionTexture;
layout(binding = TEXTURE_ENVIRONMENT) uniform samplerCube environmentMap;

in vec2 textureCoordinates;
in vec3 positionInWorldSpace;
in vec3 normalsInWorldSpace;

void main() 
{	
	vec3 L = normalize(vec3(100, 100.0, 100.0) - positionInWorldSpace);
	vec3 Idiff = vec3(1.0, 1.0, 1.0) * max(dot(normalize(normalsInWorldSpace), L), 0.0);
	Idiff = clamp(Idiff, 0.5, 1.0);

	fragColor = vec4(texture(diffuseTexture, textureCoordinates).xyz * Idiff, 1.0);
}