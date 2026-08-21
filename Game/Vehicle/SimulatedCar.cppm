module;

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

export module osr.game:SimulatedCar;

import :Options;
import :SteeringController;
import :TrackFrame;

import raceengine;

namespace osr
{

// What one tick of a car leaves behind for anything watching it, and the only thing that crosses
// from the simulation's thread to the one that draws.
//
// Everything in it is either trivially copyable or a small aggregate of doubles, which is what makes
// the handoff a memcpy under a lock nobody ever waits on. Note what is *not* here: no `VehicleStep`,
// no suspension solution, no contact patch. The sound is already derived — `deriveCarAudio` is pure
// and cheap and the tick has the step in hand, where the main thread would need the whole of it
// shipped across to ask the same question.
export struct CarSnapshot
{
    raceengine::VehicleState state{};
    // World frame, m/s^2, one-pole smoothed. What the chase camera leans against.
    glm::dvec3 acceleration{0.0};
    raceengine::CarAudioState audio{};
    // `VehicleInput::steering` as the tick used it, which is what a rendered rim falls back to when
    // there are no hands on a real one to mirror.
    double rackDemand = 0.0;
    std::int32_t gear = 1;
    std::int64_t ticks = 0;
};

// A car re-specified from a setup sheet: the two halves the sheet is allowed to move, built whole on
// the main thread and handed over.
//
// Built there rather than here because building one parses a file, allocates a torque curve and two
// vectors of mass components, and none of that belongs inside a fixed-rate deadline loop. What the
// tick does with it is a move.
export struct CarTune
{
    raceengine::VehicleSetup setup;
    raceengine::DrivelineSetup driveline;
};

// The tick half of the car the driver is in: the vehicle model, the driveline that turns a key into
// wheel torque, the steering rack the driver's hands are loaded through, and the recorder.
//
// **It runs on the simulation's thread and touches nothing the renderer owns.** No scene node, no
// mesh transform, no window. What it produces is a `CarSnapshot`; `PlayerCar` is the other half and
// is what turns that into a picture and a noise. The split is tick-versus-presentation and not
// "move the class", because half of what `PlayerCar` used to do is single-precision scene-graph
// writing that the renderer reads.
export class SimulatedCar
{
    // Per rear wheel, N.m. A handbrake is a cable to the rear brakes and is worth about a third of
    // what the pedal does there, which on this car's 525 a corner is a hundred and seventy-five.
    static constexpr double handbrakeTorque = 175.0;

    // One pole at about a hertz on the acceleration the chase camera is driven from. What comes out
    // of a tick is a difference of two velocities across a tick of contact impulses, and a camera
    // hung straight off that shakes; what a driver feels is the part of it that lasts.
    static constexpr double accelerationSmoothing = 0.16;

    // The gate's standing start, in ticks of the simulation's fixed 360 Hz step.
    //
    // Ticks and not seconds, because under `RACEENGINE_DUMP_FRAME` the simulation is driven a whole
    // number of ticks per rendered frame: the captured frame lands on the same simulated instant on
    // any machine, and a script keyed to the wall clock would not. It runs long enough past the
    // captured frame that no edge of it can land on that frame — the pedal comes up a full second
    // after the shutter. Two seconds and half a second, as they always were; the numbers are three
    // times what they were when this ran at the engine's 120 Hz and mean the same instants.
    static constexpr std::int64_t launchTicks = 720;
    static constexpr std::int64_t launchSteerTick = 180;
    static constexpr double launchSteering = 0.35;

    // Sixty seconds of telemetry at 120 Hz, which is the interesting-moment window a driver flags
    // after the fact: a ring, so what is kept is the *last* minute rather than the first, and long
    // enough that pressing the key after something happened still catches it.
    static constexpr std::size_t telemetryCapacity = 7200;

    // The tick runs at 360 Hz and the trace is written at 120, so every third one is recorded.
    //
    // Deliberate, not a leftover. A hundred and twenty samples a second is finer than the loggers
    // this file is meant to drop into beside — MoTeC i2 runs suspension channels at 100 to 200 Hz —
    // and recording every tick would spend the ring's whole minute in twenty seconds. The window is
    // the point of a ring; the extra bandwidth is not.
    static constexpr std::int64_t telemetryDivider = 3;

