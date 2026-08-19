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
import :PlayerCar;
import :RaceTrack;
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
    // Mount Panorama, once: the loaded model, and the physics world whose collision mesh was read
    // straight out of it. Both are built in the initialiser list and in this order, because the
    // renderer frees a mesh buffer's bytes when it uploads them and the collision mesh has to be
    // taken while they are still there. 615,197 triangles of BVH, so a tick that rebuilt it would be
    // a tick that did nothing else. Declared before everything that queries them, which is the same
    // rule the engine's own member list runs on.
    raceengine::Resource<raceengine::Model> trackModel;
    raceengine::PhysicsWorld track;

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
    trackModel(orThrow(engine.resource().loadModelAsync(std::string(trackAsset)).get())),
    track(orThrow(raceengine::PhysicsWorld::create(orThrow(trackCollisionMesh(engine.memoryStorage(), trackModel)))))
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

    // Faster film, and it is not a look — it is what stops the meter running out of shutter.
    //
    // Film speed and aperture cancel out of the exposure multiplier, so two cameras metering the
    // same scene agree on the picture and disagree only on the shutter it took. The one place they
    // change anything is at the clamp, and this scene reaches it: a circuit of 0.07-albedo asphalt
    // under an open sky meters 2.6 stops darker than the sunlit apron this level used to be, and on
    // ISO 6400 the meter asked for 1/1.5 s against a `maxShutterTime` of 1/4 and sat pinned there,
    // 1.42 stops under its own reading, with every further scene change moving nothing at all. Four
    // times the film speed puts the answer at 1/6 s, back inside the range, and the meter is a meter
    // again.
    engine.camera().setFilmSpeed(camera, 25600);

    // The scene's radiance is relative — a sun of 3.2 and an asphalt albedo of 0.07 — so this is the
    // number that turns it into a picture, and it is where the adaptation starts and what the camera
    // holds until the first reading comes back. Measured rather than guessed: the chase view settles
    // at 19.2 and the fixed one at 3.1, because the two see wildly different amounts of sky, and
    // seeding anywhere near the apron's old 2.5 leaves a 120-frame capture still climbing.
    engine.camera().setExposure(camera, 18.0f);
    // How dark the dark parts of the picture are, which is a different question from how much light
    // there is. Carried over from the apron unchanged, and the reasoning that fixed it there is what
    // says not to reach for it here: contrast multiplies log radiance about middle grey *before* the
    // filmic curve, and Narkowicz's fit reaches exactly 1.0 at an input of 7.24, so anything above
    // one drags the print-white ceiling down and flattens the sky against it. The shoulder is not
    // the lever it looks like either — it is pow(mapped, 1 + shoulder * mapped), and pow(1, anything)
    // is 1, so it darkens what is *near* white and cannot touch what is already at it.
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
        // The rendering gate's framing, and it deliberately has no car in it.
        //
        // The gate exists to be moved by the renderer and by nothing else. On the apron that was
        // arranged by making the car small and distant, which left it moved by physics a little; on
        // a circuit the car is either the subject or it is absent, and absent is what the split asks
        // for now that the driving gate covers the car end to end.
        //
        // So it looks at Hell Corner from above and behind, which is the one place on this asset
        // where every surface it carries is in one frame: road, the edge either side of it, painted
        // kerb — the only saturated colour a circuit has, and the one thing the grade acts on
        // visibly — a gravel trap, grass, and the concrete wall, which is the only vertical surface
        // and therefore the only thing that can cast a shadow onto the road or give the occlusion
        // term a crease to find. Two hundred metres of pit straight run away behind it, which is
        // what says whether the cascades still reach.
        //
        // It is a hundred and eighty metres from the grid, and that distance is bounded rather than
        // free: the skybox follows the camera at 2500 units and the scene's one light probe has to
        // be inside it from *both* cameras, so the probe sits between them.
        const auto stand = toWorldUnits(glm::dvec3(-60.0, 55.0, -545.0));
        engine.camera().setPosition(camera, static_cast<float>(stand.x), static_cast<float>(stand.y),
                                    static_cast<float>(stand.z));
        freeCamera.emplace(engine, -2.1376, -0.2549);
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

    // Ambient occlusion, which is the light the one probe hands to a surface that cannot actually
    // see the sky it photographed. A probe is one point: it says how bright the world is over the
    // start line, and nothing in it knows that the gutter between the road and the wall sees a fifth
    // of that. This is what knows.
    //
    // Radius and strength are the apron's numbers carried over untuned. Four metres of radius is the
    // right order for what is left to occlude here — the crease where the wall meets the road, the
    // kerb's own lip, and the contact under the car — but it was chosen against a building, and
    // nothing on this circuit has been measured against it.
    orThrow(engine.ambientOcclusion().enable(
        camera, raceengine::CreateAmbientOcclusionDTO{
                    .prepassShader = prepassShader,
                    .gatherShader = gtaoShader,
                    .blurShader = aoBlurShader,
                    .occlusion = raceengine::AmbientOcclusion{.strength = 1.4f, .radius = 40.0f}}));

    // Auto exposure, ahead of the tone map in the camera's chain because what it measures is the
    // radiance the tone map is about to consume.
    //
    // The dial is the level's because the exchange rate between this scene's relative radiance and a
    // photometric meter's cd/m² is the level's: a sun of 3.2 is a number chosen here.
    //
    // Fully centre weighted, and on a circuit that is worth more than it was on the apron rather
    // than less. The shot a flat average is worst at is the one with the subject low in the frame
    // and open sky above it, which is every frame of a driving game — and here the ground is
    // 0.07-albedo asphalt against a sky three orders of magnitude brighter, so a flat geometric mean
    // answers almost entirely to how much sky the camera happens to be pointing at. Measured on the
    // two gate views, which see very different amounts of it: the chase view settles at exposure
    // 19.2 and the fixed one at 3.1, two and a half stops apart in the same world.
    //
    // Compensation and weighting are not the same dial — the compensation says how far this whole
    // scene sits from a photometric reading and is fixed per level, the weighting says which part of
    // the frame gets to decide and answers differently every time the camera turns.
    orThrow(engine.autoExposure().enable(
        camera, raceengine::CreateAutoExposureDTO{
                    .shader = luminanceShader,
                    .meter = raceengine::AutoExposure{.compensation = 1.50f, .centreWeighting = 1.00f}}));

    // Bloom, ahead of the tone map in the camera's chain for the same reason the meter is: what it
    // produces is consumed by the pass that follows it.
    //
    // A threshold of 2.0 in exposed radiance is about eleven times middle grey: the sun and the
    // specular off the car, and *not* the sky.
    //
    // Thresholding happens in the *exposed* domain, which is what makes that number survive a change
    // of scene at all — the meter moves the shutter under it and the amount of spill stays put. It
    // was 1.0 on the apron, which the sky clears easily, so the largest bright thing in the frame
    // was blooming onto itself; a spill says "brighter than the display can be" by putting light
    // where the source is *not*, and a sky spilling over its own area is a haze.
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

    // Four depth-only orthographic cameras appended to this scene, refitted to the camera's frustum
    // every frame — so the cascades are spread over the *view* and not over the world, and a 6.2 km
    // circuit costs them nothing that a 60 m apron did not.
    //
    // 2000 units is 200 m, which is what the distance has to be measured against here: the skybox
    // follows the camera at 2500 units, so 250 m is as far as anything can be seen at all. Shadows
    // therefore reach four fifths of the visible depth, and the band behind that draws unshadowed.
    // Measured at this field of view and 2048 square, the cascade texel runs 39 mm at the near end
    // to 306 mm at 200 m. The caster extent is 150 m along the light against a tallest local caster
    // — the pit wall — of about 16 m, so it is generous rather than tight; it was sized for a
    // building and there is nothing here that needs it.
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

    // The circuit itself, drawn from the model the collision mesh above was read out of. There is
    // no position to state: the track carries its own world coordinates and `trackOrigin` is zero,
    // so the only thing between the two is the tenth of a metre a world unit is. Anything else here
    // and the surface being driven on would be somewhere the surface being drawn is not, which reads
    // as the car floating or sinking rather than as a transform error.
    auto& trackEntity = engine.scene().createEntity(
        scene, CreateRenderableModelDTO{.node = engine.sceneManager().createNode(scene),
                                        .shader = engine.shader().getShaderByName("pbr").value(),
                                        .model = trackModel});

    const auto placement = toWorldUnits(glm::dvec3(0.0));
    engine.sceneManager().setPosition(trackEntity.node, static_cast<float>(placement.x),
                                      static_cast<float>(placement.y), static_cast<float>(placement.z));
    engine.sceneManager().setScale(trackEntity.node, static_cast<float>(worldUnitsPerMetre),
                                   static_cast<float>(worldUnitsPerMetre), static_cast<float>(worldUnitsPerMetre));

    // The 60 m ground plane that used to stand in for the world is gone. It was three times the
    // width of the strip it covered and is now a twentieth of the length of the one the car can
    // reach, so what it drew was an edge to drive off; and it would have had to be laid *on* the
    // circuit, where two surfaces a few millimetres apart is a z-fight rather than a ground.

    // The apron's building and its bollard, kept and not placed. They stand at the world origin,
    // which on this circuit is six hundred metres from the grid across a gap in the ribbon — past
    // the camera's five-hundred-metre far plane and well past the skybox — so nothing sees them from
    // anywhere a car can reach. They are here because `scripts/smoke.sh` asserts that `test.glb` and
    // `bollard.glb` are processed, and dropping them from the scene means changing the gate in the
    // same commit.
    DinosaurEntity(engine, scene);
    Bollard(engine, scene);

    car.emplace(engine, scene);

    // The first authored grid slot, position and heading both. Not the AI line: its first point is a
    // racing line a metre from the right-hand edge of an eleven-metre road, and its height is the
    // recording car's own reference height rather than the tarmac — three quarters of a metre of
    // thin air. A start box states a heading, which is the other thing an AI line cannot.
    const auto& slot = gridSlots.front();
    player.emplace(engine, track, car->sceneNode(), slot.position, glm::radians(slot.yaw));

    // The image-based lighting graph, and on an open circuit it is one node rather than three.
    //
    // The two local ones straddled the building: one stood in its shadow so that what it recorded
    // *was* the wall rather than the sky, and the other covered the apron in front so a surface
    // crossing out of the shadow crossed a band where both contributed. Neither has anything to
    // stand for here. A circuit is open ground under open sky and its indirect light is the sky,
    // the tarmac and the hillside — which is what one probe over the start line records.
    //
    // Where the local probes will come back is where a circuit genuinely occludes the sky: under
    // the trees at the Dipper, in the cutting, and along the pit wall. That is a probe *per place*,
    // and the constraint that decides how it has to be built is this: the skybox follows the camera
    // at 2500 units, and a probe outside it records the box's far wall instead of the sky. Fixed
    // probes 6 km apart cannot all be inside one 250 m box, so the answer is probes near the car
    // rather than probes everywhere — a different feature from this one.
    // Thirty metres over the pit straight and midway between the two gate cameras, ninety-odd metres
    // from each — which is what the 250 m skybox allows and the reason it is not simply over the
    // grid.
    const auto overTheStraight = toWorldUnits(glm::dvec3(30.0, 65.0, -565.0));
    static_cast<void>(engine.lightProbe().createProbe(
        scene, raceengine::CreateLightProbeDTO{.name = "sky",
                                               .position = glm::vec3(static_cast<float>(overTheStraight.x),
                                                                     static_cast<float>(overTheStraight.y),
                                                                     static_cast<float>(overTheStraight.z)),
                                               .global = true,
                                               .nearClippingPlane = 5.0f,
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
