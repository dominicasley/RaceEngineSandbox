module;

#include <expected>
#include <string>
#include <utility>

#include <glm/glm.hpp>

export module osr.game:RenderRig;

import raceengine;

namespace osr
{

// Everything a scene needs the renderer to be doing, built once and identically for every scene.
//
// It exists because there are two of them. The rendering gate and the driving gate are two scenes
// so that physics can move one and not the other — but they are only two *gates* while they remain
// one renderer, and the way a shared setup written twice stops being shared is that somebody tunes
// one of the copies. Every pass, every shader, every post-process stage and the tone curve are
// stated here, so a scene can differ in what it contains and cannot differ in how it is drawn.
//
// What a scene still owns: its content, its probes, its camera, and the two metering numbers that
// are genuinely properties of the light in it — the exposure the adaptation starts from and the
// film speed that keeps the meter off its shutter clamp.

// The level is the bottom of this stack: nothing above it can carry on without the camera,
// the shaders or the sky it is asking for, so a reported failure becomes the exception that
// stops the process with the engine's own message attached.
export template <typename T> T orThrow(std::expected<T, std::string> result)
{
    if (!result)
    {
        raceengine::fail(result.error());
    }

    return std::move(result).value();
}

// One of the shaders the rig registered above, by the name it was registered under.
//
// `getShaderByName` answers with an optional because for the engine a lookup miss is not a failure —
// the caller asked whether a shader exists and the answer is no. For a level it is a failure: the
// rig either built the shader this scene names or the scene cannot be drawn, and the two names have
// to agree across two files. The sentence saying which one was missed is worth more than a bad
// optional access at the point of use, which is a terminate with no name in it.
export [[nodiscard]] raceengine::Resource<raceengine::Shader> shaderNamed(raceengine::Engine& engine,
                                                                          const std::string& name);

// The sky, which is the one thing the rig hands back: it follows the camera, so the scene has to put
// it back on every tick.
//
// `skyDistance` is how far out the sky box stands, in world units, and it is the scene's number
// because it is a fact about the scene's size and not about how anything is drawn: the box is real
// geometry that writes depth, so everything visible must sit inside it, and a 60 m apron and a 6 km
// circuit have no distance in common. A scene that raises it must raise its camera's far plane past
// `skyDistance * sqrt(3)` — the box's corners — and its probes' far clip with it, or the sky falls
// out of the frustum and the probes photograph the void.
export [[nodiscard]] RenderableModel& buildRenderRig(raceengine::Engine& engine, Scene& scene, Camera& camera,
                                                     float skyDistance = 2500.0f);

} // namespace osr

