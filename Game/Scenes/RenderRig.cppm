module;

#include <algorithm>
#include <expected>
#include <optional>
#include <string>
#include <utility>

#include <glm/glm.hpp>

export module osr.game:RenderRig;

import :CloudNoise;

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
    // The clouds, 0..1-ish, `OSR_CLOUDS` and nothing else. Zero — unset — is the clear sky and is
    // byte-identical to a renderer with no clouds in it. What the frame actually shows is the
    // *effective* coverage the rig derives below: rain imposes an overcast floor on it, because a
    // raining scene under a clear dawn is the gap this feature exists to close.
    float clouds = 0.0f;

    // The dome map's texel count, `OSR_CLOUD_MAP`. The default is the 1024x512 this rig used before
    // the size was a knob, so unset is byte-identical to the map that both goldens were blessed
    // against. It is the one cloud decision that trades frame rate against detail directly, and the
    // two axes are not equally worth raising — Options.cppm's cloudMapSize carries the arithmetic.
    int cloudMapWidth = 1024;
    int cloudMapHeight = 512;

    // How the dome mixes each march into the map, `OSR_CLOUD_BLEND`. Unset is the transmittance
    // doing the mixing, which is what the pass did until 2026-08-27; the game states 0.5, and
    // RunOptions::cloudBlendWeight carries the measurements that chose it and the reason a stated
    // weight is what lets the march be amortised at all.
    std::optional<float> cloudBlendWeight{};
    // How many frames apart the dome pass runs, `OSR_CLOUD_EVERY`, and how many vertical strips one
    // full refresh of the map is spread over, `OSR_CLOUD_STRIPS`. A texel is refreshed every
    // `interval * strips` frames; 1 and 1 is the whole map every frame, which is what the pass has
    // always done. RunOptions carries the measurements that chose the shipped pair.
    int cloudMarchInterval = 1;
    int cloudMarchStrips = 1;
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
    // The effective cloud coverage the rig settled on — the scene's own statement with the rain
    // floor applied — handed back because the scene owns the one per-tick job clouds create: the
    // scheduled probe re-photograph (CloudProbeRecapture below). Zero is the clear sky.
    float cloudCoverage = 0.0f;
    // The dome pass itself and how often it is to march, handed back for the other job clouds
    // create — the per-frame one, CloudMarchSchedule below. Unset on a clear sky, where there is
    // no pass to hold.
    std::optional<raceengine::Resource<raceengine::PostProcess>> cloudPass{};
    int cloudMarchInterval = 1;
    int cloudMarchStrips = 1;
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

// The clouded sky's one scheduled probe re-photograph, called from the scene's tick path with the
// rig's effective coverage.
//
// The startup captures photograph clouds lit by a probe band-0 that is still zero — the clouds'
// own ambient reads the probes, and the probes have not been taken yet — and probes then never
// recapture on their own, freezing that slightly-dark first photo for the whole run. One
// `invalidateAll` after the first capture generation settles re-photographs the sky the clouds
// are actually in. Scheduled on the simulated tick count and never wall time, so under a capture
// it is a function of the frame number and the gates reproduce; at coverage zero it must not fire
// at all, which is what keeps the clear sky's probe schedule byte-identical to today's.
export class CloudProbeRecapture
{
private:
    bool fired = false;

public:
    void update(raceengine::Engine& engine, Scene& scene, float coverage);
};

