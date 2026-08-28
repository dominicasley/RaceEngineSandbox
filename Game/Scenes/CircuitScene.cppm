module;

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <utility>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

export module osr.game:CircuitScene;

import :CarEntity;
import :ChaseCameraController;
import :CockpitCameraController;
import :FPSCameraController;
import :Options;
import :PlayerCar;
import :RaceTrack;
import :RenderRig;
import :Simulation;
import :SimulatedCar;
import :TrackFrame;
import :WiperController;

import raceengine;

namespace osr
{

// The driving scene: Mount Panorama, the car under the vehicle model, and the camera behind it.
// This is the game as it is played and it is what the driving gate captures, so it is expected to
// move when the vehicle model does — that is the whole reason there is a second scene beside it.
export class CircuitScene
{
private:
    raceengine::Engine& engine;
    Scene& scene;
    Camera& camera;
    // Mount Panorama, twice: the visual model the renderer draws, and the physics model the
    // collision mesh is read out of. Two files on purpose — the scenery can be re-exported without
    // moving a single triangle a tyre touches, and the BVH never wades through a hotel. The physics
    // model is loaded and never drawn, which is exactly why its buffers survive: the renderer frees
    // a mesh buffer's bytes when it uploads them, and a model nothing draws is never uploaded — the
    // ~30 MB it keeps in RAM is the price of the split and is cheaper than the VRAM it does not
    // spend. Declared before everything that queries them, the engine's own member-order rule.
    raceengine::Resource<raceengine::Model> trackModel;
    raceengine::Resource<raceengine::Model> physicsModel;
    // The world went inside the simulation when the simulation got a thread: it is the thing being
    // queried from that thread, so it is the thing that must outlive it, and holding it here would
    // leave that guarantee to this class's member order rather than stating it where the thread is.
    // It is `const` throughout today, which is what makes a reader on another thread safe at all —
    // the day anything mutates it, the simulation already owns it outright.
    std::optional<Simulation> simulation;
    SimulatedCar* simulatedCar = nullptr;

    // Exactly one of these is engaged, chosen in the constructor. Both write the camera's direction
    // on every tick, so a scene holding two live controllers would have one of them silently
    // overwritten before the first frame.
    std::optional<ChaseCameraController> chaseCamera;
    std::optional<CockpitCameraController> cockpitCamera;

    // EV, and Dominic's to move: this is the exposure of the picture rather than a property of the
    // renderer. Re-measured 2026-08-20 against the visual track — the 0.0 this used to be was
    // measured in a world that was one dark ribbon in a void, and the meter that saw that world
    // opened two stops further than the same view deserves with scenery in it. At -1.25 the road
    // ahead keeps its markings, the pit wall reads as concrete and the cabin stays legible.
    static constexpr float cockpitCompensation = -1.25f;
    std::optional<FPSCameraController> freeCamera;

    // Whether the free camera is currently flying, and the edge state of the key that toggles it.
    // Level to edge for the same reason the telemetry key is: `keyPressed` answers on every one of a
    // hundred and twenty ticks a second, so a held key would toggle a hundred and twenty times.
    bool freeCameraEngaged = false;
    bool cameraKeyHeld = false;
    bool poseKeyHeld = false;

    // Print where this camera stands and which way it points, as the command line that puts a
    // camera back there. Level to edge for the reason the two keys above are.
    void logCameraPose() const;

    // Which controller the scene was built with, so leaving the free camera puts it back rather than
    // guessing. A scene launched with OSR_CAMERA=fixed has no other controller to return to and
    // stays where it is.
    CameraChoice configuredCamera = CameraChoice::Chase;

    void toggleFreeCamera();

