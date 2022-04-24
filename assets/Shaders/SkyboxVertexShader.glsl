#version 420

layout(location = 0) in vec3 vertexPositionModelSpace;
layout(location = 1) in vec2 vertexTextureCoordinates;
layout(location = 2) in vec3 vertexNormalModelSpace;
layout(location = 3) in vec4 vertexTangentModelSpace;

uniform mat4 localToScreen4x4Matrix;
out vec3 textureCoordinates;

void main()
{
    textureCoordinates = vertexPositionModelSpace;
    gl_Position = localToScreen4x4Matrix * vec4(vertexPositionModelSpace, 1.0);
}