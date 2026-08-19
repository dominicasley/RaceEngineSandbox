module;

#include <cstdlib>
#include <expected>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include <glm/glm.hpp>

export module osr.game:WaterLevel;

import :Bollard;
import :CarEntity;
import :ChaseCameraController;
import :DinosaurEntity;
import :FPSCameraController;
import :GroundPlane;
import :PlayerCar;
import :TrackFrame;

import raceengine;

namespace osr
{

export class WaterLevel
{
private:
    raceengine::Engine& engine;
    Scene& scene;
    Camera& camera;
    // The track, generated rather than authored, and built once: it is a BVH over the quarter of a
    // million triangles a 200 x 40 m ground comes to at a quarter-metre pitch, so a tick that
    // rebuilt it would be a tick that did nothing else. Declared before everything that queries it,
    // which is the same rule the engine's own member list runs on.
    raceengine::ProvingGroundDescriptor track;
    raceengine::PhysicsWorld provingGround;

    // Exactly one of these is engaged, chosen in the constructor. Both write the camera's direction
    // on every tick, so a scene holding two live controllers would have one of them silently
    // overwritten before the first frame.
    std::optional<ChaseCameraController> chaseCamera;
    std::optional<FPSCameraController> freeCamera;

    RenderableModel* sky;
    // Built in the body rather than the initialiser list because both need the "pbr" shader, which
    // is created down there out of a file this level awaits.
    std::optional<CarEntity> car;
    std::optional<PlayerCar> player;

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

enum class CameraChoice
{
    Chase,
    Fixed
};

// Which camera the scene is watched from, off `OSR_CAMERA` and read once.
//
// It is an environment variable rather than a compile-time choice because the two frame gates need
// one run each and a gate that has to be rebuilt before it can be run is a gate nobody runs. An
// unrecognised value is refused rather than quietly defaulted: a run that fell back would compare
// one camera's capture against the other camera's golden frame and report the difference as a
// rendering change.
[[nodiscard]] CameraChoice cameraChoice()
{
    const auto* requested = std::getenv("OSR_CAMERA");
    if (requested == nullptr)
    {
        return CameraChoice::Chase;
    }

    const auto value = std::string(requested);
    if (value == "chase")
    {
        return CameraChoice::Chase;
    }

    if (value == "fixed")
    {
        return CameraChoice::Fixed;
    }

    throw std::runtime_error("OSR_CAMERA names a camera this game does not have: '" + value +
                             "'. It takes 'chase' or 'fixed'.");
}

} // namespace

WaterLevel::WaterLevel(raceengine::Engine& engine) :
    engine(engine),
    scene(engine.sceneManager().createScene()),
    camera(orThrow(engine.scene().createCamera(scene))),
    track(raceengine::defaultProvingGround()),
    provingGround(orThrow(raceengine::PhysicsWorld::create(orThrow(raceengine::generateProvingGround(track)))))
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
                            // it has no indirect light and has to fake one. The light probes below
                            // are the indirect light now, and a floor under them would put light
                            // into exactly the shadowed places they were added to keep dark.
                            .ambient = glm::vec3(0.0f),
                            .attenuation = 1.0f};

    // The scene's radiance is relative — a sun of 3.2 and an asphalt albedo of about 0.1 — so this
    // is the number that turns it into a picture. It is no longer the last word on it: the camera
    // meters the frame it drew and moves its own shutter (see below), and this is where that
    // adaptation starts from and what it holds until the first reading comes back. Hand-tuned, and
    // still the reference the meter is judged against: at one, which is what "no exposure" means,
    // sunlit asphalt lands at a twentieth of the tone curve and every shadow crushes to black;
    // past about five it blows out the sky.
    engine.camera().setExposure(camera, 2.5f);
    // How dark the dark parts of the picture are, which is a different question from how much light
    // there is. The toe grips the bottom of the output range: 0.35 was the punchiest reading of a
    // frame whose shadows were still meant to hold detail, and 0.12 opens them back up. Measured on
    // this frame it moves the shadowed facade by about two values out of 255 — which is the useful
    // finding, because it says the facade is not being *crushed* by the curve, there is simply
    // almost no light on it. The lever that mattered was the compensation below.
    //
    // Contrast is 1.0, down from 1.1, and that is the whole of what was wrong with the sky. It
    // multiplies log radiance about middle grey *before* the filmic curve, and Narkowicz's fit
    // reaches exactly 1.0 at an input of 7.24 — so 1.1 dragged the print-white ceiling down from an
    // exposed radiance of 7.24 to 5.17 and pinned everything above it at the same output value.
    // Half the sky was landing there with no gradient left in it. At 1.0 that is 39% instead of 54%
    // and the sky uses 24 values of output range instead of 17, and the shadowed facade came out
    // *brighter* by a value rather than paying for it — because a contrast above one pushes away
    // from the pivot in both directions, and the facade is a long way below it.
    //
    // The shoulder is not the lever it looks like: it is pow(mapped, 1 + shoulder * mapped), and
    // pow(1, anything) is 1, so it darkens what is near white and cannot touch what is already at
    // it. Measured, 0.30 bought the same sky as contrast 0.95 and cost two values off the facade.
    engine.camera().setToneCurve(camera, ToneCurve{.contrast = 1.0f, .toe = 0.12f, .shoulder = 0.1f});

    engine.camera().setRoll(camera, 0, 1, 0);

    // Never lookAtPoint: whichever controller is engaged below writes the direction on every tick,
    // so a framing stated here would be overwritten before the first frame — which is precisely
    // what happened to the aerial spawn view this scene used to have, for long enough to be worth a
    // note in the engine's own documentation.
    if (cameraChoice() == CameraChoice::Chase)
    {
        // The game as it is played, and the view the driving gate captures: it writes the position
        // too, so there is none to state.
        chaseCamera.emplace(engine);
    }
    else
    {
        // The rendering gate's framing, and it stands on the apron rather than looking down on it.
        // The ground-to-wall crease, the recesses in the facade and the contact under the car are
        // where the indirect light does its work, and from six hundred units up they are a handful
        // of pixels each. Yaw a little off the building's axis, pitch just above the horizon.
        engine.camera().setPosition(camera, 270, 32, 250);
        freeCamera.emplace(engine, -2.356, 0.110);
    }

    auto loaded = awaitAll(engine.resource().loadTextFileAsync("assets/Shaders/PresentToScreenVertexShader.glsl"),
                           engine.resource().loadTextFileAsync("assets/Shaders/PresentToScreenFragmentShader.glsl"),
                           engine.resource().loadTextFileAsync("assets/Shaders/PassThroughVertexShader.glsl"),
                           engine.resource().loadTextFileAsync("assets/Shaders/PbrFragmentShader.glsl"),
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
                           engine.resource().loadTextFileAsync("assets/Luts/MoodyFilm.cube"),
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
        throw std::runtime_error(loaded.error());
    }

    auto [presentationVert, presentationFrag, vert, pbrFragmentShader, depthVertexShader, depthFragmentShader,
          colourFragmentShader, hdrVertexShader, hdrFragmentShader, luminanceFragmentShader, prepassVertexShader,
          prepassFragmentShader, gtaoFragmentShader, aoBlurFragmentShader, bloomDownsampleFragmentShader,
          bloomUpsampleFragmentShader, moodyFilmGrade, skyboxModel, skyboxVertexShader, skyboxFragmentShader, front,
          back, left, right, top, bottom] = std::move(loaded).value();

    auto presentationShader = orThrow(engine.shader().createShader(
        "present", ShaderDescriptor{.vertexShaderSource = presentationVert, .fragmentShaderSource = presentationFrag}));

    orThrow(engine.shader().createShader(
        "pbr", ShaderDescriptor{.vertexShaderSource = vert, .fragmentShaderSource = pbrFragmentShader}));

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

    // Ambient occlusion, which on this scene is the light the probes hand to a surface that cannot
    // actually see the sky they photographed. A probe is one point and one box: it says how bright
    // the world is near the building, and nothing in it knows that the ground in the angle between
    // two walls sees a fifth of that. This is what knows.
    //
    // Forty units of radius against a building about six hundred across: contact darkening where the
    // walls meet the apron and under the car, and not a general dimming of the scene. Strength 1.4 is
    // the punchy reading of an integral that is already correct — the visibility term is what it is,
    // and a power on it deepens the crease without touching the open ground.
    orThrow(engine.ambientOcclusion().enable(
        camera, raceengine::CreateAmbientOcclusionDTO{
                    .prepassShader = prepassShader,
                    .gatherShader = gtaoShader,
                    .blurShader = aoBlurShader,
                    .occlusion = raceengine::AmbientOcclusion{.strength = 1.4f, .radius = 40.0f}}));

    // Auto exposure, ahead of the tone map in the camera's chain because what it measures is the
    // radiance the tone map is about to consume.
    //
    // The dial is the level's because the exchange rate between this scene's relative radiance and
    // a photometric meter's cd/m² is the level's: a sun of 3.2 is a number chosen here, and the
    // meter's own reading of it — 0.081 average, exposure 1.28 — is not what this street is meant
    // to look like. Three quarters of a stop up lands it at about 2.16, which is deliberately a
    // fifth of a stop *under* the 2.5 set by hand above rather than on it. That direction is the
    // whole point: the frame is a sunlit apron beside a building whose shadow the light probes
    // exist to keep dark, its brightest pixel had about a third of a stop left before the clip, and
    // the shoulder holding it back is only 0.10. Exposing for the highlight and letting the shadow
    // fall is what keeps the roof off the clip and the shadow genuinely dark.
    orThrow(engine.autoExposure().enable(
        camera, raceengine::CreateAutoExposureDTO{
                    .shader = luminanceShader,
                    .meter = raceengine::AutoExposure{.compensation = 1.50f, .centreWeighting = 1.00f}}));
    // 1.50, up from 0.75. The meter's own answer puts the frame's geometric mean at middle grey,
    // and this frame's mean is a skyful of bright sky over a building in shadow — so the neutral
    // answer is a correctly exposed sky and an unreadable building. Three quarters of a stop kept
    // the drama and lost the detail; a stop and a half reads the shadowed facade at about 16/255
    // instead of 3, at the cost of 1.7% of the frame clipping instead of 0.2%.
    //
    // Fully centre weighted, because the shot this scene is composed as is the one a flat average
    // is worst at: an unlit face with the sky behind it, where the subject is a fifth of the frame
    // and the thing setting the exposure is the other four fifths. Compensation and weighting are
    // not the same dial — the compensation above says how far this whole scene sits from a
    // photometric reading and is fixed, and the weighting says which part of the frame gets to
    // decide, which is a different answer every time the camera turns.
    //
    // What it is worth here, measured rather than assumed, and the second number is the point:
    // on the spawn view it opens up by a quarter of a stop (the shaded facade 11.6 → 16.7 of 255,
    // clipping 8.2% → 11.0%, all of it sky), and on a view pitched up until that same unlit face
    // stands against nothing but sky it opens by half a stop. Half a stop is what a centre-weighted
    // average is worth and not a stop more: the sky is still most of the middle of that frame, and
    // a geometric mean of a thousand-to-one range answers to how much of the frame a thing covers.
    // Tightening the falloff towards a spot meter was tried and bought 0.08 of a stop on top, which
    // is not worth the exposure pumping as the crosshair crosses a lit window. The lever that moves
    // an unlit subject further than that is the compensation, and it is deliberately not this one.

    // Bloom, ahead of the tone map in the camera's chain for the same reason the meter is: what it
    // produces is consumed by the pass that follows it.
    //
    // A threshold of 2.0 in exposed radiance is about eleven times middle grey: the sun, the neon
    // and the specular off the car, and *not* the sky.
    //
    // It was 1.0, which is a little over three times middle grey and which the sky clears easily —
    // so the largest bright thing in the frame was blooming onto itself. That is not what bloom is
    // for. A spill says "this is brighter than the display can be" by putting light where the thing
    // is not; a sky that spills over its own area is a haze, and it was pushing radiance that had
    // gradient left in it up over the tone curve's ceiling, where it printed flat. Raising the
    // threshold took the pinned fraction of the sky from 39% to 24% and doubled the output range it
    // uses, at no cost at all to the shadowed facade — which is what says this was the right lever
    // and the exposure was not. 3.0 keeps lowering the pinned fraction without recovering any more
    // gradient, so the knee is here: past this it is only removing bloom.
    //
    // 0.30 is what the intensity settled at, and it stayed: with the sky no longer in the source
    // the spill is the sun and the lights, which is the statement it was meant to be making.
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
    // published for a decade, which is what this one is.
    //
    // `assets/Luts/street.cube` beside it is the look this shader used to hold as constants, baked
    // by scripts/bake-grade.py; `scripts/grade-contact-sheet.py` puts every grade in the folder on
    // one ungraded plate so a look can be chosen by looking rather than by rebuilding.
    //
    // This one is a `.cube` straight out of Photoshop's export plugin — 32 entries a side against
    // the strips' sixteen, which the shader reads off `textureSize` rather than being told.
    auto grade = orThrow(engine.colourGrade().load("moody film", moodyFilmGrade));

    engine.presenter().setPresenter(Presenter{.output = outputAttachment.front(),
                                              .shader = presentationShader,
                                              .parameters = glm::vec4(0.004f, 1.0f, 0.0f, 0.0f),
                                              .lookupTable = grade});

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
    Bollard(engine, scene);

    car.emplace(engine, scene);

    // Ten metres along the track's centreline, which is fifty metres of flat tarmac short of the
    // kerb band and is where the grid slot was chosen to put the car exactly where this scene has
    // always had one. The height is asked of the ground rather than assumed to be zero, because
    // the ground is a function and the answer is free.
    player.emplace(engine, provingGround, car->sceneNode(),
                   glm::dvec3(0.0, raceengine::sampleProvingGround(track, 0.0, 10.0).height, 10.0));

    // The image-based lighting graph. Three nodes, and the point of each is what it can see:
    //
    // The global one stands high over the middle of the plaza with nothing near it, so what it
    // records is very nearly the sky alone. It is the fallback every fragment outside the local
    // volumes falls back on, and it is what keeps the open ground lit.
    //
    // The other two straddle the building. One sits in the shadow along its face, low and close,
    // where more than half of what it can see *is* the building — so its irradiance is the dark,
    // slightly warm bounce off a wall rather than the sky, and the specular chain it prefilters
    // has the wall in it where the sky used to be. That is the whole fix for the highlight that
    // used to survive the shadow: nothing subtracts it, the probe simply never recorded it.
    //
    // The third covers the open apron in front, overlapping the second, so a surface crossing out
    // of the shadow crosses through a band where both contribute and neither switches on.
    //
    // Every probe stays well inside the 2500-unit skybox, which follows the camera: a probe
    // captured from outside it would record the box's far wall instead of the sky.
    static_cast<void>(engine.lightProbe().createProbe(
        scene, raceengine::CreateLightProbeDTO{.name = "sky",
                                               .position = glm::vec3(0.0f, 220.0f, -260.0f),
                                               .global = true,
                                               .nearClippingPlane = 5.0f,
                                               .farClippingPlane = 4000.0f}));

    static_cast<void>(engine.lightProbe().createProbe(
        scene, raceengine::CreateLightProbeDTO{.name = "building shadow",
                                               .position = glm::vec3(0.0f, 45.0f, -120.0f),
                                               // 200 wide, not 320: the shadow this probe stands
                                               // for runs from about x = -180 to x = +150, and a
                                               // box wider than that hands the dark environment it
                                               // recorded to sunlit ground either side of the
                                               // building. A probe's volume is a claim about where
                                               // its photograph is a good answer.
                                               .halfExtents = glm::vec3(200.0f, 90.0f, 70.0f),
                                               .blendDistance = 35.0f,
                                               .nearClippingPlane = 2.0f,
                                               .farClippingPlane = 4000.0f}));

    static_cast<void>(engine.lightProbe().createProbe(
        scene, raceengine::CreateLightProbeDTO{.name = "open apron",
                                               .position = glm::vec3(0.0f, 45.0f, -330.0f),
                                               .halfExtents = glm::vec3(420.0f, 90.0f, 190.0f),
                                               .blendDistance = 60.0f,
                                               .nearClippingPlane = 2.0f,
                                               .farClippingPlane = 4000.0f}));

    // Registered last, once the level is fully built: the engine may call this the moment the
    // first tick runs, and a half-constructed level is not something it should be handed.
    engine.onUpdate([this](float delta) { update(delta); });
}

// Writers before readers, inside the stage the engine calls the game's own logic: the car is
// stepped and writes its node, the camera is aimed at where the car ended up, and the sky is put
// back on the camera. Entity behaviours and the scene's own settling both run after this returns.
void WaterLevel::update(float delta)
{
    player->update(delta);

    if (chaseCamera)
    {
        chaseCamera->update(camera, player->vehicle(), player->acceleration(), delta);
    }
    else if (freeCamera)
    {
        freeCamera->update(camera, delta);
    }

    engine.sceneManager().setPosition(sky->node, camera.position.x, camera.position.y, camera.position.z);
}

} // namespace osr
