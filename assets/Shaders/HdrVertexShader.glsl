#version 420 core

layout(location = 0) in vec2 vertexPositionModelspace;
layout(location = 1) in vec2 vertexTextureCoordinates;

out vec2 textureCoordinates;

void main ()
{
    textureCoordinates = vertexTextureCoordinates;
    gl_Position = vec4 (vertexPositionModelspace, 0.0, 1.0);
}