    RenderableModel* sky;
    // The layered frame's other two cameras and the composite between them, handed back by the rig.
    // `camera` above is the pose camera and the world layer's; these follow it every tick
    // (syncLayeredCameras), and the exposure policy — linked outside the car, split in the cockpit —
    // is this scene's to run because only it knows which view is live.
    Camera* carCamera = nullptr;
    Camera* frameCamera = nullptr;
    raceengine::Resource<raceengine::PostProcess> composite{};
    // Built in the body rather than the initialiser list because both need the "pbr" shader, which
    // the render rig creates down there out of a file this scene awaits.
    std::optional<CarEntity> car;
    std::optional<PlayerCar> player;

    // The rain's airflow phase, accumulated here because the shader is stateless: a drop's
    // position is the airstream's shear *integrated*, and handing the shader only the current
    // speed would teleport every drop the moment the speed changed. The shear grows with the
    // square of the speed, so what accumulates is speed squared over simulated ticks — a function
    // of the tick count under a capture, so a captured frame N carries the same value on every
    // machine. Double, because a float accumulating 8 ms slices loses its low bits within a lap
    // or two.
    bool raining = false;
    double rainAirflowPhase = 0.0;

    // The airspeed the *water* is responding to, which lags the car's. Water on glass has mass and
    // surface tension, so it does not know the car has stopped until it has had a moment to find
    // out — and without that moment a hard stop swings the drift from "blown back" to "running
    // down" inside a couple of ticks and the droplets visibly wiggle as they re-aim. The shader
    // cannot hold this: it is stateless by design, so the lag lives here, where state is allowed.
    double rainDampedSpeed = 0.0;

    // The stalk. Off until the driver asks for it, like every other thing on this car that a real
    // one has switched on — and off is what keeps both gates byte-identical.
    WiperController wipers;

    // The rig's effective cloud coverage and the one job it leaves the scene: the scheduled probe
    // re-photograph, a function of the tick count that never fires on a clear sky.
    float cloudCoverage = 0.0f;
    CloudProbeRecapture cloudRecapture;
    // And the per-frame one: how often the dome actually marches. On the frame callback rather
    // than the tick, because a cadence in frames cannot be kept on a clock that runs at 120 Hz
    // whatever the frame rate is.
    CloudMarchSchedule cloudMarch;

public:
    CircuitScene(raceengine::Engine& engine, const RunOptions& options);
    // The simulation's thread is stopped here, before a single member is destroyed. `~PlayerCar`
    // writes the session's rack trace out of the force feedback service, and a trace taken while
    // the simulation is still publishing into it ends in the middle of a tick.
    ~CircuitScene();

    CircuitScene(const CircuitScene&) = delete;
    CircuitScene(CircuitScene&&) = delete;
    CircuitScene& operator=(const CircuitScene&) = delete;
    CircuitScene& operator=(CircuitScene&&) = delete;

    void update(float delta);
};

} // namespace osr