// The clouded sky's other scheduled job: how often the dome actually marches, called once per frame
// from the scene's frame callback (docs/cloud-amortisation-brief.md, stages 1 and 2).
//
// The march is the most expensive single pass in the frame — **4.39 ms of a cockpit frame's 10.93
// at 2560x1440**, which is the whole of what the clouds cost — and almost nothing it computes goes
// stale between two frames: the map is lat-long in **world direction**, so a camera that turns
// re-reads the same map, and a distant cloud layer has no parallax worth the name. Refreshing each
// texel every N frames divides the cost by N.
//
// **How those frames are spent is the interesting half, and it is measured.** Two mechanisms
// compose here and neither dominates on its own: holding the pass makes the *average* frame cheap
// and leaves one frame in the interval paying the whole march, while splitting the map into strips
// makes every frame pay a fraction and costs more in total, because about 1.2 ms of the 4.39 is
// per-pass and does not divide. Holding every eight is 7.30 ms mean at p95 11.34; a strip a frame
// over eight is 8.01 at p95 8.78; four frames apart in two strips — the shipped pair — is 7.32 at
// p95 9.25. RunOptions::cloudMarchStrips carries the table.
//
// Counted in frames and never in seconds. The engine's frame callback is the only clock here that
// runs at the frame's own rate, and a cadence taken off the tick count would drift against it at
// every frame rate but 120 — sometimes marching twice in a row and sometimes skipping a whole
// cycle. Under a capture the frame count is the frame number, so a gate reproduces.
export class CloudMarchSchedule
{
private:
    std::optional<raceengine::Resource<raceengine::PostProcess>> pass{};
    int interval = 1;
    int strips = 1;
    // Counts up from zero so that frame 0 marches strip zero: a capture's first frame must draw the
    // sky it would have drawn anyway, and a probe face photographing an unmarched map would bake
    // the initial clear into the scene's ambient light.
    int frame = 0;

public:
    // Taken from what the rig handed back. A rig that built no dome — the clear sky — leaves this
    // holding nothing and every update below is a no-op, which is what keeps a cloudless run
    // plumbing-identical to a renderer with no schedule in it.
    void bind(const RigBuild& built);
    void update(raceengine::Engine& engine);
};

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

// Per-channel transmittance of the clear atmosphere along the sun's slant path — Beer-Lambert
// over numerically integrated Rayleigh and Mie optical depth. Every constant here mirrors the
// `atmosphere()` call site in SkyboxFragmentShader.glsl and MUST NOT DRIFT from it: this is the
// same air the sky is drawn through, read once on the CPU so the light's colour can follow the
// elevation the way the sky already does. Used only as a ratio against the six-degree anchor, so
// step-count precision cancels rather than accumulates.
[[nodiscard]] glm::vec3 atmosphericSunTransmittance(const float elevationRadians)
{
    constexpr auto planetRadius = 6371e3f;
    constexpr auto atmosphereRadius = 6471e3f;
    constexpr auto rayOriginHeight = 6372e3f;
    constexpr auto rayleighScattering = glm::vec3(5.5e-6f, 13.0e-6f, 22.4e-6f);
    constexpr auto mieScattering = 21e-6f;
    constexpr auto rayleighScaleHeight = 8e3f;
    constexpr auto mieScaleHeight = 1.2e3f;
    constexpr auto steps = 256;

    const auto direction = glm::vec3(0.0f, glm::sin(elevationRadians), glm::cos(elevationRadians));
    const auto origin = glm::vec3(0.0f, rayOriginHeight, 0.0f);
    const auto b = glm::dot(origin, direction);
    const auto c = glm::dot(origin, origin) - atmosphereRadius * atmosphereRadius;
    const auto exitDistance = -b + glm::sqrt(b * b - c);
    const auto stepSize = exitDistance / static_cast<float>(steps);

    auto rayleighDepth = 0.0f;
    auto mieDepth = 0.0f;
    for (auto step = 0; step < steps; step++)
    {
        const auto sample = origin + direction * ((static_cast<float>(step) + 0.5f) * stepSize);
        const auto height = glm::length(sample) - planetRadius;
        rayleighDepth += glm::exp(-height / rayleighScaleHeight) * stepSize;
        mieDepth += glm::exp(-height / mieScaleHeight) * stepSize;
    }

    return glm::exp(-(rayleighScattering * rayleighDepth + glm::vec3(mieScattering * mieDepth)));
}

