#version 420 core

layout(location = 0) out vec4 fragColor;
layout(binding=0) uniform samplerCube environmentMap;

in vec3 textureCoordinates;

void main()
{
    vec4 colour = texture(environmentMap, textureCoordinates);
    fragColor = vec4(colour);
}