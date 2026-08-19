module;

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <utility>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

export module osr.game:PlayerCar;

import :SteeringController;
import :TrackFrame;

import raceengine;

namespace osr
{

// The car the driver is in: the vehicle model, the driveline that turns a key into wheel torque,
// and the scene node the whole of it is watched through.
//
// It is the game's and not the engine's. The engine has no notion of a vehicle, no registry to
// keep one in and no owner for the world it stands on, and `Engine::onUpdate` already runs at the
// top of the fixed tick in the stage the contract calls the game's own logic — writers before the
// entity behaviours and before the scene settles what they moved. Putting `stepVehicle` inside
// `Engine::update` would buy that same position at the price of the engine owning game data.
export class PlayerCar
{
    // Three substeps of the engine's 8.33 ms tick. The vehicle model is validated at 360 Hz and
    // 360 is exactly three times 120, so the division leaves no remainder and the car keeps no
    // accumulator of its own — which is what lets a capture land on the same simulated instant
    // every run, the tick count under `RACEENGINE_DUMP_FRAME` being a function of the frame
    // number.
    static constexpr int substeps = 3;

    // Per rear wheel, N.m. A handbrake is a cable to the rear brakes and is worth about a third of
    // what the pedal does there, which on this car's placeholder 1400 is nine hundred.
    static constexpr double handbrakeTorque = 900.0;

    // One pole at about a hertz on the acceleration the chase camera is driven from. What comes out
    // of a tick is a difference of two velocities across three substeps of contact impulses, and a
    // camera hung straight off that shakes; what a driver feels is the part of it that lasts.
    static constexpr double accelerationSmoothing = 0.16;

    // Where golf_gti_2018.glb's own origin sits in the chassis body frame, in metres.
    //
    // Measured off the asset rather than carried over from the car's data: its wheel centres are at
    // z +1.5040 and -1.1341 and its tyres meet y = 0, so its axle midpoint is 0.1850 m ahead of its
    // origin, where the hardpoint frame puts that midpoint at zero and the design contact patch at
    // y = 0. The number a car's own file states is for the mesh that shipped with it and is not
    // transferable: applied here it would bury this one by 77 mm.
    static constexpr double modelOriginAhead = -0.1850;

    raceengine::Engine& engine;
    const raceengine::PhysicsWorld& world;
    SceneNode& node;

    raceengine::VehicleSetup setup;
    raceengine::VehicleState state;
    raceengine::DrivelineSetup driveline;
    raceengine::DrivelineState drivelineState;
    SteeringController steering;

    // What the road put on each wheel on the substep before this one. The driveline needs it and it
    // does not exist until the vehicle tick that produces it has run, so it is carried across rather
    // than computed twice — one substep of lag at 360 Hz.
    std::array<double, raceengine::cornerCount> lastRoadTorques{};

    glm::dvec3 smoothedAcceleration{0.0};
    std::int32_t gear = 1;
    bool upshiftHeld = false;
    bool downshiftHeld = false;

public:
    // `grid` is where the body frame's origin goes, in metres: the design contact patch under the
    // wheelbase midpoint, so it is the point on the road the car is being stood on.
    PlayerCar(raceengine::Engine& engine, const raceengine::PhysicsWorld& world, SceneNode& node,
              const glm::dvec3& grid);

    void update(float delta);

    [[nodiscard]] const raceengine::VehicleState& vehicle() const
    {
        return state;
    }

    // World frame, m/s^2, smoothed. What the camera leans against.
    [[nodiscard]] const glm::dvec3& acceleration() const
    {
        return smoothedAcceleration;
    }

    [[nodiscard]] double groundSpeed() const
    {
        return glm::length(state.chassis.linearVelocity);
    }

    // Where the body frame's origin is standing, in metres — the point the model's own origin is
    // pinned to, and therefore what says whether the car is on the road or through it.
    [[nodiscard]] glm::dvec3 contactDatum() const
    {
        return raceengine::bodyToWorld(state.chassis, glm::dvec3(0.0, 0.0, 0.0));
    }

private:
    void writeNode() const;
};

} // namespace osr