    raceengine::Engine& engine;
    const raceengine::PhysicsWorld& world;

    raceengine::VehicleSetup setup;
    raceengine::VehicleState state;
    raceengine::DrivelineSetup driveline;
    raceengine::DrivelineState drivelineState;
    SteeringController steering;

    // What the road put on each wheel on the tick before this one. The driveline needs it and it
    // does not exist until the vehicle tick that produces it has run, so it is carried across rather
    // than computed twice — one tick of lag at 360 Hz.
    std::array<double, raceengine::cornerCount> lastRoadTorques{};

    raceengine::TelemetryFrame lastTelemetry{};
    raceengine::DrivelineTorques lastDrivelineTorques{};
    raceengine::VehicleStep lastStep{};
    raceengine::VehicleInput lastInput{};

    // The steering box, as this car's own numbers rather than the model's defaults. The vehicle
    // setup carries a rack travel and the rim's rotation belongs to the steering box, and the two
    // together are the pinion radius that turns a rack force into the newton metres a driver's
    // hands feel.
    raceengine::SteeringRack rack;
    // Where the rack was on the previous tick, so its velocity is a difference and not a field
    // somebody has to remember to keep in step. **This is the number the whole threading change was
    // for.** Differenced across the engine's catch-up burst it came out double on one tick and zero
    // on the next — measured on the rig, exactly zero on 42.4% of all ticks — and the rack's Coulomb
    // friction chopped fully on and off with it at the frame rate. Two consecutive ticks of a
    // fixed-rate clock are two consecutive positions of a rack that is genuinely moving, and one
    // tick is the honest interval between them.
    double previousRackTravel = 0.0;

    DriverChoice driver;
    // Which of the three the controller above is presently shaped for. A wheel switched on halfway
    // through a session is a change of source and not a change of car, so the shaping follows it
    // and the rack does not move.
    raceengine::InputSourceKind shapedFor = raceengine::InputSourceKind::None;
    std::int64_t ticks = 0;

    glm::dvec3 smoothedAcceleration{0.0};
    std::int32_t gear = 1;
    bool upshiftHeld = false;
    bool downshiftHeld = false;

    // The handoff to whoever is drawing, and the asymmetry in it is deliberate: the tick publishes
    // with `try_lock` and never waits, the reader takes a plain lock and may. That is the rule this
    // whole change exists to keep — a fixed-rate clock that can be held up by a frame is not a
    // clock — and the reader is the main thread, which is allowed to wait a few hundred nanoseconds
    // for a memcpy. A dropped publish costs the picture one tick of freshness, which is 2.78 ms.
    // Under a capture the two threads strictly alternate and neither ever contends.
    mutable std::mutex publication;
    CarSnapshot published{};

    // A re-specified car, waiting to be taken at the top of a tick.
    mutable std::mutex tuneLock;
    std::optional<CarTune> pendingTune;

    // The recorder is the tick's, and the main thread reaches it only to swap a ring in or out —
    // two moves, at the rate a driver presses a key. `recording` is read without the lock so an
    // idle tick pays nothing at all.
    std::atomic<bool> recording{false};
    mutable std::mutex recorderLock;
    raceengine::TelemetryRecorder recorder{0};

public:
    // `grid` is where the body frame's origin goes, in metres: the design contact patch under the
    // wheelbase midpoint, so it is the point on the road the car is being stood on. `heading` is the
    // right-handed rotation about +y that takes the body's +z onto the direction it faces, in
    // radians — a grid slot states one and an AI line does not, which is most of why the slot is the
    // spawn.
    SimulatedCar(raceengine::Engine& engine, const raceengine::PhysicsWorld& world, const glm::dvec3& grid,
                 double heading, DriverChoice driver);

    SimulatedCar(const SimulatedCar&) = delete;
    SimulatedCar(SimulatedCar&&) = delete;
    SimulatedCar& operator=(const SimulatedCar&) = delete;
    SimulatedCar& operator=(SimulatedCar&&) = delete;

    // One tick, on the simulation's thread. `deltaTime` is the clock's own fixed step.
    void tick(double deltaTime);

