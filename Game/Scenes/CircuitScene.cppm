module;

#include <optional>
#include <string>
#include <utility>

#include <glm/glm.hpp>

export module osr.game:CircuitScene;

import :CarEntity;
import :ChaseCameraController;
import :FPSCameraController;
import :Options;
import :PlayerCar;
import :RaceTrack;
import :RenderRig;
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
    // the render rig creates down there out of a file this scene awaits.
    std::optional<CarEntity> car;
    std::optional<PlayerCar> player;

public:
    CircuitScene(raceengine::Engine& engine, const RunOptions& options);
    void update(float delta);
};

} // namespace osr

namespace osr
{

CircuitScene::CircuitScene(raceengine::Engine& engine, const RunOptions& options) :
    engine(engine),
    scene(engine.sceneManager().createScene()),
    camera(orThrow(engine.scene().createCamera(scene))),
    trackModel(orThrow(engine.resource().loadModelAsync(std::string(trackAsset)).get())),
    track(orThrow(raceengine::PhysicsWorld::create(orThrow(trackCollisionMesh(engine.memoryStorage(), trackModel)))))
{
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
    else
    {
        // Hell Corner from above and behind, and nothing gates it — this is the view to fly the
        // circuit from by hand. It was the rendering gate's framing for a while and was a poor one:
        // three flat regions, no car, nothing with a texture on it, and not a pixel either clipped
        // or under 2/255. The apron scene is the fixture now.
        const auto stand = toWorldUnits(glm::dvec3(-60.0, 55.0, -545.0));
        engine.camera().setPosition(camera, static_cast<float>(stand.x), static_cast<float>(stand.y),
                                    static_cast<float>(stand.z));
        freeCamera.emplace(engine, -2.1376, -0.2549);
    }

    sky = &buildRenderRig(engine, scene, camera);

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
    player.emplace(engine, track, car->sceneNode(), slot.position, glm::radians(slot.yaw), options.driver);

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
                                               .farClippingPlane = 4000.0f}));

    // Registered last, once the scene is fully built: the engine may call this the moment the
    // first tick runs, and a half-constructed scene is not something it should be handed.
    engine.onUpdate([this](float delta) { update(delta); });
}

// Writers before readers, inside the stage the engine calls the game's own logic: the car is
// stepped and writes its node, the camera is aimed at where the car ended up, and the sky is put
// back on the camera. Entity behaviours and the scene's own settling both run after this returns.
void CircuitScene::update(float delta)
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
