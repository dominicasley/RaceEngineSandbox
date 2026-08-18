#version 420
// MAX_JOINTS, MAX_LIGHTS and every ATTRIBUTE_* location are defined by the renderer from
// Graphics/Api/RenderContract.cppm; this file must not spell one of those numbers.

layout(location = ATTRIBUTE_POSITION) in vec3 vertexPositionModelSpace;
layout(location = ATTRIBUTE_TEXCOORD) in vec2 vertexTextureCoordinates;
layout(location = ATTRIBUTE_NORMAL) in vec3 vertexNormalModelSpace;
layout(location = ATTRIBUTE_TANGENT) in vec4 vertexTangentModelSpace;
layout(location = ATTRIBUTE_JOINT) in vec4 vertexJointIndicies;
layout(location = ATTRIBUTE_WEIGHT) in vec4 vertexJointWeights;

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

uniform light lights[MAX_LIGHTS];
uniform int lightCount;

out vec2 textureCoordinates;
out vec3 positionInWorldSpace;
out vec3 positionInViewSpace;
out vec3 normalsInNormalSpace;
out vec3 tangentInNormalSpace;
out vec3 bitangentInNormalSpace;
out vec3 normalsInWorldSpace;
out vec3 viewDirectionWorldSpace;
// One direction per declared light; elements at or past lightCount are never read.
out vec3 lightDirectionWorldSpace[MAX_LIGHTS];

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

	textureCoordinates = vertexTextureCoordinates;
	positionInWorldSpace = vec3(localToWorld4x4Matrix * boneTransform * vec4(vertexPositionModelSpace, 1.0));
	positionInViewSpace = vec3(localToView4x4Matrix * boneTransform * vec4(vertexPositionModelSpace, 1.0));

	vec3 bitangent = cross(vertexNormalModelSpace, vertexTangentModelSpace.xyz ) * vertexTangentModelSpace.w;

	normalsInWorldSpace = normalize(localToWorld4x4Matrix * boneTransform * vec4(vertexNormalModelSpace.xyz, 0.0)).xyz;
	normalsInNormalSpace = normalize(normalMatrix * mat3(boneTransform) * vertexNormalModelSpace);
	tangentInNormalSpace = normalize(normalMatrix * mat3(boneTransform) * vec3(vertexTangentModelSpace));
	bitangentInNormalSpace = normalize(normalMatrix * mat3(boneTransform) * bitangent);

	mat3 tangentBinormalNormalMatrix = mat3(
		tangentInNormalSpace.x, bitangentInNormalSpace.x, normalsInNormalSpace.x,
		tangentInNormalSpace.y, bitangentInNormalSpace.y, normalsInNormalSpace.y,
		tangentInNormalSpace.z, bitangentInNormalSpace.z, normalsInNormalSpace.z
	);

	for (int lightIndex = 0; lightIndex < lightCount; lightIndex++) {
		lightDirectionWorldSpace[lightIndex] = tangentBinormalNormalMatrix * (modelView3x3Matrix * lights[lightIndex].position);
	}

	viewDirectionWorldSpace	= tangentBinormalNormalMatrix * -positionInViewSpace;

	gl_Position	= localToScreen4x4Matrix * boneTransform * vec4(vertexPositionModelSpace, 1.0);
}
