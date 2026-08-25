module;

#include <algorithm>
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

// What a *scene* has to say about the air it stands in and the glass in front of its camera, where
// the rest of both is the rig's.
//
// The split is the one `skyDistance` already draws: how thick the air is and how it scatters are
// how a frame is drawn and belong to every scene equally, while where the *ground* is is a fact
// about this scene's own geometry — the apron's is y = 0 and Bathurst's pit straight stands at 350
// world units, and a fog layer quoted at the wrong one is either buried or floating.
export struct RigAir
{
    // Where the quoted density is quoted at, in world units. The scene's ground.
    float baseHeight = 0.0f;
    // A multiplier on it, which is `OSR_FOG` and nothing else — the seat knob, so that thicker and
    // thinner are one run apart rather than one rebuild apart.
    float densityScale = 1.0f;
    // The sun's elevation above the horizon in degrees, which is the hour this scene is set at.
    // `OSR_SUN` and nothing else; the rig's own default is the early morning it ships at.
    float sunElevationDegrees = 6.0f;
    // The rain, 0..1-ish, `OSR_RAIN` and nothing else. Zero — unset — is the dry scene and is
    // byte-identical to a renderer with no rain in it; today the windshield shader is its one
    // reader, so this is drops on the glass and not weather in the world.
    float rain = 0.0f;
};

// The meter's dial for any view standing outside a car: the subject is dark against a bright sky,
// and the meter has to be told to open up for it (the fuller account is beside the enable call
// below). Exported because two files have to agree on the number — the rig writes it when metering
// is enabled, and a scene that moved the dial for its cockpit puts this one back the moment the
// view steps outside again.
export constexpr float rigCompensation = 1.50f;

// The scene's layers. The engine only ands a renderable's word against a camera's mask; what each
// bit means is this game's, and it means two things: the world — track, sky, buildings, everything
// that stands still — and the car the player sits in. worldLayer is bit 1 on purpose: it is the
// layer every renderable is born on, so nothing but the car ever states one.
export constexpr unsigned int worldLayer = 1u << 0;
export constexpr unsigned int carLayer = 1u << 1;

// What the rig hands back. The sky follows the camera, so the scene has to put it back on every
// tick; the two extra cameras are the layered frame's, and the scene has to keep their pose in step
// with the one it steers (syncLayeredCameras) — three cameras are one eye, and the engine holds no
// opinion about which of them leads. `composite` is the join between the layers, kept because its
// one parameter is the world's exposure ratio and the exposure policy below writes it.
export struct RigBuild
{
    RenderableModel* sky;
    Camera* carCamera;
    Camera* frameCamera;
    raceengine::Resource<raceengine::PostProcess> composite;
};

export [[nodiscard]] RigBuild buildRenderRig(raceengine::Engine& engine, Scene& scene, Camera& camera,
                                             float skyDistance = 2500.0f, RigAir air = RigAir{});

// The three cameras are one eye. Every field the projection and the view matrix are built from is
// copied verbatim, so the matrices — and with them the culling, the blended sort keys and the
// composite's pixel alignment — come out bit-identical across the three views. Called at the end of
// every scene tick, after whatever controller steered the pose camera.
export void syncLayeredCameras(const Camera& pose, Camera& carCamera, Camera& frameCamera);

// One meter for the whole frame: the frame camera meters the composited image exactly as the single
// camera always did, the layer meters go quiet, and the composite ratio is one — which is the
// configuration both parity gates run under and is byte-identical to the un-layered renderer.
export void linkExposure(raceengine::Engine& engine, Camera& worldCamera, Camera& carCamera, Camera& frameCamera,
                         const raceengine::Resource<raceengine::PostProcess>& composite);

// Split metering, which is what the layered frame exists for: the world meters its own buffer, the
// cabin meters its own pixels (coverage-weighted, so the world showing through the windscreen does
// not vote), and the frame camera's meter goes quiet because the cabin's reading drives the lens.
// The scene then calls applySplitExposure every tick to carry the readings across.
export void splitExposure(raceengine::Engine& engine, Camera& worldCamera, Camera& carCamera, Camera& frameCamera);