namespace osr
{

CircuitScene::CircuitScene(raceengine::Engine& engine, const RunOptions& options) :
    engine(engine),
    scene(engine.sceneManager().createScene()),
    camera(orThrow(engine.scene().createCamera(scene))),
    trackModel(orThrow(engine.resource().loadModelAsync(std::string(trackVisualAsset)).get())),
    physicsModel(orThrow(engine.resource().loadModelAsync(std::string(trackPhysicsAsset)).get()))
{
    // The simulation, with the circuit's surfaces in it. Driven a tick at a time under a capture and
    // free-running otherwise — the same test `Engine::frameDelta` makes, because it is the same
    // question: is this run's clock the frame number or the wall.
    simulation.emplace(
        engine,
        orThrow(raceengine::PhysicsWorld::create(orThrow(trackCollisionMesh(engine.memoryStorage(), physicsModel)))),
        std::getenv("RACEENGINE_DUMP_FRAME") != nullptr);

    camera.debugName = "world";

    // Faster film, and it is not a look — it is what stops the meter running out of shutter.
    //
    // Film speed and aperture cancel out of the exposure multiplier, so two cameras metering the
    // same scene agree on the picture and disagree only on the shutter it took. The one place they
    // change anything is at the clamp, and this scene reaches it: a circuit of 0.07-albedo asphalt
    // under an open sky meters 2.6 stops darker than the apron beside it, and on ISO 6400 the meter
    // asked for 1/1.5 s against a `maxShutterTime` of 1/4 and sat pinned there, 1.42 stops under its
    // own reading, with every further scene change moving nothing at all. Four times the film speed
    // puts the answer at 1/6 s, back inside the range, and the meter is a meter again.
    engine.camera().setFilmSpeed(camera, 25600);

    // Bathurst is two kilometres end to end and the default far plane is five hundred metres, which
    // amputated everything past Griffins Bend from ground level. The sky box has to stand outside
    // the whole circuit (it is real geometry that writes depth), and the far plane has to reach the
    // box's *corners* — skyDistance times sqrt(3) — or the sky itself falls out of the frustum. The
    // near plane stays at 0.1 m for the cockpit's own A-pillar; D32 covers this range comfortably.
    engine.camera().setClippingPlanes(camera, 1.0f, 55000.0f);

    // Where the adaptation starts and what the camera holds until the first reading comes back.
    //
    // **Re-measured whenever the light changes, and moving the sun is a change of light.** Seeding
    // far from the answer leaves a 120-frame capture partway along the adaptation curve, which is a
    // transient the parity gate would then be holding rather than a settled frame. Under the
    // six-degree morning sun the chase view — the one the driving gate captures — settles at 8.79.
    // It was 18.0 under the 45-degree sun, which is over a stop out.
    engine.camera().setExposure(camera, 8.79f);

    // Never lookAtPoint: whichever controller is engaged below writes the direction on every tick,
    // so a framing stated here would be overwritten before the first frame — which is precisely
    // what happened to the aerial spawn view this scene used to have, for long enough to be worth a
    // note in the engine's own documentation.
    configuredCamera = options.camera;

    if (options.camera == CameraChoice::Chase)
    {
        // The game as it is played, and the view the driving gate captures: it writes the position
        // too, so there is none to state.
        chaseCamera.emplace(engine);
    }
    else if (options.camera == CameraChoice::Cockpit)
    {
        // The view feel is judged from, and the reason it exists: a wheel's weight cannot be
        // evaluated from behind the car, because half of what it is telling you is where the car is
        // pointed against where it is going.
        cockpitCamera.emplace(engine);
    }
    else
    {
        // Hell Corner from above and behind, and nothing gates it — this is the view to fly the
        // circuit from by hand. It was the rendering gate's framing for a while and was a poor one:
        // three flat regions, no car, nothing with a texture on it, and not a pixel either clipped
        // or under 2/255. The apron scene is the fixture now.
        //
        // **`OSR_CAM_POS` and `OSR_CAM_LOOK` override the two halves of this, and that is what makes
        // a view somebody else saw reproducible.** They were added for the ground-transparency
        // report, which has only ever been seen from a free camera looking into the low sun and
        // which four sessions have now theorised about without once getting it into a capture. The
        // `P` key prints the pose in exactly the form these take, so a driver hands over a line
        // rather than a description of a place.
        const auto standMetres =
            options.cameraPose.positionStated
                ? glm::dvec3(options.cameraPose.xMetres, options.cameraPose.yMetres, options.cameraPose.zMetres)
                : glm::dvec3(-660.0, 195.8, 1216.0);
        const auto stand = toWorldUnits(standMetres);
        engine.camera().setPosition(camera, static_cast<float>(stand.x), static_cast<float>(stand.y),
                                    static_cast<float>(stand.z));

        // Radians, and the scene's own two are stated in radians rather than converted from degrees
        // so that leaving both variables unset is this camera exactly as it was.
        const auto yaw = options.cameraPose.lookStated ? glm::radians(options.cameraPose.yawDegrees) : 1.5708;
        const auto pitch = options.cameraPose.lookStated ? glm::radians(options.cameraPose.pitchDegrees) : -0.08;
        freeCamera.emplace(engine, yaw, pitch);
    }

    // Three kilometres: outside every point of the circuit from every point a camera can stand.
    // Bathurst's pit straight stands at about 350 world units — thirty-five metres — and that is
    // where the fog's density is quoted, so the layer sits on the circuit rather than under it.
    // The Mountain climbs 174 m out of that, which at the rig's hundred-metre scale height puts
    // the top of it in a fifth of the air the pit lane stands in.
    //
    // A fullscreen lens dirt plate was turned on here for the cockpit and is gone (2026-08-24). It
    // needed the scene to know which view looked through glass; `WindshieldFragmentShader` does not,
    // because the glass is geometry the car carries and the grime is shaded on it. Every camera now
    // gets the same rig, which is one fewer thing for a view to have an opinion about.
    // The rig speaks in float and the options in double, and the conversion has to keep "unset"
    // unset: an absent blend weight is the dome's old transmittance-weighted accumulator, which is a
    // different thing from any number.
    const auto cloudBlend = options.cloudBlendWeight.has_value()
                                ? std::optional<float>(static_cast<float>(options.cloudBlendWeight.value()))
                                : std::optional<float>{};

    const auto rig = buildRenderRig(engine, scene, camera, 30000.0f,
                                    RigAir{.baseHeight = 350.0f,
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
    composite = rig.composite;
    cloudCoverage = rig.cloudCoverage;
    cloudMarch.bind(rig);
    raining = options.rainIntensity > 0.0;

    // The car's own sound bank, from the folder the Assetto Corsa car shipped as. Not fatal: a car
    // with no sound is a car that still drives, and a scene that refused to load over a missing
    // .bank would be a scene nobody could run without the content.
    const auto engineSound = raceengine::golfGtiMk7Driveline().engine;
    if (const auto sound = engine.audio().loadCar("assets/Sfx/vw_golf_gti_mk7.5",
                                                  engineSound.idleSpeed * raceengine::radiansPerSecondToRpm,
                                                  engineSound.limiterSpeed * raceengine::radiansPerSecondToRpm);
        !sound)
    {
        engine.log().info("No car audio: {}", sound.error());
    }

    if (cockpitCamera)
    {
        // A cockpit is a different photographic problem from every other view this game has, and
        // under the layered frame it finally gets the honest answer: the meters split. The world
        // meters its own buffer with the rig's outdoor dial, the cabin meters its own pixels —
        // coverage-weighted, so the world showing through the windscreen does not vote — and the
        // cabin's reading drives the shared lens while the world rides the composite at its own.
        // One meter could never serve both: a sunlit world is several stops above a car's cabin,
        // and the single-meter era answered it with a −1.25 EV shove on a full-frame reading.
        //
        // **After the rig and not before it.** `AutoExposureService::enable` assigns the whole meter,
        // so an override written earlier is overwritten by the rig — which is exactly what happened
        // once, and it presented as a compensation that changed nothing at all: two runs a stop
        // apart both settled on an exposure of 18.18.
        //
        // The compensation carried over is the old cockpit dial. Its metering base has changed —
        // cabin pixels only, where it was tuned on the full frame — so it is a starting point for
        // Dominic's eye, not a settled number.
        splitExposure(engine, camera, *carCamera, *frameCamera);
        carCamera->autoExposure.compensation = cockpitCompensation;
        // The cabin has a roof: from the seat, the car layer is the interior, and a wet dashboard
        // is wrong however hard it rains outside. The world's surfaces and the streaks through the
        // glass are untouched — the scale is this view's alone.
        carCamera->rainScale = 0.0f;
    }

    // The circuit itself — the visual export, while the surfaces being driven on come from the
    // physics export in the same world coordinates. There is no position to state: both carry their
    // own world coordinates and `trackOrigin` is zero, so the only thing between the two is the
    // tenth of a metre a world unit is. Anything else here and the surface being driven on would be
    // somewhere the surface being drawn is not, which reads as the car floating or sinking rather
    // than as a transform error.
    //
    // Drawn through the shader the asset names rather than through "pbr": the circuit's materials
    // are Assetto Corsa's own, exported as authored, and they describe themselves in the classic
    // model. `orThrow` rather than `.value()` because a missing shader is a rig that did not build
    // what this scene needs, and the sentence naming which one is worth more than a bad optional
    // access — the car beside it still uses "pbr", which is the same statement from the other side:
    // the model a surface is drawn with belongs to the surface.
    auto& trackEntity = engine.scene().createEntity(
        scene, CreateRenderableModelDTO{.node = engine.sceneManager().createNode(scene),
                                        .shader = shaderNamed(engine, std::string(trackVisualShader)),
                                        .model = trackModel});

    const auto placement = toWorldUnits(glm::dvec3(0.0));
    engine.sceneManager().setPosition(trackEntity.node, static_cast<float>(placement.x),
                                      static_cast<float>(placement.y), static_cast<float>(placement.z));
    engine.sceneManager().setScale(trackEntity.node, static_cast<float>(worldUnitsPerMetre),
                                   static_cast<float>(worldUnitsPerMetre), static_cast<float>(worldUnitsPerMetre));

    // The apron's building, ground plane and bollard used to stand at the world origin here, six
    // hundred metres from the grid across a gap in the ribbon, past the camera's far plane and well
    // past the skybox — three assets nothing could see from anywhere a car can reach, loaded because
    // the smoke gate asserted they were. They are the apron scene's now, and the gate asserts each
    // scene's own contents.

    car.emplace(engine, scene);

    // The first authored grid slot, position and heading both. Not the AI line: its first point is a
    // racing line a metre from the right-hand edge of an eleven-metre road, and its height is the
    // recording car's own reference height rather than the tarmac — three quarters of a metre of
    // thin air. A start box states a heading, which is the other thing an AI line cannot.
    const auto& slot = gridSlots.front();

    // The weather, from the one number this run states and the sun the scene is already lit by —
    // which is the sun's own pattern: state the hour, derive the sky, the probes, the fog and now
    // the road's temperature from it. `OSR_TYRE_TEMP=ambient` is resolved against it here, because
    // `Options.cppm` imports nothing and so cannot ask the physics what a road in this sun is at.
    const auto ambient = raceengine::ambientAt(options.airTemperatureCelsius, options.sunElevationDegrees);
    const auto startingTyreTemperature = [&]() -> std::optional<double>
    {
        switch (options.tyreTemperature.source)
        {
        case TyreTemperatureSource::Track:
            return ambient.trackTemperature;
        case TyreTemperatureSource::Stated:
            return options.tyreTemperature.celsius;
        case TyreTemperatureSource::CarsOwn:
            break;
        }

        return std::nullopt;
    }();

    simulatedCar =
        &simulation->add(slot.position, glm::radians(slot.yaw), options.driver, 0.001 * options.beltBridgingMillimetres,
                         options.geometricLoadPath, options.drivelineReaction, options.tyreThermal,
                         options.tyreContactConductance, options.tyreIdealTemperature, options.tyrePressure,
                         options.brakeThermal, startingTyreTemperature, ambient, options.assists);
    player.emplace(engine, *simulatedCar, car->sceneNode(), car->renderableModel(), options.rackTrace);

    // The image-based lighting graph, and on an open circuit it is one node rather than three.
    //
    // A circuit is open ground under open sky and its indirect light is the sky, the tarmac and the
    // hillside — which is what one probe over the start line records. Where local probes belong is
    // where a circuit genuinely occludes the sky: under the trees at the Dipper, in the cutting, and
    // along the pit wall. That is a probe *per place*, and the constraint that decides how it has to
    // be built is this: the skybox follows the camera at 2500 units, and a probe outside it records
    // the box's far wall instead of the sky. Fixed probes 6 km apart cannot all be inside one 250 m
    // box, so the answer is probes near the car rather than probes everywhere — a different feature
    // from this one.
    //
    // Thirty metres over the pit straight and midway between the two cameras this scene offers,
    // ninety-odd metres from each, which is what the 250 m skybox allows and the reason it is not
    // simply over the grid.
    const auto overTheStraight = toWorldUnits(glm::dvec3(30.0, 65.0, -565.0));
    static_cast<void>(engine.lightProbe().createProbe(
        scene, raceengine::CreateLightProbeDTO{.name = "sky",
                                               .position = glm::vec3(static_cast<float>(overTheStraight.x),
                                                                     static_cast<float>(overTheStraight.y),
                                                                     static_cast<float>(overTheStraight.z)),
                                               .global = true,
                                               .nearClippingPlane = 5.0f,
                                               // Past the sky box's corners, for the reason the
                                               // camera's own far plane is: a probe that clips the
                                               // box photographs the void below its horizon.
                                               .farClippingPlane = 55000.0f}));

    // Nothing ticks until the world is whole. Started before the update callback is registered for
    // the same reason that callback is registered last: the first tick may come immediately.
    simulation->start();

    // Registered last, once the scene is fully built: the engine may call this the moment the
    // first tick runs, and a half-constructed scene is not something it should be handed.
    engine.onUpdate([this](float delta) { update(delta); });
    // Beside it, and on the frame rather than the tick: the dome's cadence is a statement about
    // how often the frame re-marches it, and it is read while that frame is being recorded.
    // `this->engine` because the constructor's own parameter shadows the member here, and the
    // lambda outlives the parameter.
    engine.onFrame([this] { cloudMarch.update(this->engine); });
}

CircuitScene::~CircuitScene()
{
    simulation->stop();
}

// Writers before readers, inside the stage the engine calls the game's own logic — and the three
// steps are now a handoff, a simulation tick and a handoff back.
//
// `publish` sends the main thread's decisions across (the setup sheet, the driver's keys) and has to
// run *before* the ticks it is meant to affect. `advance` runs exactly one engine tick's worth of
// simulation and waits for it under a capture, and returns immediately otherwise. `collect` takes
// one snapshot and draws everything from it, so the node, the rim, the sound and the camera are all
// the same instant of car rather than several fields sampled as the tick moved under them.
// Into the free camera and back out again.
//
// **It starts where the view already is**, which is the whole of what makes it useful: the free
// camera writes the direction from its own yaw and pitch on every tick (see FPSCameraController), so
// one constructed at a default would snap the view somewhere else the instant it engaged. Seeding it
// from the camera's current direction means pressing the key steps *out* of the car from exactly
// where you were looking.
//
// Leaving it puts back whichever controller this scene was built with. A scene launched with
// `OSR_CAMERA=fixed` was already the free camera and has nothing to return to, so the key does
// nothing there rather than stranding the view.
void CircuitScene::toggleFreeCamera()
{
    if (configuredCamera == CameraChoice::Fixed)
    {
        return;
    }

    freeCameraEngaged = !freeCameraEngaged;

    if (freeCameraEngaged)
    {
        chaseCamera.reset();
        cockpitCamera.reset();

        // Yaw about world up and pitch above the horizon, recovered from the direction the camera is
        // pointing this instant. atan2(x, z) rather than (z, x): zero yaw looks along positive z,
        // which is the convention the controller builds its direction back from.
        const auto direction = glm::normalize(camera.direction);
        const auto yaw = std::atan2(static_cast<double>(direction.x), static_cast<double>(direction.z));
        const auto pitch = std::asin(static_cast<double>(std::clamp(direction.y, -1.0f, 1.0f)));

        freeCamera.emplace(engine, yaw, pitch);

        // The meters link back the moment the view is outside the car: a free camera is one eye on
        // one world, and the frame's own meter — still carrying the rig's outdoor dial — resumes.
        // Unconditional because from the chase camera this is the state already. The car's rain
        // comes back with it: outside the car, its bodywork stands in the weather.
        linkExposure(engine, camera, *carCamera, *frameCamera, composite);
        carCamera->rainScale = 1.0f;

        return;
    }

    freeCamera.reset();

    // The cursor goes back to the desktop's: the free camera captured it for mouse-look, and a
    // driving view has no use for a pointer it has hidden and pinned.
    engine.window().setCursorMode(raceengine::CursorMode::Normal);

    if (configuredCamera == CameraChoice::Chase)
    {
        chaseCamera.emplace(engine);

        return;
    }

    cockpitCamera.emplace(engine);
    // Back into the seat, back onto split meters — the cabin's own dial included, and the cabin's
    // roof with it.
    splitExposure(engine, camera, *carCamera, *frameCamera);
    carCamera->autoExposure.compensation = cockpitCompensation;
    carCamera->rainScale = 0.0f;
}

// Printed as the command line that reproduces it rather than as a position and a direction, because
// what is wanted from it is never the numbers — it is standing where the person who saw something
// was standing. A direction vector cannot be pasted into `OSR_CAM_LOOK`, so this converts back to
// the yaw and pitch that produced it: atan2(x, z) rather than (z, x), since zero yaw looks along
// positive z, which is the convention `FPSCameraController` builds its direction from.
//
// It reports whatever camera is live, chase and cockpit included — those cannot be *restored* by the
// two variables, but a chase view is still a place worth being able to name, and refusing to print
// it would make the key useless from the one seat the game is normally driven from.
void CircuitScene::logCameraPose() const
{
    const auto metres = toMetres(glm::dvec3(camera.position));
    const auto direction = glm::normalize(camera.direction);
    const auto yaw = glm::degrees(std::atan2(static_cast<double>(direction.x), static_cast<double>(direction.z)));
    const auto pitch = glm::degrees(std::asin(static_cast<double>(std::clamp(direction.y, -1.0f, 1.0f))));

    engine.log().info("Camera here: OSR_SCENE=circuit OSR_CAMERA=fixed "
                      "OSR_CAM_POS={:.1f},{:.1f},{:.1f} OSR_CAM_LOOK={:.2f},{:.2f}",
                      metres.x, metres.y, metres.z, yaw, pitch);
}

void CircuitScene::update(float delta)
{
    player->publish();
    simulation->advance(Simulation::ticksPerEngineTick);
    player->collect();

    // The wipers, on the engine's own clock rather than a second one kept here: the shader inverts
    // the same sweep law to work out what the blade cleared, so the two must agree about what time
    // it is or the clearing lands where the blade is not.
    wipers.readStalk(engine.window().keyPressed(raceengine::Key::V));
    wipers.update(engine.simulatedSeconds());
    orThrow(engine.scene().setWipers(scene, wipers.state()));

    // The rain's drift, integrated against the fixed step rather than `delta` so a captured run's
    // frame N carries the same phase on every machine.
    //
    // **Unconditional now, and that is a seat-found correction** (2026-08-25, "the dirt moves,
    // mainly on roll" — dry, wipers off): this block used to be gated on `raining ||
    // wipers.running()` to keep a dry frame block byte-identical, but the body axes it publishes
    // are read in the dry frame too — the windscreen's grime stands in a pane basis built from
    // them — and a dry run left the scene's *defaults* under the glass, which are the world's
    // axes. The dirt then held the horizon while the car rolled beneath it, invisible on the
    // straight launch the gate captures and obvious from the seat in a corner. The car points
    // somewhere whatever the weather; the scene says so every tick, and the drift rides along
    // because one code path is one determinism argument. The speed and phase stay unread by every
    // dry shader, which is what the frame block's own comment promises.
    {
        // Ground speed, and specifically the horizontal part of it: this is the airstream the body
        // is driving into, so it is the chassis' velocity across the ground and never a wheel's —
        // a locked or spinning wheel says nothing about how fast the car is meeting the air. The
        // vertical component is the suspension breathing over kerbs and is not wind.
        const auto velocity = player->vehicle().chassis.linearVelocity;
        const auto speed = glm::length(glm::dvec2(velocity.x, velocity.z));

        // One time constant, and the drift follows it rather than the pedal. Both the rate and its
        // integral are taken from the damped speed, so the two never disagree about how fast the
        // air is moving — feeding the shader a damped rate against an undamped accumulation would
        // put the drops somewhere their own velocity says they cannot be.
        constexpr auto rainSpeedResponseSeconds = 0.4;
        const auto step = static_cast<double>(raceengine::Engine::fixedTimeStep);
        rainDampedSpeed += (speed - rainDampedSpeed) * (1.0 - std::exp(-step / rainSpeedResponseSeconds));

        // The airspeed the water answers to, and it stops climbing here. Drift grows with the
        // *square* of the speed, so without a ceiling a fast lap sends the streaks up the glass
        // faster than the eye can follow and the pane reads as static noise. Capped rather than
        // curved because a drop torn off the glass has left, and how fast it went on the way is not
        // something the screen can show. About 70 km/h, settled from the seat between one figure
        // that still ran too fast to read and a second that ran too slow. Applied before the
        // integral as well as to the rate, so a drop's position and its velocity never disagree.
        constexpr auto rainAirspeedCapMetresPerSecond = 19.5;
        const auto airspeed = std::min(rainDampedSpeed, rainAirspeedCapMetresPerSecond);
        rainAirflowPhase += airspeed * airspeed * step;

        // The body's own two axes, and **neither of them flattened**. Both rotate with the car, so
        // the pane and the forces on it tilt together and the projection between them — which is
        // all the drift reads — is invariant to pitch, roll and heading alike. A heading flattened
        // to the horizontal was the last version of this, and it leans against the body every time
        // the car pitches; a lean multiplied by a displacement that has been accumulating all lap
        // is what put the water on a shear rather than on a slide.
        const auto& orientation = player->vehicle().chassis.orientation;
        const auto forward = orientation * glm::dvec3(0.0, 0.0, 1.0);
        const auto bodyUp = orientation * glm::dvec3(0.0, 1.0, 0.0);

        orThrow(engine.scene().setRainMotion(
            scene, static_cast<float>(airspeed), static_cast<float>(rainAirflowPhase),
            glm::vec3(static_cast<float>(forward.x), static_cast<float>(forward.y), static_cast<float>(forward.z)),
            glm::vec3(static_cast<float>(bodyUp.x), static_cast<float>(bodyUp.y), static_cast<float>(bodyUp.z))));
    }

    // Level to edge, then act on the edge alone.
    const auto cameraKey = engine.window().keyPressed(raceengine::Key::C);
    if (cameraKey && !cameraKeyHeld)
    {
        toggleFreeCamera();
    }

    cameraKeyHeld = cameraKey;

    // Read before the controllers write, so what it prints is the view that was on screen when the
    // key went down rather than the one the next frame is about to show.
    const auto poseKey = engine.window().keyPressed(raceengine::Key::P);
    if (poseKey && !poseKeyHeld)
    {
        logCameraPose();
    }

    poseKeyHeld = poseKey;

    if (freeCamera)
    {
        freeCamera->update(camera, delta);
    }
    else if (chaseCamera)
    {
        chaseCamera->update(camera, player->vehicle(), player->acceleration(), delta);
    }
    else if (cockpitCamera)
    {
        cockpitCamera->update(camera, player->vehicle(), delta);
    }

    // The cockpit's split meters, carried across every tick the seat view is live: the cabin's
    // reading onto the shared lens, the world's onto the composite. A tick behind the meters, which
    // the adaptation's own seconds-long closing makes invisible.
    if (cockpitCamera)
    {
        applySplitExposure(engine, camera, *carCamera, *frameCamera, composite);
    }

    // The clouded sky's one scheduled probe re-photograph — on the tick count, and only when the
    // rig settled a non-zero coverage.
    cloudRecapture.update(engine, scene, cloudCoverage);

    // After whatever steered the pose camera, so all three views record this tick's eye.
    syncLayeredCameras(camera, *carCamera, *frameCamera);

    engine.sceneManager().setPosition(sky->node, camera.position.x, camera.position.y, camera.position.z);
}

} // namespace osr
