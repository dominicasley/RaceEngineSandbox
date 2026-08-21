#version 450
// Vulkan variant of SkyboxVertexShader.glsl.

// Position only: the renderer feeds exactly the locations this shader declares, so
// declaring the unread attributes would cost a vertex binding per primitive for nothing.
layout(location = ATTRIBUTE_POSITION) in vec3 vertexPositionModelSpace;

// Declared so the set-0 block matches the ABI even though nothing here reads it.
struct Light {
    vec4 position;
    vec4 diffuse;
    vec4 specular;
    vec4 ambientAttenuation;
};

layout(set = SET_FRAME, binding = 0) uniform FrameData {
    mat4 viewMatrix;
    vec4 cameraPosition;
    ivec4 lightCount;
    Light lights[MAX_LIGHTS];
} frame;

layout(set = SET_DRAW, binding = 0) uniform DrawData {
    mat4 localToWorld;
    mat4 localToView;
    mat4 localToScreen;
    mat4 normalMatrix;
    ivec4 animated;
} draw;
// No JointData block: the palette is a binding of its own now rather than a tail of this one, so a
// sky that cannot be skinned no longer has to declare it to keep the std140 layout honest.

layout(location = 0) out vec3 textureCoordinates;

void main()
{
    // The world direction this vertex looks along, not the model-space position it used to be.
    //
    // They are not the same vector: SkyBox.glb carries a rotation in its own node transform, the
    // way a glTF exported from a Z-up tool does, so model space is the world turned on its side.
    // The fragment shader's atmosphere has a real up — its ray origin sits on a planet — and
    // feeding it model space rendered the sky rotated. It was patched there by negating y, which
    // swapped one wrong orientation for another and left no sun direction that could both light
    // the sky and agree with the scene's sun.
    //
    // localToWorld's upper 3x3 is the whole fix: it carries the node's rotation and the mesh's,
    // drops the translation that follows the camera, and leaves a scale the fragment stage
    // normalises away.
    textureCoordinates = mat3(draw.localToWorld) * vertexPositionModelSpace;
    gl_Position = draw.localToScreen * vec4(vertexPositionModelSpace, 1.0);
}