namespace osr
{

raceengine::Resource<raceengine::Shader> shaderNamed(raceengine::Engine& engine, const std::string& name)
{
    const auto shader = engine.shader().getShaderByName(name);
    if (!shader)
    {
        raceengine::fail("the render rig registered no shader called '" + name + "'");
    }

    return shader.value();
}

RenderableModel& buildRenderRig(raceengine::Engine& engine, Scene& scene, Camera& camera, const float skyDistance)
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
                            // Zero, and deliberately. Ambient used to be a floor under the diffuse
                            // term — a flat grey added everywhere, which is what a scene says when
                            // it has no indirect light and has to fake one. The light probes are
                            // the indirect light now, and a floor under them would put light into
                            // exactly the shadowed places they were added to keep dark.
                            .ambient = glm::vec3(0.0f),
                            .attenuation = 1.0f};

    // How dark the dark parts of the picture are, which is a different question from how much light
    // there is. It is here and not in a scene because it is the look rather than the light: two
    // scenes printing through different curves would be two renderers, and a change to it is meant
    // to move both gates.
    //
    // Contrast is 1.0 and not above it. It multiplies log radiance about middle grey *before* the
    // filmic curve, and Narkowicz's fit reaches exactly 1.0 at an input of 7.24, so anything above
    // one drags the print-white ceiling down and flattens the sky against it. The shoulder is not
    // the lever it looks like either — it is pow(mapped, 1 + shoulder * mapped), and pow(1, anything)
    // is 1, so it darkens what is *near* white and cannot touch what is already at it.
    engine.camera().setToneCurve(camera, ToneCurve{.contrast = 1.0f, .toe = 0.12f, .shoulder = 0.1f});

    engine.camera().setRoll(camera, 0, 1, 0);

    auto loaded = awaitAll(engine.resource().loadTextFileAsync("assets/Shaders/PresentToScreenVertexShader.glsl"),
                           engine.resource().loadTextFileAsync("assets/Shaders/PresentToScreenFragmentShader.glsl"),
                           engine.resource().loadTextFileAsync("assets/Shaders/PassThroughVertexShader.glsl"),
                           engine.resource().loadTextFileAsync("assets/Shaders/PbrFragmentShader.glsl"),
                           engine.resource().loadTextFileAsync("assets/Shaders/BlinnPhongFragmentShader.glsl"),
                           engine.resource().loadTextFileAsync("assets/Shaders/DepthOnlyVertexShader.glsl"),
                           engine.resource().loadTextFileAsync("assets/Shaders/DepthOnlyFragmentShader.glsl"),
                           engine.resource().loadTextFileAsync("assets/Shaders/ColourFragmentShader.glsl"),
                           engine.resource().loadTextFileAsync("assets/Shaders/HdrVertexShader.glsl"),
                           engine.resource().loadTextFileAsync("assets/Shaders/HdrFragmentShader.glsl"),
                           engine.resource().loadTextFileAsync("assets/Shaders/LuminanceFragmentShader.glsl"),
                           engine.resource().loadTextFileAsync("assets/Shaders/PrepassVertexShader.glsl"),
                           engine.resource().loadTextFileAsync("assets/Shaders/PrepassFragmentShader.glsl"),
                           engine.resource().loadTextFileAsync("assets/Shaders/GtaoFragmentShader.glsl"),
                           engine.resource().loadTextFileAsync("assets/Shaders/AoBlurFragmentShader.glsl"),
                           engine.resource().loadTextFileAsync("assets/Shaders/BloomDownsampleFragmentShader.glsl"),
                           engine.resource().loadTextFileAsync("assets/Shaders/BloomUpsampleFragmentShader.glsl"),
                           engine.resource().loadTextFileAsync("assets/Luts/CinematicGrade.cube"),
                           engine.resource().loadModelAsync("assets/Models/SkyBox/SkyBox.glb"),
                           engine.resource().loadTextFileAsync("assets/Shaders/SkyboxVertexShader.glsl"),
                           engine.resource().loadTextFileAsync("assets/Shaders/SkyboxFragmentShader.glsl"),
                           engine.resource().loadTextureAsync("assets/Textures/Skies/Field/pz.hdr"),
                           engine.resource().loadTextureAsync("assets/Textures/Skies/Field/nz.hdr"),
                           engine.resource().loadTextureAsync("assets/Textures/Skies/Field/nx.hdr"),
                           engine.resource().loadTextureAsync("assets/Textures/Skies/Field/px.hdr"),
                           engine.resource().loadTextureAsync("assets/Textures/Skies/Field/py.hdr"),
                           engine.resource().loadTextureAsync("assets/Textures/Skies/Field/ny.hdr"));

    if (!loaded)
    {
        raceengine::fail(loaded.error());
    }

    auto [presentationVert, presentationFrag, vert, pbrFragmentShader, blinnPhongFragmentShader, depthVertexShader,
          depthFragmentShader, colourFragmentShader, hdrVertexShader, hdrFragmentShader, luminanceFragmentShader,
          prepassVertexShader, prepassFragmentShader, gtaoFragmentShader, aoBlurFragmentShader,
          bloomDownsampleFragmentShader, bloomUpsampleFragmentShader, moodyFilmGrade, skyboxModel, skyboxVertexShader,
          skyboxFragmentShader, front, back, left, right, top, bottom] = std::move(loaded).value();

    auto presentationShader = orThrow(engine.shader().createShader(
        "present", ShaderDescriptor{.vertexShaderSource = presentationVert, .fragmentShaderSource = presentationFrag}));

    orThrow(engine.shader().createShader(
        "pbr", ShaderDescriptor{.vertexShaderSource = vert, .fragmentShaderSource = pbrFragmentShader}));

    // The same vertex stage as "pbr" and a different reflectance model behind it, for content that
    // was authored against the classic one — an imported circuit, whose materials state an ambient,
    // a diffuse, a specular and an exponent and were tuned by somebody looking at exactly those
    // four numbers. Registered here rather than in the scene that uses it because every shader is
    // stated here: a scene may differ in what it contains and may not differ in how it is drawn.
    orThrow(engine.shader().createShader(
        "blinn-phong", ShaderDescriptor{.vertexShaderSource = vert, .fragmentShaderSource = blinnPhongFragmentShader}));

    // The cascades' depth pass. Position through the light's matrix, nothing written: the target
    // has no colour attachment for a fragment output to reach.
    auto depthShader =
        orThrow(engine.shader().createShader("depth", ShaderDescriptor{.vertexShaderSource = depthVertexShader,
                                                                       .fragmentShaderSource = depthFragmentShader}));

    orThrow(engine.shader().createShader(
        "colour", ShaderDescriptor{.vertexShaderSource = vert, .fragmentShaderSource = colourFragmentShader}));

    auto skyboxShader =
        orThrow(engine.shader().createShader("skybox", ShaderDescriptor{.vertexShaderSource = skyboxVertexShader,
                                                                        .fragmentShaderSource = skyboxFragmentShader}));

    auto hdrShader = orThrow(engine.shader().createShader(
        "hdr", ShaderDescriptor{.vertexShaderSource = hdrVertexShader, .fragmentShaderSource = hdrFragmentShader}));

    // The meter's reduction runs through the same fullscreen vertex stage the tone map does; what
    // differs is entirely in the fragment stage, which is told which level of the chain it is.
    auto luminanceShader = orThrow(
        engine.shader().createShader("luminance", ShaderDescriptor{.vertexShaderSource = hdrVertexShader,
                                                                   .fragmentShaderSource = luminanceFragmentShader}));

    // The occlusion prepass's pair, and the two fullscreen stages that turn what it draws into one
    // visibility term per pixel. The prepass has a vertex stage of its own for the reason the
    // cascades' depth pass does: it writes two varyings where the shading pair writes ten.
    auto prepassShader = orThrow(engine.shader().createShader(
        "occlusion prepass",
        ShaderDescriptor{.vertexShaderSource = prepassVertexShader, .fragmentShaderSource = prepassFragmentShader}));

    auto gtaoShader = orThrow(engine.shader().createShader(
        "gtao", ShaderDescriptor{.vertexShaderSource = hdrVertexShader, .fragmentShaderSource = gtaoFragmentShader}));

    auto aoBlurShader = orThrow(
        engine.shader().createShader("ao blur", ShaderDescriptor{.vertexShaderSource = hdrVertexShader,
                                                                 .fragmentShaderSource = aoBlurFragmentShader}));

    // The bloom chain's two stages, both through the same fullscreen vertex stage as everything else
    // in the post chain.
    auto bloomDownsampleShader = orThrow(engine.shader().createShader(
        "bloom downsample", ShaderDescriptor{.vertexShaderSource = hdrVertexShader,
                                             .fragmentShaderSource = bloomDownsampleFragmentShader}));

    auto bloomUpsampleShader = orThrow(engine.shader().createShader(
        "bloom upsample",
        ShaderDescriptor{.vertexShaderSource = hdrVertexShader, .fragmentShaderSource = bloomUpsampleFragmentShader}));

    scene.environment = orThrow(engine.cubeMap().create("sky", front, back, left, right, top, bottom));

    // Ambient occlusion, which is the light a probe hands to a surface that cannot actually see the
    // sky it photographed. A probe is one point: it says how bright the world is where it stands,
    // and nothing in it knows that the crease where a wall meets the ground sees a fifth of that.
    // This is what knows.
    //
    // Forty units of radius is four metres: contact darkening under a car, in the angle between two
    // walls and along a kerb's lip, and not a general dimming. Strength 1.4 is the punchy reading of
    // an integral that is already correct — the visibility term is what it is, and a power on it
    // deepens the crease without touching the open ground.
    orThrow(engine.ambientOcclusion().enable(
        camera, raceengine::CreateAmbientOcclusionDTO{
                    .prepassShader = prepassShader,
                    .gatherShader = gtaoShader,
                    .blurShader = aoBlurShader,
                    .occlusion = raceengine::AmbientOcclusion{.strength = 1.4f, .radius = 40.0f}}));

    // Auto exposure, ahead of the tone map in the camera's chain because what it measures is the
    // radiance the tone map is about to consume.
    //
    // Compensation and weighting are not the same dial. The compensation says how far a frame drawn
    // by this rig sits from a photometric reading — the meter's own answer puts the geometric mean
    // at middle grey, and both of these scenes are a subject standing under a sky three orders of
    // magnitude brighter than it, so the neutral answer is a correctly exposed sky and an
    // unreadable subject. The weighting says which part of the frame gets to decide, and answers
    // differently every time the camera turns.
    orThrow(engine.autoExposure().enable(
        camera, raceengine::CreateAutoExposureDTO{
                    .shader = luminanceShader,
                    .meter = raceengine::AutoExposure{.compensation = 1.50f, .centreWeighting = 1.00f}}));

    // Bloom, ahead of the tone map in the camera's chain for the same reason the meter is: what it
    // produces is consumed by the pass that follows it.
    //
    // A threshold of 2.0 in exposed radiance is about eleven times middle grey: the sun, the neon
    // and the specular off the car, and *not* the sky. Thresholding happens in the *exposed* domain,
    // which is what makes that number survive a change of scene at all — the meter moves the shutter
    // under it and the amount of spill stays put.
    orThrow(engine.bloom().enable(
        camera, raceengine::CreateBloomDTO{
                    .downsampleShader = bloomDownsampleShader,
                    .upsampleShader = bloomUpsampleShader,
                    .bloom = raceengine::Bloom{
                        .threshold = 2.0f, .knee = 0.6f, .intensity = 0.30f, .maximum = 20.0f, .spread = 2.2f}}));

    auto hdr = orThrow(engine.postProcess().create("hdr", hdrShader));

    for (auto& attachment : engine.camera().getOutputBuffer(camera).attachments)
    {
        engine.postProcess().addInput(hdr, attachment);
    }

    // The third input, after the camera's colour and depth: the top of the bloom chain, which the
    // tone map adds in after exposure and before the curve.
    engine.postProcess().addInput(hdr, camera.bloom.result);
    engine.postProcess().setParameters(hdr, glm::vec4(camera.bloom.intensity, 0.0f, 0.0f, 0.0f));

    engine.camera().addPostProcess(camera, hdr);

    auto hdrPostProcess = engine.memoryStorage().postProcesses.get(hdr);

    auto outputAttachment = engine.fbo().getAttachmentsOfType(
        engine.memoryStorage().frameBuffers.get(hdrPostProcess.output.value()), FboAttachmentType::Color);

    // The lens and the grade. The lens is geometry and stays a number: the aberration is in
    // *texture* coordinates, so it is a fraction of the screen and not of a pixel — 0.004 is about
    // three pixels of separation in the corner of a 1920-wide frame and none at all in the middle.
    //
    // The grade is a file, and swapping that file swaps the look: no rebuild, no engine change, and
    // the same table the colourist was looking at when they saved it. Two formats are read — a
    // `.cube` from a grading tool, and the N-slices-of-N-squared strip the Unreal ecosystem has
    // published for a decade. This one is a `.cube` straight out of Photoshop's export plugin,
    // 32 entries a side, which the shader reads off `textureSize` rather than being told.
    auto grade = orThrow(engine.colourGrade().load("moody film", moodyFilmGrade));

    engine.presenter().setPresenter(Presenter{.output = outputAttachment.front(),
                                              .shader = presentationShader,
                                              .parameters = glm::vec4(0.004f, 1.0f, 0.0f, 0.0f),
                                              .lookupTable = grade});

    // Four depth-only orthographic cameras appended to this scene, refitted to the camera's frustum
    // every frame — so the cascades are spread over the *view* and not over the world, and a 6.2 km
    // circuit costs them nothing that a 60 m apron did not.
    //
    // Lambda 0.9, and the number is where the texels go. At 0.5 — the generic advice for a view that
    // is mostly distance — the uniform arm dominates the first split and cascade 0 ran out to 26 m,
    // which put its texels at 3 cm and made every shadow within arm's reach soft: an A-pillar's
    // shadow on the dashboard, half a metre from the eye, was a 10 cm smear once the 3-texel normal
    // offset and the PCF sat on top. At 0.9 the first split lands near 5.6 m and a cascade-0 texel
    // is 6.5 mm, which is what makes a shadow cast *into the car* read as an edge. The price is paid
    // at the far end, where the last cascade coarsens to ~12 cm texels beyond 40 m — softness that
    // distance and the PCF were already hiding.
    orThrow(engine.shadow().enable(scene, sun, camera,
                                   raceengine::CreateShadowCascadesDTO{.depthShader = depthShader,
                                                                       .resolution = 4096,
                                                                       .lambda = 0.9f,
                                                                       .distance = 2000.0f,
                                                                       .casterExtent = 1500.0f}));

    auto& skyEntity = engine.scene().createEntity(
        scene, CreateRenderableModelDTO{
                   .node = engine.sceneManager().createNode(scene), .shader = skyboxShader, .model = skyboxModel});

    engine.sceneManager().setScale(skyEntity.node, skyDistance, skyDistance, skyDistance);
    // The sky is not a caster. A 2500-unit box in the depth map fills every cascade at its near
    // plane, and the whole world is then in its shadow.
    skyEntity.castsShadow = false;

    return skyEntity;
}

} // namespace osr