namespace osr
{

PlayerCar::PlayerCar(raceengine::Engine& engine, const raceengine::PhysicsWorld& world, SceneNode& node,
                     const glm::dvec3& grid) :
    engine(engine),
    world(world),
    node(node),
    driveline(raceengine::placeholderDriveline()),
    steering(keyboardSteering())
{
    auto built = raceengine::placeholderSedan();
    if (!built)
    {
        throw std::runtime_error(built.error());
    }

    setup = std::move(built).value();

    // Standing a body up means knowing where its own centre of mass sits in its own frame, and the
    // vehicle model is what knows: `stepVehicle` assembles that ledger out of the sprung components
    // and the four unsprung masses on every tick. One inert tick asks it, and doubles as this
    // setup's first proof that it solves against this world at all rather than at the first frame
    // the driver sees.
    state.chassis.position = grid + glm::dvec3(0.0, 1.0, 0.0);
    if (const auto probed = raceengine::stepVehicle(setup, state, {}, raceengine::noDriveTorque, world, 1e-6); !probed)
    {
        throw std::runtime_error(probed.error());
    }

    const auto centreOfMass = state.chassis.centreOfMass;

    state = raceengine::VehicleState{};
    state.chassis.position = grid + centreOfMass;
    lastRoadTorques = {};

    // A default-constructed driveline is a car with the key out. Turning it is a command and not an
    // input, which is why it is a call here rather than a field the tick reads.
    raceengine::startEngine(driveline, drivelineState);

    writeNode();
}

void PlayerCar::update(const float delta)
{
    const auto tick = static_cast<double>(delta);
    const auto& window = engine.window();

    // A key is nought or one and a rack is neither, which is the whole of why the demand goes
    // through a controller rather than into the input.
    const auto demand = (window.keyPressed(Key::D) ? 1.0 : 0.0) - (window.keyPressed(Key::A) ? 1.0 : 0.0);

    const auto upshift = window.keyPressed(Key::LeftShift);
    const auto downshift = window.keyPressed(Key::LeftControl);

    // GLFW reports an edge and the window reports a level, so the edge has to be recovered here.
    // The fullscreen binding does not need this because the platform kept its edge; a polled key
    // has already lost it.
    if (upshift && !upshiftHeld)
    {
        gear = std::min(gear + 1, static_cast<std::int32_t>(driveline.gearbox.ratios.size()));
    }
    if (downshift && !downshiftHeld)
    {
        gear = std::max(gear - 1, -1);
    }

    upshiftHeld = upshift;
    downshiftHeld = downshift;

    auto input = raceengine::VehicleInput{};
    input.steering = steering.update(demand, groundSpeed(), tick);
    input.throttle = window.keyPressed(Key::W) ? 1.0 : 0.0;
    input.brake = window.keyPressed(Key::S) ? 1.0 : 0.0;
    input.gear = gear;

    const auto handbrake = window.keyPressed(Key::Space);
    const auto substepTime = tick / static_cast<double>(substeps);
    const auto entryVelocity = state.chassis.linearVelocity;
    const auto inertias = raceengine::wheelInertias(setup);

    for (auto step = 0; step < substeps; step++)
    {
        auto speeds = std::array<double, raceengine::cornerCount>{};
        for (auto index = std::size_t{0}; index < raceengine::cornerCount; index++)
        {
            speeds[index] = state.corners[index].wheelSpeed;
        }

        // The driveline is stepped from the game's loop rather than from inside the vehicle,
        // because which wheels a car drives is a property of the car and not of its suspension.
        // Its torques are recomputed here every substep rather than trusted from anywhere, which
        // is also why they are not a field on `VehicleInput`.
        const auto driven =
            raceengine::stepDriveline(driveline, drivelineState, speeds, inertias, lastRoadTorques, input, substepTime);
        if (!driven)
        {
            throw std::runtime_error(driven.error());
        }

        auto wheelTorques = driven->wheel;

        if (handbrake)
        {
            // The handbrake goes on where the rear wheels' torque does, because that is what it
            // physically is, and it is clamped against what would arrest the wheel inside one
            // substep for the reason the footbrake in the vehicle model is: a torque that turns a
            // wheel backwards reads to the tyre as enormous slip the other way, and locks by
            // oscillating instead of by stopping.
            for (auto index = std::size_t{2}; index < raceengine::cornerCount; index++)
            {
                const auto arresting = std::abs(speeds[index]) * inertias[index] / substepTime;
                wheelTorques[index] -= std::copysign(std::min(handbrakeTorque, arresting), speeds[index]);
            }
        }

        const auto stepped = raceengine::stepVehicle(setup, state, input, wheelTorques, world, substepTime);
        if (!stepped)
        {
            // Nothing here is a runtime condition: the setup was swept across its own travel at
            // load time and the linkage is clamped inside that range every tick, so a solve that
            // fails is a defect. It goes to main's boundary rather than being carried on from.
            throw std::runtime_error(stepped.error());
        }

        lastRoadTorques = raceengine::roadTorques(stepped.value());
    }

    const auto measured = (state.chassis.linearVelocity - entryVelocity) / tick;
    smoothedAcceleration += (measured - smoothedAcceleration) * (1.0 - std::exp(-tick / accelerationSmoothing));

    writeNode();
}

void PlayerCar::writeNode() const
{
    const auto origin = raceengine::bodyToWorld(state.chassis, glm::dvec3(0.0, 0.0, modelOriginAhead));
    const auto placed = toWorldUnits(origin);

    engine.sceneManager().setPosition(node, static_cast<float>(placed.x), static_cast<float>(placed.y),
                                      static_cast<float>(placed.z));

    // The scene graph is single precision throughout, so the body's attitude narrows here and it
    // does so component by component rather than through a conversion that would do it silently.
    const auto& attitude = state.chassis.orientation;
    engine.sceneManager().setOrientation(node,
                                         glm::quat(static_cast<float>(attitude.w), static_cast<float>(attitude.x),
                                                   static_cast<float>(attitude.y), static_cast<float>(attitude.z)));
}

} // namespace osr