// The per-tick half of split metering: the cabin's exposure becomes the frame's — the lens is not
// per layer, so bloom's threshold and the tone map read one number and it is the cabin's — and the
// world's own exposure rides into the composite as a ratio against it, which is how the world
// arrives at the shared tone map having been exposed by its own meter. A tick behind the meters,
// which is invisible: the adaptation is an exponential closing over seconds.
export void applySplitExposure(raceengine::Engine& engine, const Camera& worldCamera, const Camera& carCamera,
                               Camera& frameCamera, const raceengine::Resource<raceengine::PostProcess>& composite);

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

RigBuild buildRenderRig(raceengine::Engine& engine, Scene& scene, Camera& camera, const float skyDistance,
                        const RigAir air)
{
    // **Six o'clock, and the sun is six degrees up.** The one number that says what time of day this
    // is — everything else in the rig derives from it, which is what makes moving it a change of hour
    // rather than a change of lighting: the sky is a scattering integral that follows this light, the
    // probes photograph that sky and hand it back as the world's indirect light, and the fog takes
    // its shafts from this colour and its haze from those probes.
    //
    // **The heading is the track's own and is deliberately untouched.** Bathurst's `lighting.ini`
    // states `SUN_HEADING_ANGLE = 0`, and this vector has no x component, so it already stands at
    // that heading; only the pitch has moved. The same file states `SUN_PITCH_ANGLE = 25`, which is
    // the generic daylight the track was authored under and is not what was asked for here.
    //
    // Six degrees is roughly half an hour after sunrise at this latitude, and it is chosen as low as
    // the shadow bias will take: the budget is stated in cascade texels and its slope term caps at
    // `SHADOW_MAX_SLOPE`, which on flat ground is reached at about seven degrees. Lower than this and
    // the ground acnes rather than merely stretching, so this is the edge of what the shadow map can
    // honour rather than a number somebody liked.
    //
    // Directional, and its direction is the exact opposite of the position the shading reads as
    // "towards the light" — the cascades are fitted along `direction` and the lighting is computed
    // from `position`, so anything else would put the shadow where the light is not.
    const auto sunElevation = glm::radians(air.sunElevationDegrees);
    const auto sunPosition = 350.0f * glm::vec3(0.0f, glm::sin(sunElevation), glm::cos(sunElevation));
    auto& sun = engine.scene().createLight(scene);
    sun = raceengine::Light{.type = raceengine::LightType::Directional,
                            .position = sunPosition,
                            .direction = -glm::normalize(sunPosition),
                            // A sunrise sun is reddened because it is seen through nine times the
                            // air a midday one is, and both halves of that are here: the colour and
                            // the loss. Rayleigh extinction alone at this elevation transmits about
                            // 0.62 of the red, 0.38 of the green and 0.04 of the blue, which is a
                            // sun with almost no blue left in it; the ratio below is that pulled
                            // back towards what haze and aerosol actually leave, which is redder
                            // than midday and not monochrome. It is 1.17 stops down on the noon
                            // figure it replaces in luminance, and the meter opens for the rest —
                            // the *ratio* between this and the sky is the thing being stated,
                            // because the sky's own brightness comes from the scattering integral
                            // and does not read this at all.
                            .diffuse = glm::vec3(2.24, 1.30, 0.49),
                            .specular = glm::vec3(0.896, 0.520, 0.196),
                            // Zero, and deliberately. Ambient used to be a floor under the diffuse
                            // term — a flat grey added everywhere, which is what a scene says when
                            // it has no indirect light and has to fake one. The light probes are
                            // the indirect light now, and a floor under them would put light into
                            // exactly the shadowed places they were added to keep dark.
                            .ambient = glm::vec3(0.0f),
                            .attenuation = 1.0f};

    engine.camera().setRoll(camera, 0, 1, 0);

    auto loaded = awaitAll(engine.resource().loadTextFileAsync("assets/Shaders/PresentToScreenVertexShader.glsl"),
                           engine.resource().loadTextFileAsync("assets/Shaders/PresentToScreenFragmentShader.glsl"),
                           engine.resource().loadTextFileAsync("assets/Shaders/PassThroughVertexShader.glsl"),
                           engine.resource().loadTextFileAsync("assets/Shaders/PbrFragmentShader.glsl"),
                           engine.resource().loadTextFileAsync("assets/Shaders/BlinnPhongFragmentShader.glsl"),
                           engine.resource().loadTextFileAsync("assets/Shaders/WindshieldFragmentShader.glsl"),
                           engine.resource().loadTextFileAsync("assets/Shaders/CarpaintFragmentShader.glsl"),
                           engine.resource().loadTextFileAsync("assets/Shaders/DepthOnlyVertexShader.glsl"),
                           engine.resource().loadTextFileAsync("assets/Shaders/DepthOnlyFragmentShader.glsl"),
                           engine.resource().loadTextFileAsync("assets/Shaders/ColourFragmentShader.glsl"),
                           engine.resource().loadTextFileAsync("assets/Shaders/HdrVertexShader.glsl"),
                           engine.resource().loadTextFileAsync("assets/Shaders/HdrFragmentShader.glsl"),
                           engine.resource().loadTextFileAsync("assets/Shaders/LuminanceFragmentShader.glsl"),
                           engine.resource().loadTextFileAsync("assets/Shaders/CompositeFragmentShader.glsl"),
                           engine.resource().loadTextFileAsync("assets/Shaders/WorldRainFragmentShader.glsl"),
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

    auto [presentationVert, presentationFrag, vert, pbrFragmentShader, blinnPhongFragmentShader,
          windshieldFragmentShader, carpaintFragmentShader, depthVertexShader,
          depthFragmentShader, colourFragmentShader, hdrVertexShader, hdrFragmentShader, luminanceFragmentShader,
          compositeFragmentShader, worldRainFragmentShader, prepassVertexShader, prepassFragmentShader,
          gtaoFragmentShader, aoBlurFragmentShader,
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

    // The inner pane of a windscreen: the grime on the glass and nothing else, the outer pane beside
    // it staying "pbr" and keeping the reflection. Registered under the name the *asset* asks for —
    // the car's `GlassInt` carries `extras.shader = "windshield"`, mapped by the exporter from AC's
    // own `ksWindscreen` — so this string and that one have to agree or the material falls back to
    // its renderable's shader and says so.
    orThrow(engine.shader().createShader(
        "windshield",
        ShaderDescriptor{.vertexShaderSource = vert, .fragmentShaderSource = windshieldFragmentShader}));

    // Car paint: the PBR base coat with flake, a clearcoat and orange peel over it. Registered under
    // the name the exporter writes for a material called `Carpaint` — AC gives paint no dedicated
    // shader of its own, so the name is the only thing that identifies it, which is why the mapping
    // is a default a Blender property can override rather than an answer.
    //
    // What it draws is the *renderable's* paint rather than the material's, so a car is recoloured
    // through `RenderableModel::paint` without anything going looking for a material.
    orThrow(engine.shader().createShader(
        "carpaint",
        ShaderDescriptor{.vertexShaderSource = vert, .fragmentShaderSource = carpaintFragmentShader}));

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

    // The layered frame's join: car buffer over world buffer, premultiplied, with the world's
    // exposure ratio as its one parameter. Fullscreen like everything else in the chain.
    auto compositeShader = orThrow(
        engine.shader().createShader("composite", ShaderDescriptor{.vertexShaderSource = hdrVertexShader,
                                                                   .fragmentShaderSource = compositeFragmentShader}));

    // Falling rain over the composited frame, before anything blended. Registered always, built
    // into the chain only when the scene states rain.
    auto worldRainShader = orThrow(
        engine.shader().createShader("world rain", ShaderDescriptor{.vertexShaderSource = hdrVertexShader,
                                                                    .fragmentShaderSource = worldRainFragmentShader}));

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

    // **The layered frame** (2026-08-25, Dominic's architecture): the world and the car render into
    // separate buffers, a composite lays one over the other in linear radiance, and one final view
    // draws everything blended over the join. Three cameras, in the order the frame records them:
    //
    //   1. `camera` — the one the scene created and steers — becomes the **world** camera: the
    //      world layer's opaque draws, into its own colour buffer and the frame's one depth buffer.
    //   2. `carCamera` draws the car layer's opaque draws into a transparent buffer against that
    //      same depth, so the world occludes the car per pixel with no compositing cleverness; the
    //      composite pass then joins the two buffers as its post chain.
    //   3. `frameCamera` draws **every** layer's blended geometry over the composite, keeping the
    //      global back-to-front sort, and carries the lens — meter, bloom, tone map — because the
    //      lens is not per layer: bloom is light spilling inside a camera, and a bright sky must be
    //      able to bleed onto an A-pillar.
    //
    // What the split buys, in order: interior and exterior exposure can decouple (splitExposure);
    // world weather becomes one cheap fullscreen pass over the world buffer, behind the windscreen
    // by construction; and the glass's behind-copy is taken from the composited frame, so a drop's
    // refraction shows the finished exterior. With one meter and a ratio of one the whole
    // construction is byte-identical to the un-layered renderer, which is how it was proven inert.
    camera.layerMask = worldLayer;
    camera.partition = raceengine::DrawPartition::OpaqueOnly;
    // The depth outlives the pass: the car camera loads it, and no sampler exists to say so.
    camera.keepDepth = true;

    // The world's own meter, over the world's own buffer. Built quiet: linkExposure below leaves the
    // frame camera metering exactly as the single camera always did, and splitExposure wakes this
    // one for a cockpit. Its compensation is the rig's outdoor dial, which is precisely the frame it
    // was tuned on — a world under a sky, no car in the reading.
    orThrow(engine.autoExposure().enable(
        camera, raceengine::CreateAutoExposureDTO{
                    .shader = luminanceShader,
                    .meter = raceengine::AutoExposure{.compensation = rigCompensation, .centreWeighting = 1.00f}}));
    camera.autoExposure.enabled = false;

    // The attachment handles the layered targets are composed from, resolved before anything else is
    // created: these are borrows out of the same storage the creations below add to.
    const auto worldColour =
        engine.fbo().getAttachmentsOfType(engine.camera().getOutputBuffer(camera), FboAttachmentType::Color);
    const auto worldDepth =
        engine.fbo().getAttachmentsOfType(engine.camera().getOutputBuffer(camera), FboAttachmentType::Depth);
    if (worldColour.empty() || worldDepth.empty())
    {
        raceengine::fail("the world camera's render target must carry colour and depth for the layered frame");
    }

    const auto frameWidth = engine.memoryStorage().bufferAttachments.get(worldColour.front()).width;
    const auto frameHeight = engine.memoryStorage().bufferAttachments.get(worldColour.front()).height;

    // The car layer's buffer: colour of its own, the world's depth. Transparent black where nothing
    // drew, because its alpha is what the composite lays it over the world by.
    const auto carColourFbo = orThrow(engine.fbo().create(raceengine::CreateFboDTO{
        .type = raceengine::FboType::Planar,
        .attachments = {raceengine::CreateFboAttachmentDTO{.width = frameWidth,
                                                           .height = frameHeight,
                                                           .type = raceengine::FboAttachmentType::Color,
                                                           .captureFormat = raceengine::TextureFormat::RGBA,
                                                           .internalFormat = raceengine::TextureFormat::RGBA16F}}}));
    const auto carColour = engine.fbo().getAttachmentsOfType(engine.memoryStorage().frameBuffers.get(carColourFbo),
                                                             FboAttachmentType::Color);
    const auto carOutput = orThrow(engine.fbo().compose({carColour.front(), worldDepth.front()}));

    auto& carCamera =
        orThrow(engine.scene().createCamera(scene, raceengine::CreateCameraDTO{.output = carOutput})).get();
    carCamera.tracksWindowSize = true;
    carCamera.layerMask = carLayer;
    carCamera.partition = raceengine::DrawPartition::OpaqueOnly;
    carCamera.loadDepth = true;
    carCamera.keepDepth = true;
    carCamera.clearColour = glm::vec4(0.0f);
    engine.camera().setRoll(carCamera, 0, 1, 0);
    // One eye: the same film, the same aperture, the same starting exposure the scene stated.
    carCamera.iso = camera.iso;
    carCamera.aperture = camera.aperture;
    engine.camera().setExposure(carCamera, camera.exposure);
    // The occlusion the car's shading samples is the one gather the world camera records over every
    // layer — enabled here only so the binding resolves; no prepass, so nothing records twice.
    carCamera.ambientOcclusion.enabled = true;
    carCamera.ambientOcclusion.occlusion = camera.ambientOcclusion.occlusion;

    // The cabin's meter, over the cabin's own pixels: coverage-weighted, so the world showing
    // through the glass does not vote, and flat rather than centre-weighted, because the centre of a
    // cockpit frame is the windscreen and the windscreen is not the cabin. Quiet until a cockpit
    // asks (splitExposure); the cockpit's own compensation is the scene's to state, as it always
    // was.
    orThrow(engine.autoExposure().enable(
        carCamera, raceengine::CreateAutoExposureDTO{
                       .shader = luminanceShader,
                       .meter = raceengine::AutoExposure{.centreWeighting = 0.0f, .coverageWeighting = 1.0f}}));
    carCamera.autoExposure.enabled = false;

    // The join, riding the car camera's chain so it runs after both layers are down and before the
    // frame camera draws anything blended. Parameter x is the world's exposure over the frame's —
    // one until a cockpit splits the meters.
    auto composite = orThrow(engine.postProcess().create("composite", compositeShader));
    engine.postProcess().addInput(composite, worldColour.front());
    engine.postProcess().addInput(composite, carColour.front());
    engine.postProcess().setParameters(composite, glm::vec4(1.0f, 0.0f, 0.0f, 0.0f));
    engine.camera().addPostProcess(carCamera, composite);

    // The frame camera: the composite's buffer as its colour, loaded rather than cleared, the same
    // shared depth, and every layer's blended draws in one globally sorted pass — which is also
    // where the behind-copy is taken, so the glass refracts the finished, composited exterior.
    const auto compositeOutput = engine.memoryStorage().postProcesses.get(composite).output;
    if (!compositeOutput.has_value())
    {
        raceengine::fail("the composite pass has no output buffer for the frame camera to draw over");
    }

    const auto frameColour = engine.fbo().getAttachmentsOfType(
        engine.memoryStorage().frameBuffers.get(compositeOutput.value()), FboAttachmentType::Color);
    if (frameColour.empty())
    {
        raceengine::fail("the composite pass's output has no colour attachment");
    }

    // The occlusion prepass's buffer, resolved here because two passes read it: the world-rain
    // pass below takes its alpha as the view depth of the nearest opaque surface — of **every**
    // layer, because the prepass mask is forced wide for the ambient occlusion seam, and the rain
    // inherits that decision for free — and the tone map binds it further down as its glass mask.
    const auto prepassBuffer = camera.ambientOcclusion.prepass;
    if (!prepassBuffer.has_value())
    {
        raceengine::fail("the layered frame reads the occlusion prepass, and this camera gathers none");
    }

    const auto prepassColour = engine.fbo().getAttachmentsOfType(
        engine.memoryStorage().frameBuffers.get(prepassBuffer.value()), FboAttachmentType::Color);
    if (prepassColour.empty())
    {
        raceengine::fail("the occlusion prepass has no colour attachment");
    }

    // Falling rain, stage 2 of docs/world-rain-brief.md: one fullscreen pass on the car camera's
    // chain, immediately after the composite — so it draws over the world and the car's opaque
    // geometry and under everything blended, which puts it behind the windscreen, its grime and
    // its drops by construction. Built only when the scene states rain: the dry frame's plumbing
    // is exactly the un-rained plumbing, which is what keeps both gates byte-identical without a
    // branch anywhere in the engine. The frame camera then draws over whichever buffer ends the
    // car camera's chain.
    auto framePane = frameColour.front();
    if (air.rain > 0.0f)
    {
        auto rainPass = orThrow(engine.postProcess().create("world rain", worldRainShader));
        engine.postProcess().addInput(rainPass, frameColour.front());
        engine.postProcess().addInput(rainPass, prepassColour.front());
        // Density, streak length and brightness multipliers, one each — the seat's dials, defaulted
        // to the model's own numbers.
        engine.postProcess().setParameters(rainPass, glm::vec4(1.0f, 1.0f, 1.0f, 0.0f));
        engine.camera().addPostProcess(carCamera, rainPass);

        const auto rainOutput = engine.memoryStorage().postProcesses.get(rainPass).output;
        if (!rainOutput.has_value())
        {
            raceengine::fail("the world-rain pass has no output buffer");
        }

        const auto rainColour = engine.fbo().getAttachmentsOfType(
            engine.memoryStorage().frameBuffers.get(rainOutput.value()), FboAttachmentType::Color);
        if (rainColour.empty())
        {
            raceengine::fail("the world-rain pass's output has no colour attachment");
        }

        framePane = rainColour.front();
    }

    const auto frameOutput = orThrow(engine.fbo().compose({framePane, worldDepth.front()}));

    auto& frameCamera =
        orThrow(engine.scene().createCamera(scene, raceengine::CreateCameraDTO{.output = frameOutput})).get();
    frameCamera.tracksWindowSize = true;
    frameCamera.partition = raceengine::DrawPartition::BlendedOnly;
    frameCamera.loadColour = true;
    frameCamera.loadDepth = true;
    engine.camera().setRoll(frameCamera, 0, 1, 0);
    frameCamera.iso = camera.iso;
    frameCamera.aperture = camera.aperture;
    engine.camera().setExposure(frameCamera, camera.exposure);

    // How dark the dark parts of the picture are, which is a different question from how much light
    // there is. On the frame camera because the tone map is: it is the look rather than the light,
    // and two scenes printing through different curves would be two renderers.
    //
    // Contrast is 1.0 and not above it. It multiplies log radiance about middle grey *before* the
    // filmic curve, and Narkowicz's fit reaches exactly 1.0 at an input of 7.24, so anything above
    // one drags the print-white ceiling down and flattens the sky against it. The shoulder is not
    // the lever it looks like either — it is pow(mapped, 1 + shoulder * mapped), and pow(1, anything)
    // is 1, so it darkens what is *near* white and cannot touch what is already at it.
    engine.camera().setToneCurve(frameCamera, ToneCurve{.contrast = 1.0f, .toe = 0.12f, .shoulder = 0.1f});

    // Pose before the first tick, so frame zero is not three cameras looking three ways.
    syncLayeredCameras(camera, carCamera, frameCamera);

    // Auto exposure, ahead of the tone map in the camera's chain because what it measures is the
    // radiance the tone map is about to consume. This is the frame's meter — the one that reads the
    // composited image exactly as the single camera always did, and the one the linked default
    // leaves running.
    //
    // Compensation and weighting are not the same dial. The compensation says how far a frame drawn
    // by this rig sits from a photometric reading — the meter's own answer puts the geometric mean
    // at middle grey, and both of these scenes are a subject standing under a sky three orders of
    // magnitude brighter than it, so the neutral answer is a correctly exposed sky and an
    // unreadable subject. The weighting says which part of the frame gets to decide, and answers
    // differently every time the camera turns.
    orThrow(engine.autoExposure().enable(
        frameCamera, raceengine::CreateAutoExposureDTO{
                         .shader = luminanceShader,
                         .meter = raceengine::AutoExposure{.compensation = rigCompensation, .centreWeighting = 1.00f}}));

    // Bloom, ahead of the tone map in the camera's chain for the same reason the meter is: what it
    // produces is consumed by the pass that follows it.
    //
    // A threshold of 2.0 in exposed radiance is about eleven times middle grey: the sun, the neon
    // and the specular off the car, and *not* the sky. Thresholding happens in the *exposed* domain,
    // which is what makes that number survive a change of scene at all — the meter moves the shutter
    // under it and the amount of spill stays put.
    orThrow(engine.bloom().enable(
        frameCamera, raceengine::CreateBloomDTO{
                         .downsampleShader = bloomDownsampleShader,
                         .upsampleShader = bloomUpsampleShader,
                         .bloom = raceengine::Bloom{
                             .threshold = 2.0f, .knee = 0.6f, .intensity = 0.30f, .maximum = 20.0f, .spread = 2.2f}}));

    auto hdr = orThrow(engine.postProcess().create("hdr", hdrShader));

    // Element 0: the frame this pass tone maps — the composite, or the rained composite when the
    // scene states rain; either way it is the frame camera's own colour attachment.
    engine.postProcess().addInput(hdr, framePane);

    // Element 1: **the occlusion prepass's buffer, and not the camera's own depth attachment.**
    //
    // The camera's depth used to ride here and was never read by anything, which was just as well —
    // it carried no sampler then, so its `storeOp` was `DONT_CARE` and its contents after the pass
    // were undefined. What the prepass writes *is* readable: a view-space normal in rgb and **the
    // distance in front of the eye in alpha**, at the view's own resolution, with transparent
    // geometry deliberately excluded.
    //
    // That last exclusion is what makes it the mask this pass wants. Through a windscreen the nearest
    // *opaque* surface is the road beyond it, and the dashboard, the A-pillar, the roof lining and
    // the mirror are opaque and a metre away — so one comparison separates "looking through the
    // glass" from "looking at the car", which is the difference between grime on a windscreen and
    // grime on a monitor. Resolved above, beside the world-rain pass that reads the same buffer.
    engine.postProcess().addInput(hdr, prepassColour.front());

    // The third input, after the camera's colour and depth: the top of the bloom chain, which the
    // tone map adds in after exposure and before the curve.
    engine.postProcess().addInput(hdr, frameCamera.bloom.result);


    // One number: how much of the bloom chain the tone map adds back. The three beside it were the
    // lens dirt plate's — its strength, the depth at which the cabin ended and the ceiling that kept
    // the solar disc from printing the whole windscreen white — and they went with it (2026-08-24).
    // Grime is `WindshieldFragmentShader`'s now, shaded on the pane the car actually carries.
    engine.postProcess().setParameters(hdr, glm::vec4(frameCamera.bloom.intensity, 0.0f, 0.0f, 0.0f));

    engine.camera().addPostProcess(frameCamera, hdr);

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

    // The air, which is what gives distance a cost and what the god rays are made of.
    //
    // It is here rather than in a scene for the reason the tone curve is: two scenes standing in
    // different air would be two renderers, and the shafts falling across the pit lane have to be
    // the same shafts that fall across the apron. What a scene states is its ground and a
    // multiplier — see RigAir.
    //
    // **Nothing here is a colour**, and that is the whole of how this works at any hour. The shafts
    // are the colour of the light casting them, and the haze is the mean of what the scene's global
    // probe photographed — so noon hazes blue, this dawn hazes orange, and a re-captured probe after
    // `invalidateAll` moves both without a number changing anywhere. What is stated is the medium.
    //
    // **The three numbers below were set against the edge of the world, not by eye**, and that is
    // worth knowing before any of them is moved. Bathurst's scenery stops somewhere out in the
    // valley and the sky box stands behind it, so where the terrain runs out there is a seam — the
    // far edge of a finite world against the sky. Fog is what hides it, and it hides it only if the
    // haze at that distance is close to opaque: what remains visible of the seam is the
    // transmittance times the difference between the land and the sky, so a horizon at 70% haze
    // still shows three tenths of the join. Every fragment past `maximumDistance` — terrain and sky
    // alike — is fogged over exactly the same path, so at that range the two converge on one colour
    // and the seam has nothing left to be.
    //
    // Hence 30,000 units, which is the sky box's own distance and therefore the furthest anything in
    // this scene can be, and a density of 1.2e-4 per world unit, which is an optical depth of one at
    // about 830 m and better than 5 optical depths across that whole reach at the valley floor. The
    // first was 6e-5 over 20,000 and it was measurably not enough: read from the Esses, where the
    // camera stands 115 m up in a fifth of the density the pit lane does, that combination reached
    // only about a third of a stop of haze at the horizon and the join stayed visible.
    //
    // The scale height of 1600 units — 160 m — is what keeps it a *layer* rather than a dimming.
    // Bathurst climbs 174 m, so the top of the Mountain stands in about half the density the pit
    // straight does and looking down off Skyline is looking down into it; that gradient is most of
    // what makes the fog read as air with weight rather than as a filter over the lens. It went up
    // from 1000 with the density, because at 1000 the one place a driver spends most of the lap —
    // up on the Mountain — was also the one place the effect had thinned to almost nothing.
    //
    // 0.9 says the medium scatters nine tenths of what it takes and absorbs the rest, which is a
    // clean air's answer; smoke would be far lower. The asymmetry at 0.7 is where the forward lobe
    // is strong enough that turning towards the sun is a different picture from turning away — which
    // is the effect being asked for, since a medium at 0 has no shafts in it at all, only a wash.
    //
    // `OSR_FOG` multiplies the density for one run, which is how a thickness is chosen from the seat
    // rather than from a rebuild, and `OSR_FOG=off` is the control every judgement here is made
    // against.
    orThrow(engine.scene().setFog(
        scene, raceengine::Fog{.enabled = air.densityScale > 0.0f,
                               .density = 1.2e-4f * air.densityScale,
                               .scaleHeight = 1600.0f,
                               .baseHeight = air.baseHeight,
                               .maximumDistance = 30000.0f,
                               .scatteringAlbedo = glm::vec3(0.9f),
                               .anisotropy = 0.7f}));

    // The rain beside the fog: weather the scene states once and every reader branches on.
    orThrow(engine.scene().setRain(scene, air.rain));

    auto& skyEntity = engine.scene().createEntity(
        scene, CreateRenderableModelDTO{
                   .node = engine.sceneManager().createNode(scene), .shader = skyboxShader, .model = skyboxModel});

    engine.sceneManager().setScale(skyEntity.node, skyDistance, skyDistance, skyDistance);
    // The sky is not a caster. A 2500-unit box in the depth map fills every cascade at its near
    // plane, and the whole world is then in its shadow.
    skyEntity.castsShadow = false;

    return RigBuild{.sky = &skyEntity, .carCamera = &carCamera, .frameCamera = &frameCamera, .composite = composite};
}

void syncLayeredCameras(const Camera& pose, Camera& carCamera, Camera& frameCamera)
{
    for (auto* follower : {&carCamera, &frameCamera})
    {
        follower->position = pose.position;
        follower->direction = pose.direction;
        follower->roll = pose.roll;
        follower->fieldOfView = pose.fieldOfView;
        follower->aspectRatio = pose.aspectRatio;
        follower->nearClippingPlane = pose.nearClippingPlane;
        follower->farClippingPlane = pose.farClippingPlane;
        follower->projection = pose.projection;
        follower->orthographicVolume = pose.orthographicVolume;
    }
}

void linkExposure(raceengine::Engine& engine, Camera& worldCamera, Camera& carCamera, Camera& frameCamera,
                  const raceengine::Resource<raceengine::PostProcess>& composite)
{
    worldCamera.autoExposure.enabled = false;
    carCamera.autoExposure.enabled = false;
    frameCamera.autoExposure.enabled = true;
    // Re-seeded from where the frame stands, because the reading a quiet meter holds is however old
    // its last enablement is — adapting out of a stale number is a visible lurch, and adapting out
    // of the current exposure is not.
    engine.camera().setExposure(frameCamera, frameCamera.exposure);
    engine.postProcess().setParameters(composite, glm::vec4(1.0f, 0.0f, 0.0f, 0.0f));
}

void splitExposure(raceengine::Engine& engine, Camera& worldCamera, Camera& carCamera, Camera& frameCamera)
{
    worldCamera.autoExposure.enabled = true;
    carCamera.autoExposure.enabled = true;
    frameCamera.autoExposure.enabled = false;
    // Both waking meters start from the exposure the frame holds now, for the reason linkExposure
    // re-seeds: the split should diverge from here, not from wherever each meter last stood.
    engine.camera().setExposure(worldCamera, frameCamera.exposure);
    engine.camera().setExposure(carCamera, frameCamera.exposure);
}

void applySplitExposure(raceengine::Engine& engine, const Camera& worldCamera, const Camera& carCamera,
                        Camera& frameCamera, const raceengine::Resource<raceengine::PostProcess>& composite)
{
    // The cabin's reading drives the lens: the tone map and bloom's threshold read one exposure and
    // it is this one. Copied rather than metered on the frame camera, whose own meter is quiet.
    frameCamera.exposure = carCamera.exposure;
    frameCamera.shutterTime = carCamera.shutterTime;
    // And the world rides in at its own meter's level, as a ratio the composite applies — so after
    // the shared tone map multiplies by the frame's exposure, the world has seen exactly its own.
    engine.postProcess().setParameters(
        composite, glm::vec4(worldCamera.exposure / std::max(frameCamera.exposure, 1e-6f), 0.0f, 0.0f, 0.0f));
}

} // namespace osr
