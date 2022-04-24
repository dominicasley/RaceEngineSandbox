#version 420 core

layout(location = 0) out vec4 fragColor;
layout(binding = 0) uniform sampler2D diffuseTexture;
layout(binding = 1) uniform sampler2D normalTexture;
layout(binding = 2) uniform sampler2D roughnessTexture;
layout(binding = 3) uniform sampler2D emissiveTexture;
layout(binding = 4) uniform sampler2D occlusionTexture;
layout(binding = 5) uniform samplerCube environmentMap;

in vec2 textureCoordinates;
in vec3 positionInWorldSpace;
in vec3 normalsInWorldSpace;
in vec4 jointIndicies;

void main() 
{	
	vec3 L = normalize(vec3(100, 100.0, 100.0) - positionInWorldSpace);
	vec3 Idiff = vec3(1.0, 1.0, 1.0) * max(dot(normalize(normalsInWorldSpace), L), 0.0);
	Idiff = clamp(Idiff, 0.5, 1.0);

	fragColor = vec4(texture(diffuseTexture, textureCoordinates).xyz * Idiff, 1.0);
}