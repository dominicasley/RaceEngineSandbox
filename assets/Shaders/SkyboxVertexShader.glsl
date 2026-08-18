#version 420

layout(location = ATTRIBUTE_POSITION) in vec3 vertexPositionModelSpace;
layout(location = ATTRIBUTE_TEXCOORD) in vec2 vertexTextureCoordinates;
layout(location = ATTRIBUTE_NORMAL) in vec3 vertexNormalModelSpace;
layout(location = ATTRIBUTE_TANGENT) in vec4 vertexTangentModelSpace;

uniform mat4 localToScreen4x4Matrix;
out vec3 textureCoordinates;

void main()
{
    textureCoordinates = vertexPositionModelSpace;
    gl_Position = localToScreen4x4Matrix * vec4(vertexPositionModelSpace, 1.0);
}