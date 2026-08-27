module;

#include <optional>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

export module osr.game:ApronScene;

import :Bollard;
import :CarEntity;
import :DinosaurEntity;
import :FPSCameraController;
import :GroundPlane;
import :Options;
import :RenderRig;

import raceengine;

namespace osr
{

// The rendering fixture: a street corner composed to be looked at, and the only thing the rendering
// gate captures.
//
// It is a scene rather than a camera because that is the only way to make a gate insensitive to
// something. A view of the circuit can be aimed away from the car, and what it then watches is a
// collision-mesh export: three flat regions, nothing with a texture on it, and no glass, no paint
// and no shadow caster anywhere in the frame. Half the renderer's *do not break* list was left with
// no gate over it at all. This scene has all of it in one frame.
//
// **Nothing here is simulated.** There is no physics world, no vehicle and no driveline: the car is
// a transform. That is the point — physics cannot move what physics does not touch, so a driveline
// change moves the driving gate and this one stays bit-identical. The previous arrangement got the
// same property by making the car small and distant and measuring that it only leaked 0.089% of the
// frame, which is a measurement that has to be redone every time anything moves.
export class ApronScene
{
private:
    raceengine::Engine& engine;
    Scene& scene;
    Camera& camera;

    // The one thing that aims this camera. It writes the direction from its own yaw and pitch on
    // every tick, which is why the framing is stated in its constructor and not with `lookAtPoint`.
    // Under an unattended run the window reports no mouse motion at all, so what it writes every
    // tick is the same direction it started with.
    FPSCameraController freeCamera;

    RenderableModel* sky;
    // The layered frame's other two cameras, kept only to hold their pose to the one above: this
    // fixture never splits its meters, so the linked default the rig builds is the whole policy.
    Camera* carCamera = nullptr;
    Camera* frameCamera = nullptr;
    // Built in the body rather than the initialiser list because it needs the "pbr" shader, which
    // the render rig creates down there out of a file this scene awaits.
    std::optional<CarEntity> car;

    // The rig's effective cloud coverage and the one job it leaves the scene: the scheduled probe
    // re-photograph, a function of the tick count that never fires on a clear sky.
    float cloudCoverage = 0.0f;
    CloudProbeRecapture cloudRecapture;
    // And the per-frame one: how often the dome actually marches. See CircuitScene's for why it
    // rides the frame callback and not the tick.
    CloudMarchSchedule cloudMarch;

public:
    // Takes the run's configuration for one thing only: how much air the fixture stands in. The
    // rest of `RunOptions` is the circuit's — this scene has no camera to choose, no driver and
    // no assists — but the fog is the look, the look is shared by both gates, and a fixture that
    // could not be A/B'd against `OSR_FOG=off` would be a fixture that stopped watching it.
    explicit ApronScene(raceengine::Engine& engine, const RunOptions& options);
    void update(float delta);
};

} // namespace osr