RigBuild buildRenderRig(raceengine::Engine& engine, Scene& scene, Camera& camera, const float skyDistance,
                        const RigAir air)
{
    // **Half past four in the afternoon, and the sun is nineteen degrees up** (the derivation is on
    // the option's default). The one number that says what time of day this
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
    // Six degrees — roughly half an hour after sunrise at this latitude, and the anchor the colour
    // scale below is stated against — remains the floor the knob can honour: the shadow budget is
    // stated in cascade texels and its slope term caps at `SHADOW_MAX_SLOPE`, which on flat ground is
    // reached at about seven degrees. Lower than that and the ground acnes rather than merely
    // stretching, so the floor is the edge of what the shadow map can honour rather than a number
    // somebody liked.
    //
    // Directional, and its direction is the exact opposite of the position the shading reads as
    // "towards the light" — the cascades are fitted along `direction` and the lighting is computed
    // from `position`, so anything else would put the shadow where the light is not.
    // **The weather the frame actually shows, settled before the sun is built because the sun
    // reads it.** Effective coverage is the scene's own statement with rain's overcast floor
    // applied — rain couples one way, 0.9 rather than 1.0 so a rain sky is heavy overcast with
    // variation left in it rather than a uniform lid. Effective type blends stratus (0) to
    // cumulus (1): the dry default leans cumulus at 0.7, fair-weather heaps; rain forces it to
    // 0.15, the low flat sheet rain actually falls from. These two constants live here and this
    // file is their record.
    const auto effectiveCloudCoverage = std::max(air.clouds, air.rain > 0.0f ? 0.9f : 0.0f);
    const auto effectiveCloudType = air.rain > 0.0f ? 0.15f : 0.7f;

    // How much of the sun survives the cloud shell, Beer-Lambert and derived rather than tuned:
    // T_sun = exp(-k . coverage . path), the path being the shell's 18,000 world units of
    // thickness (1,800 m — base 15,000, top 33,000, the dome shader's own constants, thickened
    // 2026-08-26 for the cumulus towers) over sin(elevation), floored at 0.05 so a sun on or
    // under the horizon states a long path rather than an infinite one. k = 1.55e-5 per world
    // unit — halved when the shell doubled, so the calibrated products stand: full coverage at
    // six degrees is still T = 0.069, the "roughly 5-10% of direct light under overcast" figure,
    // and overhead full coverage 0.76, a bright-overcast day. One multiply on the one light
    // everything reads, so the scene's direct term, the fog's shafts and the solar disc all dim
    // together; the disc's per-pixel occlusion is the map's and can disagree with this energetic
    // mean at broken coverage, a seam accepted by the brief. At coverage zero the factor is
    // exactly one and the light is bit-identical.
    const auto sunElevation = glm::radians(air.sunElevationDegrees);
    const auto cloudSunPath = 18000.0f / std::max(glm::sin(sunElevation), 0.05f);
    const auto sunCloudTransmittance = glm::exp(-1.55e-5f * effectiveCloudCoverage * cloudSunPath);
    // The hour moves the light's colour as well as its height, and both come from the same air:
    // the stated constants below are the accepted six-degree dawn — the anchor measurement — and
    // this ratio of slant-path transmittances carries them to whatever elevation was asked for.
    // At six degrees it is a value divided by itself, exactly (1,1,1), so the dawn is preserved
    // bit-for-bit; higher suns come out whiter and brighter because less air is in the way, which
    // is the physics rather than a second authored colour.
    const auto sunColourScale =
        atmosphericSunTransmittance(sunElevation) / atmosphericSunTransmittance(glm::radians(6.0f));
    const auto sunPosition = 350.0f * glm::vec3(0.0f, glm::sin(sunElevation), glm::cos(sunElevation));
    auto& sun = engine.scene().createLight(scene);
    sun = raceengine::Light{.type = raceengine::LightType::Directional,
                            .position = sunPosition,
                            .direction = -glm::normalize(sunPosition),
                            // The stated numbers are the six-degree dawn — reddened because a
                            // sunrise sun is seen through nine times the air a midday one is, and
                            // 1.17 stops down on the noon figure in luminance. They stay stated
                            // because they are the accepted anchor: `sunColourScale` above carries
                            // them to the hour actually asked for through the same atmosphere the
                            // sky is drawn with, so at any other elevation the colour whitens and
                            // brightens by derivation rather than by a second authored figure. The
                            // *ratio* between this and the sky is the thing being stated, because
                            // the sky's own brightness comes from the scattering integral and does
                            // not read this at all.
                            .diffuse = glm::vec3(2.24, 1.30, 0.49) * sunColourScale * sunCloudTransmittance,
                            .specular = glm::vec3(0.896, 0.520, 0.196) * sunColourScale * sunCloudTransmittance,
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
                           engine.resource().loadTextFileAsync("assets/Shaders/VolumetricFogFragmentShader.glsl"),
                           engine.resource().loadTextFileAsync("assets/Shaders/FogMarchFragmentShader.glsl"),
                           engine.resource().loadTextFileAsync("assets/Shaders/CloudDomeFragmentShader.glsl"),
                           engine.resource().loadTextFileAsync("assets/Shaders/PrepassVertexShader.glsl"),
                           engine.resource().loadTextFileAsync("assets/Shaders/PrepassFragmentShader.glsl"),
                           engine.resource().loadTextFileAsync("assets/Shaders/GtaoFragmentShader.glsl"),
                           engine.resource().loadTextFileAsync("assets/Shaders/AoBlurFragmentShader.glsl"),
                           engine.resource().loadTextFileAsync("assets/Shaders/AoUpsampleFragmentShader.glsl"),
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
          compositeFragmentShader, worldRainFragmentShader, volumetricFogFragmentShader, fogMarchFragmentShader,
          cloudDomeFragmentShader,
          prepassVertexShader, prepassFragmentShader, gtaoFragmentShader, aoBlurFragmentShader,
          aoUpsampleFragmentShader,
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

    // The volumetric fog, moved out of the scene shaders (2026-08-25): one fullscreen pass over the
    // world buffer, before the composite, marching the same cascades through the fullscreen
    // layout's shadow set. Registered always, built into the chain only when the scene states air.
    auto volumetricFogShader = orThrow(engine.shader().createShader(
        "volumetric fog", ShaderDescriptor{.vertexShaderSource = hdrVertexShader,
                                           .fragmentShaderSource = volumetricFogFragmentShader}));

    auto fogMarchShader = orThrow(
        engine.shader().createShader("fog march", ShaderDescriptor{.vertexShaderSource = hdrVertexShader,
                                                                   .fragmentShaderSource = fogMarchFragmentShader}));

    // The cloud dome: one fullscreen march per frame into a lat-long map over the sky hemisphere,
    // which the skybox composites over its scattering integral — every consumer, probe faces
    // included, then samples the same map. Registered always, built into the chain only when the
    // effective coverage above is non-zero.
    auto cloudDomeShader = orThrow(engine.shader().createShader(
        "cloud dome", ShaderDescriptor{.vertexShaderSource = hdrVertexShader,
                                       .fragmentShaderSource = cloudDomeFragmentShader}));

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

    auto aoUpsampleShader = orThrow(
        engine.shader().createShader("ao upsample", ShaderDescriptor{.vertexShaderSource = hdrVertexShader,
                                                                     .fragmentShaderSource = aoUpsampleFragmentShader}));

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
                    .upsampleShader = aoUpsampleShader,
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
    // ...and it arrives already filled, which is pre-Z (2026-08-26). See the composed target below.
    camera.loadDepth = true;
    // **Transparent black where nothing drew, which pre-Z made load-bearing** (2026-08-26, found
    // from the seat). The contract's clear colour is *white*, and until pre-Z that never showed: the
    // skybox is a 2500-unit box around the camera, so the world pass covered every pixel of its own
    // buffer and the clear was unreachable. With the car in the shared depth the sky behind it is
    // z-rejected, and in a cockpit view **70% of the world buffer came back pure white** — invisible
    // in the picture, because the composite masks the world by `1 - car.a` and the car is opaque
    // there, and catastrophic in the *meter*, which read the world twelve times too bright and shut
    // the exposure from 9.50 to 0.76. That is the crushed world through the windscreen Dominic
    // reported. This is exactly the statement the car camera makes about its own buffer, and it has
    // to be made here for the same reason: where nothing drew there must be nothing.
    camera.clearColour = glm::vec4(0.0f);

    // The world's own meter, over the world's own buffer. Built quiet: linkExposure below leaves the
    // frame camera metering exactly as the single camera always did, and splitExposure wakes this
    // one for a cockpit. Its compensation is the rig's outdoor dial, which is precisely the frame it
    // was tuned on — a world under a sky, no car in the reading.
    //
    // **Coverage-weighted, the other half of the clear above.** A cleared pixel now carries alpha
    // zero, and a meter that counted it would read the world as black exactly where the car hides
    // it — the same error in the other direction. Weighted by coverage, the world meters the world
    // it actually drew: through a cockpit's glazing that is what the driver can see out of, which is
    // the honest subject for an exterior reading anyway. The cabin's meter has said this about its
    // own buffer since the split was built.
    orThrow(engine.autoExposure().enable(
        camera, raceengine::CreateAutoExposureDTO{.shader = luminanceShader,
                                                  .meter = raceengine::AutoExposure{.compensation = rigCompensation,
                                                                                    .centreWeighting = 1.00f,
                                                                                    .coverageWeighting = 1.00f}}));
    camera.autoExposure.enabled = false;

    // The attachment handles the layered targets are composed from, resolved before anything else is
    // created: these are borrows out of the same storage the creations below add to.
    const auto worldColour =
        engine.fbo().getAttachmentsOfType(engine.camera().getOutputBuffer(camera), FboAttachmentType::Color);
    if (worldColour.empty())
    {
        raceengine::fail("the world camera's render target must carry colour for the layered frame");
    }

    // The occlusion prepass's buffer, resolved once for the four things that read it: the fog pass
    // and the world-rain pass take its **alpha** as the view depth of the nearest opaque surface —
    // of **every** layer, because the prepass mask is forced wide for the ambient occlusion seam —
    // the tone map binds it further down as its glass mask, and since 2026-08-26 its **depth
    // attachment is the layered frame's one depth buffer**.
    const auto prepassBuffer = camera.ambientOcclusion.prepass;
    if (!prepassBuffer.has_value())
    {
        raceengine::fail("the layered frame reads the occlusion prepass, and this camera gathers none");
    }

    const auto prepassColour = engine.fbo().getAttachmentsOfType(
        engine.memoryStorage().frameBuffers.get(prepassBuffer.value()), FboAttachmentType::Color);
    const auto prepassDepth = engine.fbo().getAttachmentsOfType(
        engine.memoryStorage().frameBuffers.get(prepassBuffer.value()), FboAttachmentType::Depth);
    if (prepassColour.empty() || prepassDepth.empty())
    {
        raceengine::fail("the occlusion prepass must carry colour and depth for the layered frame");
    }

    // **Pre-Z** (2026-08-26, docs/pre-z-brief.md). The prepass rasterises this exact view — every
    // layer, at the view's own resolution, immediately before the world camera records — and the
    // profile of 2026-08-26 called the depth it writes beside its normals "a depth nothing samples".
    // It is the frame's visibility, already paid for. Loading it as the layered frame's one depth
    // buffer gives the world, car and frame passes perfect early-z, and the world pass was shading
    // roughly 1.5-2x its visible pixels through the heaviest fragment shaders in the engine.
    //
    // Three things make it safe, and each is a rule somewhere else in this stack rather than a hope:
    // the prepass tests masked alpha with the *same mipped sample* the scene shaders test
    // (PrepassFragmentShader, unified the same day — otherwise a texel the prepass keeps and shading
    // discards is a hole nothing can fill); the scene pipeline compares **LessOrEqual**, because
    // both stages compute `gl_Position` from the identical expression and every re-rasterised
    // fragment therefore arrives at *equal* depth; and the skybox is the one opaque entity absent
    // from the prepass (`castsShadow = false`), so sky pixels hold the clear and the sky draws
    // normally. The car being in the shared depth is deliberate: world pixels behind it are covered
    // by the composite anyway, and rejecting them early is part of what this buys.
    //
    // `shareDepth` is what turns the prepass's `storeOp` from DONT_CARE into STORE — the consumer is
    // the next view's rasteriser, which no sampler handle can prove.
    camera.ambientOcclusion.shareDepth = true;

    // The camera's own framebuffer stops being its target here. Its colour attachment carries on —
    // the composed target names the same element, so it follows every resize the way the car and
    // frame targets already do — and the depth attachment beside it is now written by nothing and
    // read by nothing. Left rather than removed: dropping it means the scenes asking for a
    // colour-only camera, which is a change to two scenes for a buffer that costs only memory.
    const auto worldOutput = orThrow(engine.fbo().compose({worldColour.front(), prepassDepth.front()}));
    camera.output = worldOutput;

    // The volumetric fog, on the world camera's chain so the composite lays the car over an
    // already-fogged world — which keeps the fog in world-domain radiance whatever the exposure
    // policy, and gives the windscreen's behind-copy a fogged exterior for free. Built only when
    // the scene states air, so `OSR_FOG=off` is plumbing-identical to the un-fogged frame. What
    // moved and why the goldens moved with it: docs/renderer.md, *The fog moved to the layered
    // frame*.
    auto worldPane = worldColour.front();
    if (air.densityScale > 0.0f)
    {
        // Two passes since 2026-08-26: the march — the whole integration, at half resolution,
        // blend off so its alpha carries the transmittance as data — and the apply, which lifts
        // (inscatter, transmittance) back to the view's size joint-bilaterally against the
        // prepass depth and lays them over the world. Occlusion made the same split for the same
        // reason: the integral is low-frequency and was the frame's most expensive fullscreen work.
        const auto& worldSize = engine.memoryStorage().bufferAttachments.get(worldColour.front());
        const auto marchFbo = orThrow(engine.fbo().create(raceengine::CreateFboDTO{
            .type = raceengine::FboType::Planar,
            .attachments = {raceengine::CreateFboAttachmentDTO{
                .width = std::max(worldSize.width / 2u, 1u),
                .height = std::max(worldSize.height / 2u, 1u),
                .type = FboAttachmentType::Color,
                .captureFormat = raceengine::TextureFormat::RGBA,
                .internalFormat = raceengine::TextureFormat::RGBA16F}}}));
        const auto marchColour = engine.fbo().getAttachmentsOfType(
            engine.memoryStorage().frameBuffers.get(marchFbo), FboAttachmentType::Color);
        if (marchColour.empty())
        {
            raceengine::fail("the fog march target has no colour attachment");
        }

        auto fogMarch = engine.postProcess().create(fogMarchShader, marchFbo, 0, true, "fog march");
        engine.postProcess().addInput(fogMarch, prepassColour.front());
        engine.postProcess().setBlend(fogMarch, false);
        engine.postProcess().setWindowSizeDivisor(fogMarch, 2);
        engine.camera().addPostProcess(camera, fogMarch);

        auto fogPass = orThrow(engine.postProcess().create("volumetric fog", volumetricFogShader));
        engine.postProcess().addInput(fogPass, worldColour.front());
        engine.postProcess().addInput(fogPass, marchColour.front());
        engine.postProcess().addInput(fogPass, prepassColour.front());
        engine.camera().addPostProcess(camera, fogPass);

        const auto fogOutput = engine.memoryStorage().postProcesses.get(fogPass).output;
        if (!fogOutput.has_value())
        {
            raceengine::fail("the volumetric fog pass has no output buffer");
        }

        const auto foggedColour = engine.fbo().getAttachmentsOfType(
            engine.memoryStorage().frameBuffers.get(fogOutput.value()), FboAttachmentType::Color);
        if (foggedColour.empty())
        {
            raceengine::fail("the volumetric fog pass's output has no colour attachment");
        }

        worldPane = foggedColour.front();
    }

    // The clouds, docs/cloud-rendering-brief.md: one dome pass on the world camera's chain,
    // marching baked noise volumes into a 1024x512 lat-long map the skybox composites next frame
    // — a one-tick lag that is 8 ms of cloud drift, and under capture both sides are functions of
    // the frame number. Built only when the effective coverage is non-zero, so the clear sky is
    // plumbing-identical to a renderer with no clouds in it: no map, no pass, no volumes, and the
    // scene's cloud fields left at the zero every shader branches on. The pass writes the whole
    // map every frame, wherever it sits in this chain — it reads no other pass's output and
    // nothing here reads it, so its order does not matter.
    std::optional<raceengine::Resource<raceengine::PostProcess>> domePass{};
    if (effectiveCloudCoverage > 0.0f)
    {
        // The two baked volumes, compact bytes with a repeat sampler — the noise tiles. Channel
        // count comes off each file's own header; the formats below are the byte formats the
        // volume upload accepts.
        const auto volumeTexture = [&engine](const char* path) {
            auto volume = orThrow(loadCloudVolume(path));
            const auto format = volume.channels == 1   ? raceengine::TextureFormat::R
                                : volume.channels == 2 ? raceengine::TextureFormat::RG
                                : volume.channels == 3 ? raceengine::TextureFormat::RGB
                                                       : raceengine::TextureFormat::RGBA;

            return engine.memoryStorage().textures.add(
                raceengine::Texture{.name = path,
                                    .format = format,
                                    .pixelDataType = raceengine::PixelDataType::UnsignedByte,
                                    .colourSpace = raceengine::ColourSpace::Linear,
                                    .width = volume.width,
                                    .height = volume.height,
                                    .depth = volume.depth,
                                    .bitsPerPixel = volume.channels * 8,
                                    .data = std::move(volume.data)});
        };

        const auto baseNoise = volumeTexture("assets/Textures/cloud-base-noise.rawvol");
        const auto detailNoise = volumeTexture("assets/Textures/cloud-detail-noise.rawvol");

        // The map: half-float radiance in rgb, transmittance in alpha, at a size a window resize
        // leaves alone — it is angular, so it has nothing to do with how many pixels the frame has.
        // `OSR_CLOUD_MAP` states it and unset is the 1024x512 both goldens were blessed against.
        // Cleared once at creation to "no cloud, full transmittance" so the frame-0 probe faces —
        // which sample the map before the first dome pass has run — composite a cloudless sky
        // rather than garbage.
        const auto cloudMapFbo = orThrow(engine.fbo().create(raceengine::CreateFboDTO{
            .type = raceengine::FboType::Planar,
            .attachments = {raceengine::CreateFboAttachmentDTO{.width = static_cast<unsigned int>(air.cloudMapWidth),
                                                               .height =
                                                                   static_cast<unsigned int>(air.cloudMapHeight),
                                                               .type = raceengine::FboAttachmentType::Color,
                                                               .captureFormat = raceengine::TextureFormat::RGBA,
                                                               .internalFormat = raceengine::TextureFormat::RGBA16F,
                                                               .initialColour = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f)}}}));
        const auto cloudMapColour = engine.fbo().getAttachmentsOfType(
            engine.memoryStorage().frameBuffers.get(cloudMapFbo), FboAttachmentType::Color);
        if (cloudMapColour.empty())
        {
            raceengine::fail("the cloud map has no colour attachment");
        }

        auto cloudPass = engine.postProcess().create(cloudDomeShader, cloudMapFbo, 0, false, "cloud dome");
        // Base then detail: order decides the volume slot exactly as it decides an input's element.
        engine.postProcess().addVolumeInput(cloudPass, baseNoise);
        engine.postProcess().addVolumeInput(cloudPass, detailNoise);

        // **This pass reads its own target and now says so** (docs/cloud-amortisation-brief.md,
        // stage 0). Every fullscreen pass blends by source alpha, and this is the one shader whose
        // alpha is not coverage but a transmittance — so what the map stores has always been
        // `radiance * T + previous * (1 - T)`, an accumulation over frames, taken over a load op
        // that leaves the previous frame undefined. It worked because this driver leaves the image
        // alone; it is also the leading suspect for the recorded run-to-run sky flake, and it is
        // exactly the sort of accident that renders correctly until the day it does not.
        //
        // The accumulation itself is kept — its fixed point is the marched answer — but the weight
        // is now stated rather than borrowed from the cloud's own opacity, which is what makes the
        // map converge at a rate the cadence cannot destroy. It is also what closes the flake:
        // measured, two identical captures are bit-identical at a stated weight and differ at
        // `alpha`, with the load op explicit in both, so the undefined load was never the cause.
        engine.postProcess().setLoadColour(cloudPass, true);
        engine.postProcess().setBlendWeight(cloudPass, air.cloudBlendWeight);

        engine.camera().addPostProcess(camera, cloudPass);

        engine.scene().setCloudMap(scene, cloudMapColour.front());
        orThrow(engine.scene().setClouds(scene, effectiveCloudCoverage, effectiveCloudType));

        domePass = cloudPass;
    }

    const auto frameWidth = engine.memoryStorage().bufferAttachments.get(worldColour.front()).width;
    const auto frameHeight = engine.memoryStorage().bufferAttachments.get(worldColour.front()).height;

    // The car layer's buffer: colour of its own, the frame's shared depth — the prepass's since
    // pre-Z, which already holds the car's own geometry as well as the world's. Transparent black
    // where nothing drew, because its alpha is what the composite lays it over the world by.
    const auto carColourFbo = orThrow(engine.fbo().create(raceengine::CreateFboDTO{
        .type = raceengine::FboType::Planar,
        .attachments = {raceengine::CreateFboAttachmentDTO{.width = frameWidth,
                                                           .height = frameHeight,
                                                           .type = raceengine::FboAttachmentType::Color,
                                                           .captureFormat = raceengine::TextureFormat::RGBA,
                                                           .internalFormat = raceengine::TextureFormat::RGBA16F}}}));
    const auto carColour = engine.fbo().getAttachmentsOfType(engine.memoryStorage().frameBuffers.get(carColourFbo),
                                                             FboAttachmentType::Color);
    const auto carOutput = orThrow(engine.fbo().compose({carColour.front(), prepassDepth.front()}));

    auto& carCamera =
        orThrow(engine.scene().createCamera(scene, raceengine::CreateCameraDTO{.debugName = "car", .output = carOutput}))
            .get();
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
    engine.postProcess().addInput(composite, worldPane);
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

    const auto frameOutput = orThrow(engine.fbo().compose({framePane, prepassDepth.front()}));

    auto& frameCamera = orThrow(engine.scene().createCamera(
                                    scene, raceengine::CreateCameraDTO{.debugName = "frame", .output = frameOutput}))
                            .get();
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
                                                                       .farResolution = 2048,
                                                                       .cacheFarCascades = true,
                                                                       .lambda = 0.9f,
                                                                       .distance = 2000.0f,
                                                                       .casterExtent = 1500.0f}));

    // The car is masked out of the cached far cascades, and it is a correctness rule rather than
    // a saving: a held map replays whatever stood in it at the refit, and the one thing in these
    // scenes that moves is the car — held with it in, a stale car shadow would trail the real one
    // across far ground. Consistently absent instead: at the far cascades' 24 cm texels a car's
    // shadow was PCF mush anyway, and the pixels a car shadows sit within metres of it, in the
    // near cascades' own bands.
    for (auto index = 2u; index < scene.shadows.cascades.size(); index++)
    {
        if (scene.shadows.cascades[index].camera != nullptr)
        {
            scene.shadows.cascades[index].camera->layerMask = worldLayer;
        }
    }

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

    return RigBuild{.sky = &skyEntity,
                    .carCamera = &carCamera,
                    .frameCamera = &frameCamera,
                    .composite = composite,
                    .cloudCoverage = effectiveCloudCoverage,
                    .cloudPass = domePass,
                    .cloudMarchInterval = air.cloudMarchInterval,
                    .cloudMarchStrips = air.cloudMarchStrips};
}

