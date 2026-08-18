module;

#include <stdexcept>
#include <utility>

#include <glm/glm.hpp>

export module osr.game:WaterLevel;

import :Bollard;
import :CarEntity;
import :DinosaurEntity;
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

WaterLevel::WaterLevel(raceengine::Engine& engine) :
    engine(engine),
    scene(engine.sceneManager().createScene()),
    camera(engine.scene().createCamera(scene)),
    cameraController(engine)
{
    engine.scene().createLight(scene) = raceengine::Light{.position = glm::vec3(0.0f, 350.0f, 350.0f),
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
                 engine.resource().loadTextFileAsync("assets/Shaders/vulkan/ColourFragmentShader.glsl"),
                 engine.resource().loadTextFileAsync("assets/Shaders/vulkan/HdrVertexShader.glsl"),
                 engine.resource().loadTextFileAsync("assets/Shaders/vulkan/HdrFragmentShader.glsl"),
                 engine.resource().loadTextFileAsync("assets/Shaders/vulkan/SkyboxVertexShader.glsl"),
                 engine.resource().loadTextFileAsync("assets/Shaders/vulkan/SkyboxFragmentShader.glsl"));

    if (!loaded)
    {
        throw std::runtime_error(loaded.error());
    }

    auto [presentationVert, presentationFrag, vert, pbrFragmentShader, colourFragmentShader, hdrVertexShader,
          hdrFragmentShader, skyboxModel, skyboxVertexShader, skyboxFragmentShader, front, back, left, right, top,
          bottom, vulkanPresentationVert, vulkanPresentationFrag, vulkanVert, vulkanPbrFragmentShader,
          vulkanColourFragmentShader, vulkanHdrVertexShader, vulkanHdrFragmentShader, vulkanSkyboxVertexShader,
          vulkanSkyboxFragmentShader] = std::move(loaded).value();

    auto presentationShader =
        engine.shader().createShader("present", ShaderDescriptor{.vertexShaderSource = presentationVert,
                                                                 .fragmentShaderSource = presentationFrag,
                                                                 .vulkanVertexShaderSource = vulkanPresentationVert,
                                                                 .vulkanFragmentShaderSource = vulkanPresentationFrag});

    engine.shader().createShader("pbr", ShaderDescriptor{.vertexShaderSource = vert,
                                                         .fragmentShaderSource = pbrFragmentShader,
                                                         .vulkanVertexShaderSource = vulkanVert,
                                                         .vulkanFragmentShaderSource = vulkanPbrFragmentShader});

    engine.shader().createShader("colour", ShaderDescriptor{.vertexShaderSource = vert,
                                                            .fragmentShaderSource = colourFragmentShader,
                                                            .vulkanVertexShaderSource = vulkanVert,
                                                            .vulkanFragmentShaderSource = vulkanColourFragmentShader});

    auto skyboxShader = engine.shader().createShader(
        "skybox", ShaderDescriptor{.vertexShaderSource = skyboxVertexShader,
                                   .fragmentShaderSource = skyboxFragmentShader,
                                   .vulkanVertexShaderSource = vulkanSkyboxVertexShader,
                                   .vulkanFragmentShaderSource = vulkanSkyboxFragmentShader});

    auto hdrShader =
        engine.shader().createShader("hdr", ShaderDescriptor{.vertexShaderSource = hdrVertexShader,
                                                             .fragmentShaderSource = hdrFragmentShader,
                                                             .vulkanVertexShaderSource = vulkanHdrVertexShader,
                                                             .vulkanFragmentShaderSource = vulkanHdrFragmentShader});

    scene.environment = engine.cubeMap().create("sky", front, back, left, right, top, bottom);

    auto hdr = engine.postProcess().create("hdr", hdrShader.value());

    for (auto& attachment : engine.camera().getOutputBuffer(camera).attachments)
    {
        engine.postProcess().addInput(hdr, attachment);
    }

    engine.camera().addPostProcess(camera, hdr);

    auto hdrPostProcess = engine.memoryStorage().postProcesses.get(hdr);

    auto outputAttachment = engine.fbo().getAttachmentsOfType(
        engine.memoryStorage().frameBuffers.get(hdrPostProcess.output.value()), FboAttachmentType::Color);

    engine.presenter().setPresenter(
        Presenter{.output = outputAttachment.front(), .shader = presentationShader.value()});

    auto& skyEntity =
        engine.scene().createEntity(scene, CreateRenderableModelDTO{.node = engine.sceneManager().createNode(scene),
                                                                    .shader = skyboxShader.value(),
                                                                    .model = skyboxModel});

    engine.sceneManager().setScale(skyEntity.node, 2500.0f, 2500.0f, 2500.0f);

    this->sky = &skyEntity;

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