    // The newest tick the publish got through with. Called from the thread that draws.
    [[nodiscard]] CarSnapshot snapshot() const;

    // Main thread. Applied at the top of the next tick, onto the whole car rather than onto the one
    // being driven — see `PlayerCar::reloadSetupIfChanged` for why a sheet must never be layered on
    // its own last application.
    void applyTune(CarTune tune);

    // Main thread, both of them, and the ring is allocated there. Starting installs a fresh one and
    // stopping takes it away, so the tick never allocates and never frees.
    void startRecording(raceengine::TelemetryRecorder fresh);
    [[nodiscard]] std::vector<raceengine::TelemetryFrame> stopRecording();

    [[nodiscard]] bool isRecording() const
    {
        return recording.load();
    }

    // What a fresh ring should be, so the main thread does not have to know the number.
    [[nodiscard]] static raceengine::TelemetryRecorder freshRecorder()
    {
        return raceengine::TelemetryRecorder{telemetryCapacity};
    }

    // What this car's own rim turns, lock to lock, in degrees. The Mk7 GTI's progressive rack is
    // 2.1 turns, which is what a device with more travel than that is geared against: a 900 degree
    // wheel reaches this car's full lock at eighty-four percent of its own travel and the rest is
    // past the stops, exactly as it would be in the car. It is stated here and not in the vehicle
    // setup because it is a property of the steering box and the vehicle model carries a rack
    // travel rather than a ratio.
    static constexpr double steeringLockToLock = 756.0;

    // The rate the trace is written at, for whoever reports how long a capture ran.
    static constexpr double telemetryHz = 120.0;

private:
    [[nodiscard]] raceengine::DriverInput demand();
    // Stage one, once per tick: what the road is doing to the steering, in newton metres at the rim.
    // It is computed whether or not anything is plugged in — the trace is the artefact, and a
    // channel that only exists when a wheel is attached is a channel nobody can compare a run
    // against.
    void publishRackTorque(const raceengine::VehicleStep& step, double rackTravel, double deltaTime);
    void publishSnapshot();
    void takeTune();

    [[nodiscard]] double groundSpeed() const
    {
        return glm::length(state.chassis.linearVelocity);
    }
};

} // namespace osr