namespace osr
{

namespace
{

// Where the camera stands and what it is pointed at. Eye height rather than six hundred units up:
// the ground-to-wall crease, the recesses in the facade and the contact under the car are where the
// indirect light does its work, and from above they are a handful of pixels each.
constexpr auto cameraStand = glm::vec3(270.0f, 32.0f, 250.0f);
constexpr auto cameraYaw = -2.356;
constexpr auto cameraPitch = 0.110;

// The car, out on the open apron past the building's sunlit corner and close enough to the camera to
// be a subject rather than a detail. It is turned across the view so that a windscreen, a side
// window and a rear screen are all in frame at once — the glazing is two near-coplanar shells and
// the blend, the sort and the prepass exclusion are all things that only show on it.
constexpr auto carStand = glm::vec3(228.0f, 0.0f, 108.0f);
// Turned so the flank the camera sees is the flank the sun is on. The sun arrives from +z, so a car
// square to the view has its whole visible side in its own shade and prints as a silhouette — which
// is a picture of the car's outline rather than of its paint, and the paint is what the
// metallicRoughness factor path is read off.
constexpr auto carHeading = -2.269f;

// A metre of painted steel a metre and a half off the building's shaded corner: the smallest thing
// in the frame, and the one place where a contact shadow, an occlusion crease against a wall and the
// nearest cascade's texel size are all being asked the same question at once.
constexpr auto bollardStand = glm::vec3(168.0f, 0.0f, 205.0f);

} // namespace

ApronScene::ApronScene(raceengine::Engine& engine, const RunOptions& options) :
    engine(engine),
    scene(engine.sceneManager().createScene()),
    camera(orThrow(engine.scene().createCamera(scene))),
    freeCamera(engine, cameraYaw, cameraPitch)
{
    camera.debugName = "apron";

    // The number that turns this scene's relative radiance — a sun of 3.2 and an asphalt albedo of
    // about 0.1 — into a picture, and where the meter's adaptation starts from. 4.75 because the
    // meter's own answer on this framing is 4.78: seeding at the answer means the captured frame is
    // not a point on an adaptation curve. Measured, seeding at the apron's historical 2.5 instead
    // leaves the frame a whole value out of 255 short at frame 120, over 4329 of its 32400 blocks —
    // small, and still exactly reproducible, but it is a transient the gate would be holding.
    //
    // Film speed is left where it is: unlike the circuit, this frame does not drive the meter
    // anywhere near its shutter clamp.
    // Re-measured under the six-degree morning sun, where this fixture settles at 7.44; it was 4.75
    // under the 45-degree one. Seeding at the answer is what keeps the captured frame off the
    // adaptation curve — measured previously, seeding a stop out left the frame a whole value of 255
    // short at frame 120 across 4329 of its 32400 blocks.
    engine.camera().setExposure(camera, 7.44f);

    // Faster film, for the reason CircuitScene gives and now for the same measured cause: the low
    // morning sun took this fixture 2.4 stops darker than the midday one it was composed under, and
    // on ISO 6400 the meter asked for longer than `maxShutterTime` and sat pinned at 1/4 s — under
    // its own reading, with the ground plane crushed to black as a result. Film speed and aperture
    // cancel out of the exposure multiplier, so this changes nothing about the picture except that
    // the meter can reach the answer it wants.
    engine.camera().setFilmSpeed(camera, 25600);

    engine.camera().setPosition(camera, cameraStand.x, cameraStand.y, cameraStand.z);

    // The apron's ground is y = 0, so the fog layer is quoted where the fixture actually stands.
    // The rig speaks in float and the options in double, and the conversion has to keep "unset"
    // unset: an absent blend weight is the dome's old transmittance-weighted accumulator, which is a
    // different thing from any number.
    const auto cloudBlend = options.cloudBlendWeight.has_value()
                                ? std::optional<float>(static_cast<float>(options.cloudBlendWeight.value()))
                                : std::optional<float>{};

    const auto rig = buildRenderRig(engine, scene, camera, 2500.0f,
                                    RigAir{.baseHeight = 0.0f,
                                           .densityScale = static_cast<float>(options.fogDensityScale),
                                           .sunElevationDegrees = static_cast<float>(options.sunElevationDegrees),
                                           .rain = static_cast<float>(options.rainIntensity),
                                           .clouds = static_cast<float>(options.cloudCoverage),
                                           .cloudMapWidth = options.cloudMapWidth,
                                           .cloudMapHeight = options.cloudMapHeight,
                                           .cloudBlendWeight = cloudBlend,
                                           .cloudMarchInterval = options.cloudMarchInterval,
                                           .cloudMarchStrips = options.cloudMarchStrips});
    sky = rig.sky;
    carCamera = rig.carCamera;
    frameCamera = rig.frameCamera;
    cloudCoverage = rig.cloudCoverage;
    cloudMarch.bind(rig);

    GroundPlane(engine, scene);
    DinosaurEntity(engine, scene);
    Bollard(engine, scene, bollardStand);

    car.emplace(engine, scene);
    engine.sceneManager().setPosition(car->sceneNode(), carStand.x, carStand.y, carStand.z);
    // The model's own y = 0 is the plane its tyres meet, so a car stood at y = 0 stands on the
    // ground rather than at its ride height above it. That is the whole of what a placed car gives
    // up against a simulated one: no roll, no pitch and no suspension deflection, none of which the
    // renderer can tell from a car that happens to be level.
    engine.sceneManager().setOrientation(car->sceneNode(), glm::angleAxis(carHeading, glm::vec3(0.0f, 1.0f, 0.0f)));

    // The image-based lighting graph. Three nodes, and the point of each is what it can see:
    //
    // The global one stands high with nothing near it, so what it records is very nearly the sky
    // alone. It is what every fragment outside the local volumes falls back on.
    //
    // The second sits in the building's shadow along its back face, low and close, where more than
    // half of what it can see *is* the building — so its irradiance is the dark, slightly warm
    // bounce off a wall rather than the sky, and the specular chain it prefilters has the wall in it
    // where the sky used to be. That is the whole fix for the highlight that used to survive the
    // shadow: nothing subtracts it, the probe simply never recorded it.
    //
    // The third stands out on the open apron beside the car, which is the ground this camera
    // actually looks at, and its box reaches back far enough to meet the second in a band rather
    // than nesting inside it — so a surface crossing out of the shadow crosses through a region
    // where both contribute and neither switches on.
    //
    // Every probe stays well inside the 2500-unit skybox, which follows the camera: a probe captured
    // from outside it would record the box's far wall instead of the sky. Each one also stands clear
    // of the building's own footprint, because a probe inside a building photographs its interior.
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
                                               .position = glm::vec3(250.0f, 45.0f, 120.0f),
                                               .halfExtents = glm::vec3(250.0f, 90.0f, 180.0f),
                                               .blendDistance = 60.0f,
                                               .nearClippingPlane = 2.0f,
                                               .farClippingPlane = 4000.0f}));

    // Registered last, once the scene is fully built: the engine may call this the moment the
    // first tick runs, and a half-constructed scene is not something it should be handed.
    engine.onUpdate([this](float delta) { update(delta); });
    // `this->engine` because the constructor's own parameter shadows the member here, and the
    // lambda outlives the parameter.
    engine.onFrame([this] { cloudMarch.update(this->engine); });
}

void ApronScene::update(float delta)
{
    freeCamera.update(camera, delta);

    // The clouded sky's one scheduled probe re-photograph — on the tick count, and only when the
    // rig settled a non-zero coverage.
    cloudRecapture.update(engine, scene, cloudCoverage);

    // After the controller, so all three views record this tick's eye.
    syncLayeredCameras(camera, *carCamera, *frameCamera);

    engine.sceneManager().setPosition(sky->node, camera.position.x, camera.position.y, camera.position.z);
}

} // namespace osr
