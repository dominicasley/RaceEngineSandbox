#version 420 core

layout(location = 0) out vec4 fragColor;

layout(binding = 0) uniform sampler2D presentationTexture;

in vec2 textureCoordinates;

void main ()
{
    vec4 colour = texture(presentationTexture, textureCoordinates);
    fragColor = colour;
}