void CloudMarchSchedule::bind(const RigBuild& built)
{
    pass = built.cloudPass;
    interval = std::max(built.cloudMarchInterval, 1);
    strips = std::max(built.cloudMarchStrips, 1);
}

void CloudMarchSchedule::update(raceengine::Engine& engine)
{
    if (!pass.has_value())
    {
        return;
    }

    // The counter is the schedule's own rather than anything the engine keeps, for the reason the
    // probe scheduler's is: what matters is that it advances exactly once per frame, and the frame
    // callback is the only place that is true.
    //
    // The two knobs multiply: the pass runs on every interval-th frame, and each run it takes the
    // next strip. So a texel is refreshed every `interval * strips` frames, and both mechanisms are
    // always live — a strip count of one is a plain hold and an interval of one is a strip a frame.
    const auto held = frame % interval != 0;
    engine.postProcess().setContentsHeld(pass.value(), held);

    if (!held)
    {
        engine.postProcess().setSlice(pass.value(), static_cast<unsigned int>(frame / interval),
                                      static_cast<unsigned int>(strips));
    }

    frame++;
}

void CloudProbeRecapture::update(raceengine::Engine& engine, Scene& scene, const float coverage)
{
    if (fired || coverage <= 0.0f)
    {
        return;
    }

    // Tick 144 — 1.2 simulated seconds — is after the startup capture generation has settled:
    // the probe scheduler photographs one face per frame in a fixed order, so by then every
    // probe's first cube is on disk and the invalidation re-photographs a sky whose clouds are
    // now lit by a real band-0 rather than the zero the first pass saw. Stated against
    // `simulatedSeconds`, which is ticks times the fixed step and therefore a function of the
    // frame number under capture — never the wall.
    constexpr auto recaptureTick = 144.0;
    if (engine.simulatedSeconds() < recaptureTick * static_cast<double>(raceengine::Engine::fixedTimeStep))
    {
        return;
    }

    engine.lightProbe().invalidateAll(scene);
    fired = true;
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
