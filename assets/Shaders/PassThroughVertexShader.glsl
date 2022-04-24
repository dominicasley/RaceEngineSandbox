#version 420
#define MAX_JOINTS 128

layout(location = 0) in vec3 vertexPositionModelSpace;
layout(location = 1) in vec2 vertexTextureCoordinates;
layout(location = 2) in vec3 vertexNormalModelSpace;
layout(location = 3) in vec4 vertexTangentModelSpace;
layout(location = 4) in vec4 vertexJointIndicies;
layout(location = 5) in vec4 vertexJointWeights;

uniform bool animated;
uniform mat4 localToScreen4x4Matrix;
uniform mat4 localToView4x4Matrix;
uniform mat4 localToWorld4x4Matrix;
uniform mat3 modelView3x3Matrix;
uniform mat3 normalMatrix;
uniform mat4 jointTransformationMatrixes[MAX_JOINTS];

struct light {
	vec3 position;
	vec3 diffuse;
	vec3 specular;
	vec3 ambient;
	float attenuation;
};

uniform light lights;

out vec2 textureCoordinates;
out vec3 positionInWorldSpace;
out vec3 positionInViewSpace;
out vec3 normalsInNormalSpace;
out vec3 tangentInNormalSpace;
out vec3 bitangentInNormalSpace;
out vec3 normalsInWorldSpace;
out vec4 jointIndicies;
out vec3 viewDirectionWorldSpace;
out vec3 lightDirectionWorldSpace;
out mat3 tangentBinormalNormalMatrix;

void main()
{
	mat4 boneTransform = mat4(1.0f);
	vec4 jointWeights = vertexJointWeights;

	if (animated) {
		boneTransform = jointWeights.x * jointTransformationMatrixes[int(vertexJointIndicies.x)] +
		jointWeights.y * jointTransformationMatrixes[int(vertexJointIndicies.y)] +
		jointWeights.z * jointTransformationMatrixes[int(vertexJointIndicies.z)] +
		jointWeights.w * jointTransformationMatrixes[int(vertexJointIndicies.w)];
	}

	jointIndicies = vertexJointIndicies;
	textureCoordinates = vertexTextureCoordinates;
	positionInWorldSpace = vec3(localToWorld4x4Matrix * boneTransform * vec4(vertexPositionModelSpace, 1.0));
	positionInViewSpace = vec3(localToView4x4Matrix * boneTransform * vec4(vertexPositionModelSpace, 1.0));

	vec3 bitangent = cross(vertexNormalModelSpace, vertexTangentModelSpace.xyz ) * vertexTangentModelSpace.w;

	normalsInWorldSpace = normalize(localToWorld4x4Matrix * boneTransform * vec4(vertexNormalModelSpace.xyz, 0.0)).xyz;
	normalsInNormalSpace = normalize(normalMatrix * mat3(boneTransform) * vertexNormalModelSpace);
	tangentInNormalSpace = normalize(normalMatrix * mat3(boneTransform) * vec3(vertexTangentModelSpace));
	bitangentInNormalSpace = normalize(normalMatrix * mat3(boneTransform) * bitangent);

	tangentBinormalNormalMatrix = mat3(
		tangentInNormalSpace.x, bitangentInNormalSpace.x, normalsInNormalSpace.x,
		tangentInNormalSpace.y, bitangentInNormalSpace.y, normalsInNormalSpace.y,
		tangentInNormalSpace.z, bitangentInNormalSpace.z, normalsInNormalSpace.z
	);

	lightDirectionWorldSpace = tangentBinormalNormalMatrix * (modelView3x3Matrix * lights.position);
	viewDirectionWorldSpace	= tangentBinormalNormalMatrix * -positionInViewSpace;

	gl_Position	= localToScreen4x4Matrix * boneTransform * vec4(vertexPositionModelSpace, 1.0);
}


