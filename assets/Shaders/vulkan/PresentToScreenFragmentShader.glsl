#version 450
// Vulkan variant of PresentToScreenFragmentShader.glsl.

layout(location = 0) out vec4 fragColor;
layout(set = 0, binding = 0) uniform sampler2D presentationTexture;
layout(location = 0) in vec2 textureCoordinates;

void main ()
{
    fragColor = texture(presentationTexture, textureCoordinates);
}
