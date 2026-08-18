#version 420
// The shadow cascades' depth pass. Position through the light's matrix and nothing else: a depth
// map records where a surface is, and every material in the world agrees about that.
//
// MAX_JOINTS and the ATTRIBUTE_* locations are defined by the renderer from
// Graphics/Api/RenderContract.cppm; this file must not spell one of those numbers.

layout(location = ATTRIBUTE_POSITION) in vec3 vertexPositionModelSpace;
layout(location = ATTRIBUTE_JOINT) in vec4 vertexJointIndicies;
layout(location = ATTRIBUTE_WEIGHT) in vec4 vertexJointWeights;

// The cascade camera's own localToScreen: the renderer fills it per draw exactly as it does for a
// shading pass, so a skinned caster deforms in the shadow map the same way it deforms on screen.
uniform bool animated;
uniform mat4 localToScreen4x4Matrix;
uniform mat4 jointTransformationMatrixes[MAX_JOINTS];

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

	gl_Position = localToScreen4x4Matrix * boneTransform * vec4(vertexPositionModelSpace, 1.0);
}
