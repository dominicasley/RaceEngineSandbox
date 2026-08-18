#version 420 core
// Only u_baseColour is consumed. The PassThroughVertexShader interpolants and the light
// uniforms it never reads are absent rather than redeclared: a stale redeclaration of an
// array-typed varying is a link error, and this pair is what the Vulkan variant mirrors.

layout(location = 0) out vec4 fragColor;

uniform vec4 u_baseColour;

void main()
{
    vec4 albedo = u_baseColour;

    if (albedo.a < 0.01)
    {
        discard;
    }


    fragColor = albedo;
}