namespace osr
{

SimulatedCar::SimulatedCar(raceengine::Engine& engine, const raceengine::PhysicsWorld& world, const glm::dvec3& grid,
                           const double heading, const DriverChoice driver) :
    engine(engine),
    world(world),
    driveline(raceengine::golfGtiMk7Driveline()),
    steering(keyboardSteering()),
    driver(driver)
{
    // The car the mesh already is. `placeholderSedan` stays what it has always been — a fixture
    // whose every figure was chosen so the model could be validated against a car with no data of
    // its own — and it is not what a game should be driving once a real one exists.
    auto built = raceengine::golfGtiMk7();
    if (!built)
    {
        throw std::runtime_error(built.error());
    }

    setup = std::move(built).value();

    rack.travelPerInput = setup.rackTravelPerInput;
    rack.lockToLockDegrees = steeringLockToLock;

    // Standing a body up means knowing where its own centre of mass sits in its own frame, and the
    // vehicle model is what knows: `stepVehicle` assembles that ledger out of the sprung components
    // and the four unsprung masses on every tick. One inert tick asks it, and doubles as this
    // setup's first proof that it solves against this world at all rather than at the first frame
    // the driver sees.
    const auto attitude = glm::angleAxis(heading, glm::dvec3(0.0, 1.0, 0.0));

    state.chassis.orientation = attitude;
    state.chassis.position = grid + glm::dvec3(0.0, 1.0, 0.0);
    if (const auto probed = raceengine::stepVehicle(setup, state, {}, raceengine::noDriveTorque, world, 1e-6); !probed)
    {
        throw std::runtime_error(probed.error());
    }

    const auto centreOfMass = state.chassis.centreOfMass;

    state = raceengine::VehicleState{};
    state.chassis.orientation = attitude;
    // The centre of mass is a point in the *body* frame, so a car placed facing anywhere but along
    // +z has to turn it before it can be subtracted: what is being pinned to the grid slot is the
    // body's origin, and `position` is where its centre of mass has to be for that to be true.
    state.chassis.position = grid + attitude * centreOfMass;
    lastRoadTorques = {};

    // A default-constructed driveline is a car with the key out. Turning it is a command and not an
    // input, which is why it is a call here rather than a field the tick reads.
    raceengine::startEngine(driveline, drivelineState);

    // What the device is set to is the device's; what the car needs is the car's. Told here because
    // this is the only thing that knows the steering box, and the mapping between the two is what
    // stops a nine hundred degree wheel steering this car like a go-kart.
    engine.input().setVehicleRotation(steeringLockToLock);

    // So that the first frame drawn has a car in it rather than a default-constructed one: the
    // renderer may collect a snapshot before the simulation's thread has run a single tick.
    publishSnapshot();
}

// Whoever is driving, or the gate's standing start. Which one is a property of the run and is
// settled before the first tick; what comes out of either is the same struct, and the shaping stage
// below is the only thing that ever learns which device answered.
raceengine::DriverInput SimulatedCar::demand()
{
    if (driver == DriverChoice::Launch)
    {
        // Full throttle from rest in first, and a steering input half a second later. What that
        // puts in front of the driving gate is the whole chain the parked car left out: the torque
        // curve away from idle, the clutch, the gearbox and final drive, the differential's split,
        // longitudinal slip at the driven wheels, the load transfer that squats the car, and then
        // the tyre's lateral model and the camera's lean against all of it.
        //
        // It does not go through the input service on purpose: a scripted run must be a function of
        // the tick count and of nothing else, and a device attached to the machine the gate happens
        // to be running on is exactly the sort of thing that would put a different image on disk.
        return raceengine::DriverInput{.steering = ticks < launchSteerTick ? 0.0 : launchSteering,
                                       .throttle = ticks < launchTicks ? 1.0 : 0.0};
    }

    const auto asked = engine.input().sample();

    if (const auto kind = engine.input().activeKind(); kind != shapedFor)
    {
        steering.reconfigure(kind == raceengine::InputSourceKind::Wheel     ? wheelSteering()
                             : kind == raceengine::InputSourceKind::Gamepad ? gamepadSteering()
                                                                            : keyboardSteering());
        shapedFor = kind;
    }

    return asked;
}

void SimulatedCar::applyTune(CarTune tune)
{
    const auto guard = std::lock_guard<std::mutex>(tuneLock);
    pendingTune = std::move(tune);
}

void SimulatedCar::takeTune()
{
    auto held = std::unique_lock<std::mutex>(tuneLock, std::try_to_lock);
    if (!held.owns_lock() || !pendingTune)
    {
        return;
    }

    // The move frees the car this one replaces — a torque curve and two small vectors — on this
    // thread rather than on the one that built it. That is one free at the rate a human saves a
    // file, against a 2.78 ms budget, and it is the price of the tick owning its own setup rather
    // than reading one somebody else may be writing.
    setup = std::move(pendingTune->setup);
    driveline = std::move(pendingTune->driveline);
    pendingTune.reset();

    // The wheel's own geometry follows the car's. Left behind, an inverted rack would steer the car
    // one way and load the driver's hands the other — a self-centring force pushing *into* the
    // corner, which is the least diagnosable thing a wheel can do.
    rack.travelPerInput = setup.rackTravelPerInput;
}

void SimulatedCar::startRecording(raceengine::TelemetryRecorder fresh)
{
    {
        const auto guard = std::lock_guard<std::mutex>(recorderLock);
        recorder = std::move(fresh);
    }

    recording.store(true);
}

std::vector<raceengine::TelemetryFrame> SimulatedCar::stopRecording()
{
    recording.store(false);

    auto taken = raceengine::TelemetryRecorder{0};

    {
        // Two moves and nothing else under the lock. `inOrder` allocates and copies seven thousand
        // frames, which is not something a fixed-rate tick should ever be made to wait behind.
        const auto guard = std::lock_guard<std::mutex>(recorderLock);
        std::swap(taken, recorder);
    }

    return taken.inOrder();
}

CarSnapshot SimulatedCar::snapshot() const
{
    const auto guard = std::lock_guard<std::mutex>(publication);

    return published;
}

void SimulatedCar::publishSnapshot()
{
    const auto next = CarSnapshot{
        .state = state,
        .acceleration = smoothedAcceleration,
        // The tick's own, from the same step the force feedback came from and for the same reason:
        // the newest thing this tick knows. Derived here rather than shipped as a `VehicleStep` for
        // the main thread to derive, because this is pure arithmetic over data the tick already has
        // in registers and the alternative is copying a suspension solution across a lock.
        .audio = raceengine::deriveCarAudio(driveline, drivelineState, lastDrivelineTorques, state, lastStep, lastInput),
        .rackDemand = lastInput.steering,
        .gear = gear,
        .ticks = ticks};

    auto held = std::unique_lock<std::mutex>(publication, std::try_to_lock);
    if (!held.owns_lock())
    {
        return;
    }

    published = next;
}

void SimulatedCar::tick(const double deltaTime)
{
    takeTune();

    const auto asked = demand();

    ticks++;

    // A device reports an edge and this reads a level, so the edge has to be recovered here.
    if (asked.upshift && !upshiftHeld)
    {
        gear = std::min(gear + 1, static_cast<std::int32_t>(driveline.gearbox.ratios.size()));
    }
    if (asked.downshift && !downshiftHeld)
    {
        gear = std::max(gear - 1, -1);
    }

    upshiftHeld = asked.upshift;
    downshiftHeld = asked.downshift;

    auto input = raceengine::VehicleInput{};
    // A key is nought or one and a rack is neither, which is the whole of why the demand goes
    // through a controller rather than into the input.
    input.steering = steering.update(asked.steering, groundSpeed(), deltaTime);
    input.throttle = asked.throttle;
    input.brake = asked.brake;
    input.gear = gear;

    const auto entryVelocity = state.chassis.linearVelocity;
    const auto inertias = raceengine::wheelInertias(setup);

    auto speeds = std::array<double, raceengine::cornerCount>{};
    for (auto index = std::size_t{0}; index < raceengine::cornerCount; index++)
    {
        speeds[index] = state.corners[index].wheelSpeed;
    }

    // The driveline is stepped from the game's loop rather than from inside the vehicle, because
    // which wheels a car drives is a property of the car and not of its suspension. Its torques are
    // recomputed here every tick rather than trusted from anywhere, which is also why they are not
    // a field on `VehicleInput`.
    const auto driven =
        raceengine::stepDriveline(driveline, drivelineState, speeds, inertias, lastRoadTorques, input, deltaTime);
    if (!driven)
    {
        throw std::runtime_error(driven.error());
    }

    auto wheelTorques = driven->wheel;

    if (asked.handbrake)
    {
        // The handbrake goes on where the rear wheels' torque does, because that is what it
        // physically is, and it is clamped against what would arrest the wheel inside one tick for
        // the reason the footbrake in the vehicle model is: a torque that turns a wheel backwards
        // reads to the tyre as enormous slip the other way, and locks by oscillating instead of by
        // stopping.
        for (auto index = std::size_t{2}; index < raceengine::cornerCount; index++)
        {
            const auto arresting = std::abs(speeds[index]) * inertias[index] / deltaTime;
            wheelTorques[index] -= std::copysign(std::min(handbrakeTorque, arresting), speeds[index]);
        }
    }

    const auto stepped = raceengine::stepVehicle(setup, state, input, wheelTorques, world, deltaTime);
    if (!stepped)
    {
        // Nothing here is a runtime condition: the setup was swept across its own travel at load
        // time and the linkage is clamped inside that range every tick, so a solve that fails is a
        // defect. It goes to main's boundary rather than being carried on from.
        throw std::runtime_error(stepped.error());
    }

    lastRoadTorques = raceengine::roadTorques(stepped.value());
    lastTelemetry = stepped->telemetry;
    lastDrivelineTorques = driven.value();
    lastStep = stepped.value();
    lastInput = input;

    // Every tick now, which is the whole point of the clock: a 500 Hz writer sees a fresh value on
    // nearly every output frame instead of one in eight, and `RackFeedback::publishInterval` states
    // a period this publish genuinely keeps.
    publishRackTorque(stepped.value(), input.steering * setup.rackTravelPerInput, deltaTime);

    // Opt-in, because a line a second is noise in a log nobody is reading and the only thing worth
    // having when the car is not going where it is pointed. `OSR_LOG_INPUT=1`.
    //
    // It prints the demand, what the controller made of it, and what the car did with it — the three
    // numbers that separate "the input never arrived", "the input arrived and did nothing" and "the
    // input arrived and did the opposite", which are indistinguishable from the driver's seat and
    // are the three things worth telling apart.
    static const auto logInput = std::getenv("OSR_LOG_INPUT") != nullptr;

    if (logInput && ticks % 360 == 0)
    {
        const auto toBody = glm::conjugate(state.chassis.orientation);
        const auto drift = toBody * state.chassis.linearVelocity;

        // **Negated on the way out, and the label is the whole reason this line exists.** `drift.x` is
        // along the body's +x, and this body frame's +x is the car's *left* — the same convention the
        // cockpit seat had to be measured off a picture to establish. Printed raw under a heading that
        // said "+ is the car's right", this line reported a car turning left as a car turning right,
        // which is worse than not printing it: it is the instrument you reach for when the steering is
        // in doubt, answering confidently and backwards.
        engine.log().info("input: demand {:+.3f} -> rack {:+.3f} ({:+.4f} m), speed {:.1f} m/s, yaw rate {:+.3f}, "
                          "sideways {:+.2f} m/s (+ is the car's right)",
                          asked.steering, input.steering, input.steering * setup.rackTravelPerInput, groundSpeed(),
                          (toBody * raceengine::angularVelocity(state.chassis)).y, -drift.x);
    }

    const auto measured = (state.chassis.linearVelocity - entryVelocity) / deltaTime;
    smoothedAcceleration += (measured - smoothedAcceleration) * (1.0 - std::exp(-deltaTime / accelerationSmoothing));

    if (recording.load() && ticks % telemetryDivider == 0)
    {
        // The tick's own frame, with the driveline's channels written into it beside the vehicle's,
        // so a run drops into i2 with the engine and the suspension on the same time base.
        auto frame = lastTelemetry;
        frame.time = static_cast<double>(ticks) * deltaTime;
        raceengine::fillDrivelineTelemetry(frame, drivelineState, lastDrivelineTorques);

        const auto guard = std::lock_guard<std::mutex>(recorderLock);
        recorder.record(frame);
    }

    publishSnapshot();
}

// The whole of the game's side of force feedback, and there is deliberately very little of it: what
// the engine wants is newton metres at the rim, and everything that turns a tick's result into that
// is `steeringRackTorque`. Nothing here knows what device is attached or whether one is.
void SimulatedCar::publishRackTorque(const raceengine::VehicleStep& stepped, const double rackTravel,
                                     const double deltaTime)
{
    // The tyre model and the ray cast both answer in world coordinates and the kinematic solve
    // answers in the chassis frame. The kingpin axis is the chassis's, so the forces come to it
    // rather than the other way about — one conjugation per corner against three per force.
    const auto toBody = glm::conjugate(state.chassis.orientation);

    auto corners = std::array<raceengine::SteeredCorner, raceengine::steeredCornerLimit>{};

    for (auto index = std::size_t{0}; index < raceengine::steeredCornerLimit; index++)
    {
        const auto& solution = stepped.corners[index];
        const auto& suspension = solution.suspension;
        const auto& hardpoints = setup.corners[index].hardpoints;

        const auto worldForce = glm::dvec3(0.0, solution.forces.tireVertical, 0.0) +
                                solution.contact.tyre.longitudinal * solution.contact.forward +
                                solution.contact.tyre.lateral * solution.contact.lateral;

        corners[index] =
            raceengine::SteeredCorner{.lowerBallJoint = suspension.lowerBallJoint,
                                      .upperBallJoint = suspension.upperBallJoint,
                                      .steeringArm = suspension.steeringArm,
                                      .rackOuter = hardpoints.steeringRackOuter + glm::dvec3(rackTravel, 0.0, 0.0),
                                      .contactPatch = suspension.contactPatch,
                                      .patchNormal = toBody * solution.patch.normal,
                                      .tyreForce = toBody * worldForce,
                                      .aligningMoment = solution.contact.tyre.aligningMoment};
    }

    // The tick's own interval, plainly — **and the plainness is what the fixed-rate clock finally
    // makes true** (2026-08-21).
    //
    // Two policies preceded it and both were repairs to the same wound. Differenced over the device
    // sample's interval, with a stale-stamp rule holding the previous velocity for one tick and
    // zeroing it after, this stood a mid-correction friction torque on a still rim; differenced over
    // the tick with the demand reconstructed across the burst, it stopped standing and started
    // chopping, because the reconstruction could only ever be an interpolation of a staircase. What
    // both were working around is that a catch-up burst's ticks all read one device report. A
    // fixed-rate thread has no burst: consecutive ticks are consecutive positions of a rack that is
    // genuinely moving, and one tick is the honest denominator.
    const auto sampleNanos = engine.input().sampleTimestampNanos();
    const auto rackVelocity = deltaTime > 0.0 ? (rackTravel - previousRackTravel) / deltaTime : 0.0;

    previousRackTravel = rackTravel;

    const auto torque = raceengine::steeringRackTorque(rack, corners, rackVelocity);

    // The Mk7's rack is electrically assisted, and the assist is not a nicety — it is what makes
    // low-speed steering renderable at all. The unassisted rack this derivation answers with is
    // the honest physics (criterion 10 is checked against it), but a parked car's carcass spring
    // is about 2.5 N·m per rim degree's worth of rack at it, so ±1.6 mm of travel saturates the
    // sheet's whole 4 N·m ceiling: the motor becomes a gradient-free bang-bang relay, which the
    // 2026-08-21 exit traces show as a 10 Hz limit cycle slamming rail to rail — the shake that
    // wanted the wheel out of Dominic's hands, whose only damping was his grip. The real car does
    // not feel like that because the real car's EPS absorbs precisely these efforts: parking a
    // Mk7 is 3–4 N·m at the rim, not 17. Strongest at rest, lightening with speed, which is what
    // every electric rack's speed map does.
    constexpr auto assistAtRest = 0.22;
    constexpr auto assistAtSpeed = 0.55;
    constexpr auto assistBuildSpeed = 8.0;
    const auto speed = groundSpeed();
    const auto assisted = assistAtRest + (assistAtSpeed - assistAtRest) * (speed / (speed + assistBuildSpeed));

    // The rack's parking damper, scheduled by the same speed the assist is: the assisted carcass
    // spring is ~6 N·m/rad at the rim, so a released rim needs about 2·sqrt(K·J) ≈ 0.5 of damping
    // to glide back to centre instead of shimmying around it — which is what the seat reported at
    // a standstill — while at speed the driver's own sheet dial (0.1 felt best on a full lap) is
    // the whole story. Fades over a few metres per second, and rides on the dial rather than
    // replacing it.
    constexpr auto parkingDamping = 0.4;
    constexpr auto parkingFadeSpeed = 5.0;
    const auto scheduledDamping = parkingDamping * (1.0 - speed / (speed + parkingFadeSpeed));

    // A stage-one refusal is forwarded as one rather than smoothed into a zero. Zero is a torque and
    // is a perfectly good thing for the wheel to hold; what has happened here is that the physics
    // handed the steering a number that is not one, and the honest answer to that is to let go.
    // Every published channel carries the assist, so the trace's ratios stay meaningful.
    engine.forceFeedback().publish(raceengine::RackFeedback{
        .steeringTorque = torque.finite ? torque.steeringTorque * assisted : std::numeric_limits<double>::quiet_NaN(),
        .rackForce = torque.rackForce * assisted,
        .tyreRackForce = torque.tyreForce * assisted,
        .rackTravel = rackTravel,
        .rackVelocity = rackVelocity,
        // One simulation tick: what one publish is worth in simulated time, which is the slope the
        // writer's reconstruction may continue. The wall clock cannot say this — see `RackFeedback`.
        .publishInterval = deltaTime,
        .damping = scheduledDamping,
        .inputTimestampNanos = sampleNanos});
}

} // namespace osr
