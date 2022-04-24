#version 420 core

layout(location = 0) out vec4 fragColor;

layout(binding = 0) uniform sampler2D presentationTexture;

in vec2 textureCoordinates;

vec3 acesFilm(const vec3 x) {
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d ) + e), 0.0, 1.0);
}

void main ()
{
    vec3 colour = acesFilm(texture(presentationTexture, textureCoordinates).xyz);
    fragColor = vec4(colour, 1.0f);
}