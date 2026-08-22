module;

#include <optional>
#include <string>
#include <utility>

#include <glm/glm.hpp>

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

    RenderableModel* sky;
    // Built in the body rather than the initialiser list because both need the "pbr" shader, which
    // the render rig creates down there out of a file this scene awaits.
    std::optional<CarEntity> car;
    std::optional<PlayerCar> player;

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
    // Measured rather than guessed: the chase view settles at 19.2, and seeding anywhere near the
    // apron's 2.5 leaves a 120-frame capture still climbing.
    engine.camera().setExposure(camera, 18.0f);

    // Never lookAtPoint: whichever controller is engaged below writes the direction on every tick,
    // so a framing stated here would be overwritten before the first frame — which is precisely
    // what happened to the aerial spawn view this scene used to have, for long enough to be worth a
    // note in the engine's own documentation.
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
        const auto stand = toWorldUnits(glm::dvec3(-660.0, 195.8, 1216.0)); // TEMP fence-shadow probe
        engine.camera().setPosition(camera, static_cast<float>(stand.x), static_cast<float>(stand.y),
                                    static_cast<float>(stand.z));
        freeCamera.emplace(engine, 1.5708, -0.08); // TEMP look along +x
    }

    // Three kilometres: outside every point of the circuit from every point a camera can stand.
    sky = &buildRenderRig(engine, scene, camera, 30000.0f);

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
        // A cockpit is a different photographic problem from every other view this game has, which
        // is why the rig's compensation does not survive it. Outside the car the subject is dark
        // against a bright sky and the meter has to be told to open up for it; from the seat the
        // *frame* is a dark cabin with a bright hole in the middle of it, and the rig's centre
        // weighting points the meter straight down that hole. Left at +1.50 EV the windscreen
        // prints as paper and takes the dashboard with it.
        //
        // **After the rig and not before it.** `AutoExposureService::enable` assigns the whole meter,
        // so an override written earlier is overwritten by the rig — which is exactly what happened,
        // and it presented as a compensation that changed nothing at all: two runs a stop apart both
        // settled on an exposure of 18.18.
        //
        // Compensation rather than the weighting, because it is the one of the two that is read
        // live: the weighting is written into the reduction pass when metering is enabled.
        camera.autoExposure.compensation = cockpitCompensation;
    }

    // The circuit itself — the visual export, while the surfaces being driven on come from the
    // physics export in the same world coordinates. There is no position to state: both carry their
    // own world coordinates and `trackOrigin` is zero, so the only thing between the two is the
    // tenth of a metre a world unit is. Anything else here and the surface being driven on would be
    // somewhere the surface being drawn is not, which reads as the car floating or sinking rather
    // than as a transform error.
    auto& trackEntity = engine.scene().createEntity(
        scene, CreateRenderableModelDTO{.node = engine.sceneManager().createNode(scene),
                                        .shader = engine.shader().getShaderByName("pbr").value(),
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
    simulatedCar = &simulation->add(slot.position, glm::radians(slot.yaw), options.driver,
                                    0.001 * options.beltBridgingMillimetres);
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
void CircuitScene::update(float delta)
{
    player->publish();
    simulation->advance(Simulation::ticksPerEngineTick);
    player->collect();

    if (chaseCamera)
    {
        chaseCamera->update(camera, player->vehicle(), player->acceleration(), delta);
    }
    else if (cockpitCamera)
    {
        cockpitCamera->update(camera, player->vehicle());
    }
    else if (freeCamera)
    {
        freeCamera->update(camera, delta);
    }

    engine.sceneManager().setPosition(sky->node, camera.position.x, camera.position.y, camera.position.z);
}

} // namespace osr
