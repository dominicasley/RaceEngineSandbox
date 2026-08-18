module;

#include <expected>
#include <stdexcept>
#include <string>
#include <utility>

#include <glm/glm.hpp>

export module osr.game:WaterLevel;

import :Bollard;
import :CarEntity;
import :DinosaurEntity;
import :GroundPlane;
import :FPSCameraController;

import raceengine;

namespace osr
{

export class WaterLevel
{
private:
    raceengine::Engine& engine;
    Scene& scene;
    Camera& camera;
    FPSCameraController cameraController;

    RenderableModel* sky;

    glm::vec3 velocity = glm::vec3(0.0f);
    glm::vec3 cpuVelocity = glm::vec3(0.0f);
    glm::vec3 acceleration = glm::vec3(0.0f);
    glm::vec3 ballVelocity = glm::vec3(0.0f, 0.0, 300.0f);

public:
    explicit WaterLevel(raceengine::Engine& engine);
    void update(float delta);
};

} // namespace osr

namespace osr
{

namespace
{

// The level is the bottom of this stack: nothing above it can carry on without the camera,
// the shaders or the sky it is asking for, so a reported failure becomes the exception that
// stops the process with the engine's own message attached.
template <typename T> T orThrow(std::expected<T, std::string> result)
{
    if (!result)
    {
        throw std::runtime_error(result.error());
    }

    return std::move(result).value();
}

} // namespace

WaterLevel::WaterLevel(raceengine::Engine& engine) :
    engine(engine),
    scene(engine.sceneManager().createScene()),
    camera(orThrow(engine.scene().createCamera(scene))),
    cameraController(engine)
{
    // Directional, and its direction is the exact opposite of the position the shading reads as
    // "towards the light" — the cascades are fitted along `direction` and the lighting is computed
    // from `position`, so anything else would put the shadow where the light is not.
    const auto sunPosition = glm::vec3(0.0f, 350.0f, 350.0f);
    auto& sun = engine.scene().createLight(scene);
    sun = raceengine::Light{.type = raceengine::LightType::Directional,
                            .position = sunPosition,
                            .direction = -glm::normalize(sunPosition),
                            .diffuse = glm::vec3(1.2859 * 2.5, 1.2973 * 2.5, 1.3 * 2.5),
                            .specular = glm::vec3(1.2859, 1.2973, 1.3),
                            .ambient = glm::vec3(0.29859, 0.29973, 0.3),
                            .attenuation = 1.0f};

    engine.camera().setPosition(camera, 0, 600, -450);
    engine.camera().setRoll(camera, 0, 1, 0);
    engine.camera().lookAtPoint(camera, 0, 0, 0);

    auto loaded =
        awaitAll(engine.resource().loadTextFileAsync("assets/Shaders/PresentToScreenVertexShader.glsl"),
                 engine.resource().loadTextFileAsync("assets/Shaders/PresentToScreenFragmentShader.glsl"),
                 engine.resource().loadTextFileAsync("assets/Shaders/PassThroughVertexShader.glsl"),
                 engine.resource().loadTextFileAsync("assets/Shaders/PbrFragmentShader.glsl"),
                 engine.resource().loadTextFileAsync("assets/Shaders/DepthOnlyVertexShader.glsl"),
                 engine.resource().loadTextFileAsync("assets/Shaders/DepthOnlyFragmentShader.glsl"),
                 engine.resource().loadTextFileAsync("assets/Shaders/ColourFragmentShader.glsl"),
                 engine.resource().loadTextFileAsync("assets/Shaders/HdrVertexShader.glsl"),
                 engine.resource().loadTextFileAsync("assets/Shaders/HdrFragmentShader.glsl"),
                 engine.resource().loadModelAsync("assets/Models/SkyBox/SkyBox.glb"),
                 engine.resource().loadTextFileAsync("assets/Shaders/SkyboxVertexShader.glsl"),
                 engine.resource().loadTextFileAsync("assets/Shaders/SkyboxFragmentShader.glsl"),
                 engine.resource().loadTextureAsync("assets/Textures/Skies/Field/pz.hdr"),
                 engine.resource().loadTextureAsync("assets/Textures/Skies/Field/nz.hdr"),
                 engine.resource().loadTextureAsync("assets/Textures/Skies/Field/nx.hdr"),
                 engine.resource().loadTextureAsync("assets/Textures/Skies/Field/px.hdr"),
                 engine.resource().loadTextureAsync("assets/Textures/Skies/Field/py.hdr"),
                 engine.resource().loadTextureAsync("assets/Textures/Skies/Field/ny.hdr"),
                 engine.resource().loadTextFileAsync("assets/Shaders/vulkan/PresentToScreenVertexShader.glsl"),
                 engine.resource().loadTextFileAsync("assets/Shaders/vulkan/PresentToScreenFragmentShader.glsl"),
                 engine.resource().loadTextFileAsync("assets/Shaders/vulkan/PassThroughVertexShader.glsl"),
                 engine.resource().loadTextFileAsync("assets/Shaders/vulkan/PbrFragmentShader.glsl"),
                 engine.resource().loadTextFileAsync("assets/Shaders/vulkan/DepthOnlyVertexShader.glsl"),
                 engine.resource().loadTextFileAsync("assets/Shaders/vulkan/DepthOnlyFragmentShader.glsl"),
                 engine.resource().loadTextFileAsync("assets/Shaders/vulkan/ColourFragmentShader.glsl"),
                 engine.resource().loadTextFileAsync("assets/Shaders/vulkan/HdrVertexShader.glsl"),
                 engine.resource().loadTextFileAsync("assets/Shaders/vulkan/HdrFragmentShader.glsl"),
                 engine.resource().loadTextFileAsync("assets/Shaders/vulkan/SkyboxVertexShader.glsl"),
                 engine.resource().loadTextFileAsync("assets/Shaders/vulkan/SkyboxFragmentShader.glsl"));

    if (!loaded)
    {
        throw std::runtime_error(loaded.error());
    }

    auto [presentationVert, presentationFrag, vert, pbrFragmentShader, depthVertexShader, depthFragmentShader,
          colourFragmentShader, hdrVertexShader, hdrFragmentShader, skyboxModel, skyboxVertexShader,
          skyboxFragmentShader, front, back, left, right, top, bottom, vulkanPresentationVert, vulkanPresentationFrag,
          vulkanVert, vulkanPbrFragmentShader, vulkanDepthVertexShader, vulkanDepthFragmentShader,
          vulkanColourFragmentShader, vulkanHdrVertexShader, vulkanHdrFragmentShader, vulkanSkyboxVertexShader,
          vulkanSkyboxFragmentShader] = std::move(loaded).value();

    auto presentationShader = orThrow(engine.shader().createShader(
        "present", ShaderDescriptor{.vertexShaderSource = presentationVert,
                                    .fragmentShaderSource = presentationFrag,
                                    .vulkanVertexShaderSource = vulkanPresentationVert,
                                    .vulkanFragmentShaderSource = vulkanPresentationFrag}));

    orThrow(
        engine.shader().createShader("pbr", ShaderDescriptor{.vertexShaderSource = vert,
                                                             .fragmentShaderSource = pbrFragmentShader,
                                                             .vulkanVertexShaderSource = vulkanVert,
                                                             .vulkanFragmentShaderSource = vulkanPbrFragmentShader}));

    // The cascades' depth pass. Position through the light's matrix, nothing written: the target
    // has no colour attachment for a fragment output to reach.
    auto depthShader = orThrow(engine.shader().createShader(
        "depth", ShaderDescriptor{.vertexShaderSource = depthVertexShader,
                                  .fragmentShaderSource = depthFragmentShader,
                                  .vulkanVertexShaderSource = vulkanDepthVertexShader,
                                  .vulkanFragmentShaderSource = vulkanDepthFragmentShader}));

    orThrow(engine.shader().createShader("colour",
                                         ShaderDescriptor{.vertexShaderSource = vert,
                                                          .fragmentShaderSource = colourFragmentShader,
                                                          .vulkanVertexShaderSource = vulkanVert,
                                                          .vulkanFragmentShaderSource = vulkanColourFragmentShader}));

    auto skyboxShader = orThrow(engine.shader().createShader(
        "skybox", ShaderDescriptor{.vertexShaderSource = skyboxVertexShader,
                                   .fragmentShaderSource = skyboxFragmentShader,
                                   .vulkanVertexShaderSource = vulkanSkyboxVertexShader,
                                   .vulkanFragmentShaderSource = vulkanSkyboxFragmentShader}));

    auto hdrShader = orThrow(
        engine.shader().createShader("hdr", ShaderDescriptor{.vertexShaderSource = hdrVertexShader,
                                                             .fragmentShaderSource = hdrFragmentShader,
                                                             .vulkanVertexShaderSource = vulkanHdrVertexShader,
                                                             .vulkanFragmentShaderSource = vulkanHdrFragmentShader}));

    scene.environment = orThrow(engine.cubeMap().create("sky", front, back, left, right, top, bottom));

    auto hdr = orThrow(engine.postProcess().create("hdr", hdrShader));

    for (auto& attachment : engine.camera().getOutputBuffer(camera).attachments)
    {
        engine.postProcess().addInput(hdr, attachment);
    }

    engine.camera().addPostProcess(camera, hdr);

    auto hdrPostProcess = engine.memoryStorage().postProcesses.get(hdr);

    auto outputAttachment = engine.fbo().getAttachmentsOfType(
        engine.memoryStorage().frameBuffers.get(hdrPostProcess.output.value()), FboAttachmentType::Color);

    engine.presenter().setPresenter(Presenter{.output = outputAttachment.front(), .shader = presentationShader});

    // Four depth-only orthographic cameras appended to this scene, refitted to the camera's
    // frustum every frame. 2048 square: at this camera's field of view the nearest cascade is then
    // well under a world unit per texel, which is what makes a contact shadow read as an edge
    // rather than a staircase. The distance is where the world stops being worth shadowing, and
    // the caster extent is roughly how tall the building is, measured along the light.
    orThrow(engine.shadow().enable(scene, sun, camera,
                                   raceengine::CreateShadowCascadesDTO{.depthShader = depthShader,
                                                                       .resolution = 2048,
                                                                       .lambda = 0.5f,
                                                                       .distance = 2000.0f,
                                                                       .casterExtent = 1500.0f}));

    auto& skyEntity = engine.scene().createEntity(
        scene, CreateRenderableModelDTO{
                   .node = engine.sceneManager().createNode(scene), .shader = skyboxShader, .model = skyboxModel});

    engine.sceneManager().setScale(skyEntity.node, 2500.0f, 2500.0f, 2500.0f);
    // The sky is not a caster. A 2500-unit box in the depth map fills every cascade at its near
    // plane, and the whole world is then in its shadow.
    skyEntity.castsShadow = false;

    this->sky = &skyEntity;

    GroundPlane(engine, scene);
    DinosaurEntity(engine, scene);
    CarEntity(engine, scene);
    Bollard(engine, scene);

    // Registered last, once the level is fully built: the engine may call this the moment the
    // first tick runs, and a half-constructed level is not something it should be handed.
    engine.onUpdate([this](float delta) { update(delta); });
}

void WaterLevel::update(float delta)
{
    cameraController.update(camera, delta);
    engine.sceneManager().setPosition(sky->node, camera.position.x, camera.position.y, camera.position.z);
}

} // namespace osr
