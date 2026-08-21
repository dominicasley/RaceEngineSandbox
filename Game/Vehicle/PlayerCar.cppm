module;

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <utility>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

export module osr.game:PlayerCar;

import :Options;
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
    // what the pedal does there, which on this car's 525 a corner is a hundred and seventy-five.
    static constexpr double handbrakeTorque = 175.0;

    // One pole at about a hertz on the acceleration the chase camera is driven from. What comes out
    // of a tick is a difference of two velocities across three substeps of contact impulses, and a
    // camera hung straight off that shakes; what a driver feels is the part of it that lasts.
    static constexpr double accelerationSmoothing = 0.16;

    // The gate's standing start, in ticks of the engine's fixed 120 Hz step.
    //
    // Ticks and not seconds, because under `RACEENGINE_DUMP_FRAME` the tick count is a function of
    // the frame number: the captured frame lands on the same simulated instant on any machine, and
    // a script keyed to the wall clock would not. It runs long enough past the captured frame that
    // no edge of it can land on that frame — the pedal comes up a full second after the shutter.
    static constexpr std::int64_t launchTicks = 240;
    // Half a second in, so the launch and the first steering input are separable in the picture.
    static constexpr std::int64_t launchSteerTick = 60;
    static constexpr double launchSteering = 0.35;

    // Where golf_gti_2018.glb's own origin sits in the chassis body frame, in metres.
    //
    // Measured off the asset rather than carried over from the car's data: its wheel centres are at
    // z +1.5040 and -1.1341 and its tyres meet y = 0, so its axle midpoint is 0.1850 m ahead of its
    // origin, where the hardpoint frame puts that midpoint at zero and the design contact patch at
    // y = 0. The number a car's own file states is for the mesh that shipped with it and is not
    // transferable: applied here it would bury this one by 77 mm.
    static constexpr double modelOriginAhead = -0.1850;

    // What this car's own rim turns, lock to lock, in degrees. The Mk7 GTI's progressive rack is
    // 2.1 turns, which is what a device with more travel than that is geared against: a 900 degree
    // wheel reaches this car's full lock at eighty-four percent of its own travel and the rest is
    // past the stops, exactly as it would be in the car. It is stated here and not in the vehicle
    // setup because it is a property of the steering box and the vehicle model carries a rack
    // travel rather than a ratio.
    static constexpr double steeringLockToLock = 756.0;

    raceengine::Engine& engine;
    const raceengine::PhysicsWorld& world;
    SceneNode& node;
    RenderableModel& carRenderable;

    // The steering wheel's spin axis in the STEER_HR node's own frame, **measured off the asset
    // rather than assumed**: the disc's smallest-variance axis by PCA over its vertices, because
    // the export keeps the column rake in the vertex data and the node's local frame axis-aligned.
    // The node origin sits on this axis to within 0.3 mm — measured the same way — so rotating
    // about it through the origin spins the rim about the column. If the asset is re-exported with
    // its local +Z on the column, this becomes (0, 0, 1) and nothing else moves.
    static constexpr glm::vec3 steeringWheelAxis{0.0f, -0.92315f, 0.38443f};

    // Index into the renderable's meshes, found once by name; nothing if the model carries no
    // STEER_HR, which is a car whose wheel simply does not turn rather than an error.
    std::optional<std::size_t> steeringWheelMesh;

    raceengine::VehicleSetup setup;
    raceengine::VehicleState state;
    raceengine::DrivelineSetup driveline;
    raceengine::DrivelineState drivelineState;
    SteeringController steering;

    // What the road put on each wheel on the substep before this one. The driveline needs it and it
    // does not exist until the vehicle tick that produces it has run, so it is carried across rather
    // than computed twice — one substep of lag at 360 Hz.
    std::array<double, raceengine::cornerCount> lastRoadTorques{};

    // The newest substep's, kept so the recorder can be filled once per tick rather than three
    // times: three samples microseconds apart with 8.33 ms of simulated time between them is a
    // trace whose every derivative is wrong by a factor of three.
    raceengine::TelemetryFrame lastTelemetry{};
    raceengine::DrivelineTorques lastDrivelineTorques{};
    raceengine::VehicleStep lastStep{};
    raceengine::VehicleInput lastInput{};

    // The steering box, as this car's own numbers rather than the model's defaults. It is stated
    // here for the reason `steeringLockToLock` is: the vehicle setup carries a rack travel and the
    // rim's rotation belongs to the steering box, and the two together are the pinion radius that
    // turns a rack force into the newton metres a driver's hands feel.
    raceengine::SteeringRack rack;
    // Where the rack was on the previous tick, so its velocity is a difference and not a field
    // somebody has to remember to keep in step. One field and no stamp: the demand is reconstructed
    // across the engine's catch-up burst now (`CatchUpPhase`), so consecutive ticks are consecutive
    // positions of a rack that is genuinely moving and the tick is the honest interval between
    // them. See `publishRackTorque` for the two policies this replaced and what each of them did to
    // the rack's friction term.
    double previousRackTravel = 0.0;

    DriverChoice driver;
    // Which of the three the controller above is presently shaped for. A wheel switched on halfway
    // through a session is a change of source and not a change of car, so the shaping follows it
    // and the rack does not move.
    raceengine::InputSourceKind shapedFor = raceengine::InputSourceKind::None;
    std::int64_t ticks = 0;

    // Where the setup sheet lives and what it looked like when it was last read. A path rather than
    // a watcher: `last_write_time` is one stat call a frame and needs no thread, no inotify and no
    // second way for the game to be told something, and the thing being watched changes at the speed
    // a human edits a file.
    // Sixty seconds at 120 Hz, which is the interesting-moment window a driver flags after the fact:
    // a ring, so what is kept is the *last* minute rather than the first, and long enough that
    // pressing the key after something happened still catches it.
    static constexpr std::size_t telemetryCapacity = 7200;

    raceengine::TelemetryRecorder recorder{telemetryCapacity};
    bool recording = false;
    bool telemetryHeld = false;
    std::int32_t captures = 0;

    std::filesystem::path setupPath;
    std::filesystem::file_time_type setupStamp{};
    bool setupSeen = false;
    bool setupMissingReported = false;

    glm::dvec3 smoothedAcceleration{0.0};
    std::int32_t gear = 1;
    bool upshiftHeld = false;
    bool downshiftHeld = false;

public:
    // `grid` is where the body frame's origin goes, in metres: the design contact patch under the
    // wheelbase midpoint, so it is the point on the road the car is being stood on. `heading` is the
    // right-handed rotation about +y that takes the body's +z onto the direction it faces, in
    // radians — a grid slot states one and an AI line does not, which is most of why the slot is the
    // spawn.
    PlayerCar(raceengine::Engine& engine, const raceengine::PhysicsWorld& world, SceneNode& node,
              RenderableModel& carRenderable, const glm::dvec3& grid, double heading, DriverChoice driver);

    // An attended session leaves its steering evidence behind without being asked: the last five
    // minutes of the rack trace and the force feedback service's own summary, written beside the
    // binary on the way out. A wheel complaint is settled by reading these off disk after the
    // drive, rather than by the driver describing a vibration in words — nothing is written on an
    // unattended run, whose gates have no wheel and no driver.
    ~PlayerCar();

    PlayerCar(const PlayerCar&) = delete;
    PlayerCar(PlayerCar&&) = delete;
    PlayerCar& operator=(const PlayerCar&) = delete;
    PlayerCar& operator=(PlayerCar&&) = delete;

    void update(float delta);

    // One stat a frame, and a reload when the answer changes. Public so a test or a tool can ask for
    // it directly rather than having to run a frame.
    void reloadSetupIfChanged();

    // The driver's own hand on the recorder, from the seat. Public for the same reason the reload
    // is: something other than a keypress may want to ask for it.
    void toggleTelemetry();

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
    [[nodiscard]] raceengine::DriverInput demand();
    void writeNode() const;
    // Stage one, once per tick: what the road is doing to the steering, in newton metres at the rim.
    // It is computed whether or not anything is plugged in — the trace is the artefact, and a
    // channel that only exists when a wheel is attached is a channel nobody can compare a run
    // against.
    void publishRackTorque(const raceengine::VehicleStep& step, double rackTravel, double deltaTime);
};

} // namespace osr

namespace osr
{

PlayerCar::PlayerCar(raceengine::Engine& engine, const raceengine::PhysicsWorld& world, SceneNode& node,
                     RenderableModel& carRenderable, const glm::dvec3& grid, const double heading,
                     const DriverChoice driver) :
    engine(engine),
    world(world),
    node(node),
    carRenderable(carRenderable),
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

    // The sheet this car is tuned by, if there is one. Read on the first update rather than here, so
    // that a file appearing mid-session is picked up on the same path as a file being edited — one
    // code path for "there is a setup now" and "the setup changed", which is one fewer way for the
    // two to disagree.
    setupPath = "assets/Setups/golf-gti-mk7.setup";

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

    // The wheel that turns, found once by the name the asset tags it with. Said when it is absent,
    // because a model without it is a car whose wheel silently stops turning after a re-export.
    for (auto index = std::size_t{0}; index < carRenderable.meshes.size(); index++)
    {
        const auto* mesh = engine.memoryStorage().meshes.find(carRenderable.meshes[index].mesh);
        if (mesh != nullptr && mesh->name == "STEER_HR")
        {
            steeringWheelMesh = index;
            break;
        }
    }

    if (!steeringWheelMesh)
    {
        engine.log().info("This car's model carries no STEER_HR node, so its steering wheel will not turn");
    }

    writeNode();
}

// Whoever is driving, or the gate's standing start. Which one is a property of the run and is
// settled before the first tick; what comes out of either is the same struct, and the shaping stage
// below is the only thing that ever learns which device answered.
raceengine::DriverInput PlayerCar::demand()
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

// A setup sheet, read again whenever it changes on disk.
//
// The whole point of it is iteration speed, so the failure modes are chosen for somebody who is
// mid-edit: a file that does not exist is not an error, a file that does not parse is logged once
// per change and the car keeps the setup it already had, and a file that parses is applied whole.
// Refusing to start over a half-saved file would be the one behaviour that makes this worse than
// rebuilding.
void PlayerCar::reloadSetupIfChanged()
{
    auto error = std::error_code{};
    const auto stamp = std::filesystem::last_write_time(setupPath, error);

    if (error)
    {
        // Said once, and said at all. A hot-reload that silently does nothing when its path is wrong
        // is worse than no hot-reload: every setting in the file reads as implemented and behaves as
        // a comment, and the first thing it costs is somebody's afternoon deciding whether the value
        // they changed had any effect.
        if (!setupMissingReported)
        {
            setupMissingReported = true;
            engine.log().warn("No setup sheet at {} (looked in {}), so the car is exactly what its own data says",
                              setupPath.string(), std::filesystem::current_path().string());
        }

        setupSeen = false;

        return;
    }

    setupMissingReported = false;

    if (setupSeen && stamp == setupStamp)
    {
        return;
    }

    setupStamp = stamp;
    setupSeen = true;

    auto file = std::ifstream(setupPath);
    if (!file)
    {
        return;
    }

    const auto text = std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    const auto tune = raceengine::parseVehicleTune(text);

    if (!tune)
    {
        // Named rather than swallowed, and the car is left alone: what a driver wants from a typo is
        // to be told, not to be handed a vehicle assembled from half a sheet.
        engine.log().warn("Setup {} was not read: {}", setupPath.string(), tune.error());

        return;
    }

    // **Onto a car built fresh, never onto the one being driven.** A sheet layered on top of its own
    // last application can only add: delete a line, save, and the number it used to state is still in
    // the car with nothing left in the file to account for it — so the sheet stops being what the car
    // is and becomes everything the sheet has *ever* said. Rebuilding is what makes "absent means
    // whatever the car says" true on the second save as well as the first, and it is what lets
    // `steering.invert` be the flip it actually is instead of a claim about a sign.
    //
    // Only the setup is rebuilt, not the state: this is a car being re-specified, not re-spawned, and
    // a driver who nudges a damper mid-lap should not be put back on the grid.
    auto rebuilt = raceengine::golfGtiMk7();
    if (!rebuilt)
    {
        throw std::runtime_error(rebuilt.error());
    }

    setup = std::move(rebuilt).value();
    driveline = raceengine::golfGtiMk7Driveline();

    raceengine::applyVehicleTune(tune.value(), setup);
    raceengine::applyVehicleTune(tune.value(), driveline);

    // The wheel's own geometry follows the car's. Left behind, an inverted rack would steer the car
    // one way and load the driver's hands the other — a self-centring force pushing *into* the corner,
    // which is the least diagnosable thing a wheel can do.
    rack.travelPerInput = setup.rackTravelPerInput;

    if (tune->feedback.gain || tune->feedback.ceilingTorque || tune->feedback.damping || tune->feedback.damperBandwidth)
    {
        auto mapping = engine.forceFeedback().mapping();
        mapping.gain = tune->feedback.gain.value_or(mapping.gain);
        mapping.ceilingTorque = tune->feedback.ceilingTorque.value_or(mapping.ceilingTorque);
        mapping.damping = tune->feedback.damping.value_or(mapping.damping);
        mapping.damperBandwidth = tune->feedback.damperBandwidth.value_or(mapping.damperBandwidth);

        engine.forceFeedback().setMapping(mapping);
    }

    // Only when it said something. The sheet that ships is entirely comments — a valid setup that
    // changes no number — and announcing a reload that moved nothing is how a log stops being read.
    if (raceengine::statesAnything(tune.value()))
    {
        // What it did, not that it ran. The rack travel is in here because its *sign* is the whole of
        // `steering.invert`, and "the setting had no effect" and "the setting was never read" are
        // indistinguishable from a line that only says a file was applied.
        engine.log().info("Setup {} applied: rack travel {:+.4f} m per unit, front spring {:.0f} N/m, diff preload "
                          "{:.0f} N.m, ffb gain {:.2f} ceiling {:.1f} N.m",
                          setupPath.string(), setup.rackTravelPerInput, setup.corners[0].springRate,
                          driveline.differential.preload, engine.forceFeedback().mapping().gain,
                          engine.forceFeedback().mapping().ceilingTorque);
    }
}

PlayerCar::~PlayerCar()
{
    // Unattended runs are the gates, which have no wheel, no driver and no business littering
    // their build directories.
    if (std::getenv("RACEENGINE_UNATTENDED") != nullptr || std::getenv("RACEENGINE_DUMP_FRAME") != nullptr)
    {
        return;
    }

    const auto frames = engine.forceFeedback().takeTrace();
    if (!frames.empty())
    {
        if (auto file = std::ofstream("rack-exit.csv"))
        {
            file << raceengine::rackTorqueToCsv(frames);
        }
    }

    if (auto file = std::ofstream("ffb-exit.txt"))
    {
        file << engine.forceFeedback().report() << "\n";
    }
}

// Start recording, or stop and write what was recorded.
//
// A ring rather than a growing buffer, because a recorder that allocated would allocate inside the
// tick — and because what a driver wants after something interesting happened is the minute *before*
// they reached for the key, which a ring is and a start-stop capture is not.
void PlayerCar::toggleTelemetry()
{
    recording = !recording;

    if (recording)
    {
        recorder = raceengine::TelemetryRecorder{telemetryCapacity};
        engine.log().info("Telemetry recording");

        return;
    }

    const auto frames = recorder.inOrder();
    if (frames.empty())
    {
        return;
    }

    captures++;

    // Numbered rather than timestamped: a capture is identified by which one it was in this session,
    // and a name built from the clock is a name that differs between two runs of the same gate.
    const auto path = std::filesystem::path("telemetry-" + std::to_string(captures) + ".csv");

    auto file = std::ofstream(path);
    if (!file)
    {
        engine.log().warn("Telemetry {} could not be written", path.string());

        return;
    }

    file << raceengine::telemetryToCsv(frames);

    engine.log().info("Telemetry {} written: {} frames, {:.1f} s", path.string(), frames.size(),
                      static_cast<double>(frames.size()) / 120.0);

    // The steering's own trace beside the car's, from the same key: stage one in newton metres at
    // the rim with stage two's answer next to it, which is the artefact a steering complaint is
    // settled with. The service records it whether or not anything asked, so stopping a recording
    // is simply when it is collected.
    const auto rackFrames = engine.forceFeedback().takeTrace();
    if (rackFrames.empty())
    {
        return;
    }

    const auto rackPath = std::filesystem::path("rack-" + std::to_string(captures) + ".csv");

    auto rackFile = std::ofstream(rackPath);
    if (!rackFile)
    {
        engine.log().warn("Rack trace {} could not be written", rackPath.string());

        return;
    }

    rackFile << raceengine::rackTorqueToCsv(rackFrames);

    engine.log().info("Rack trace {} written: {} frames", rackPath.string(), rackFrames.size());
}

void PlayerCar::update(const float delta)
{
    const auto tick = static_cast<double>(delta);

    reloadSetupIfChanged();

    // Level to edge, the same recovery the paddles need and for the same reason: a held key asks a
    // hundred and twenty times a second and would start and stop the recorder on every one of them.
    const auto telemetryKey = engine.window().keyPressed(raceengine::Key::T);
    if (telemetryKey && !telemetryHeld)
    {
        toggleTelemetry();
    }

    telemetryHeld = telemetryKey;

    const auto asked = demand();

    ticks++;

    // GLFW reports an edge and the window reports a level, so the edge has to be recovered here.
    // The fullscreen binding does not need this because the platform kept its edge; a polled key
    // has already lost it.
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
    input.steering = steering.update(asked.steering, groundSpeed(), tick);
    input.throttle = asked.throttle;
    input.brake = asked.brake;
    input.gear = gear;

    const auto handbrake = asked.handbrake;
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
        lastTelemetry = stepped->telemetry;
        lastDrivelineTorques = driven.value();
        lastStep = stepped.value();
        lastInput = input;

        // The last substep's, which is the newest thing this tick knows. Publishing all three would
        // hand the writer a burst of samples microseconds apart with 8.33 ms of simulated time
        // between them, and any reconstruction fed that reads a rate three hundred times too high.
        if (step == substeps - 1)
        {
            publishRackTorque(stepped.value(), input.steering * setup.rackTravelPerInput, tick);
        }
    }

    // Opt-in, because a line a second is noise in a log nobody is reading and the only thing worth
    // having when the car is not going where it is pointed. `OSR_LOG_INPUT=1`.
    //
    // It prints the demand, what the controller made of it, and what the car did with it — the three
    // numbers that separate "the input never arrived", "the input arrived and did nothing" and "the
    // input arrived and did the opposite", which are indistinguishable from the driver's seat and
    // are the three things worth telling apart.
    static const auto logInput = std::getenv("OSR_LOG_INPUT") != nullptr;

    if (logInput && ticks % 120 == 0)
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

    // The rim the driver sees, turned to the angle the rack demand means: the demand is a fraction
    // of full lock and the rim's lock to lock is this car's own 756 degrees. The sign is negative
    // and was **measured from the seat** (2026-08-21, "left and right are inversed") after the
    // right-hand-rule guess went the other way — the same lesson every sign in this project keeps:
    // both directions produce a plausible wheel, and only the seat can say which is the car's.
    if (steeringWheelMesh)
    {
        // Scaled to the *rig's* lock to lock when a wheel is driving, which is Dominic's call on
        // what the rendered rim is for: it mirrors his hands one to one across the device's whole
        // travel, including past the car's own lock where the demand clamps and a demand-scaled rim
        // would freeze. Without a wheel there are no hands to mirror, and the demand against the
        // car's own lock is the honest stand-in.
        const auto rim = engine.input().rimDegrees();
        const auto rimRadians = rim ? -glm::radians(*rim) : -input.steering * glm::radians(steeringLockToLock) * 0.5;
        carRenderable.meshes[*steeringWheelMesh].localTransform =
            glm::rotate(glm::mat4(1.0f), static_cast<float>(rimRadians), steeringWheelAxis);
    }

    const auto measured = (state.chassis.linearVelocity - entryVelocity) / tick;
    smoothedAcceleration += (measured - smoothedAcceleration) * (1.0 - std::exp(-tick / accelerationSmoothing));

    // What the car sounds like, from the same substep the force feedback came from and for the same
    // reason: the newest thing this tick knows. Published once a tick rather than three times —
    // three sets of parameters microseconds apart with 8.33 ms of simulated time between them is a
    // crossfade being driven three times as fast as the car is moving.
    engine.audio().update(
        raceengine::deriveCarAudio(driveline, drivelineState, lastDrivelineTorques, state, lastStep, lastInput));

    if (recording)
    {
        // The tick's own frame, with the driveline's channels written into it beside the vehicle's,
        // so a run drops into i2 with the engine and the suspension on the same time base.
        auto frame = lastTelemetry;
        frame.time = static_cast<double>(ticks) / 120.0;
        raceengine::fillDrivelineTelemetry(frame, drivelineState, lastDrivelineTorques);

        recorder.record(frame);
    }

    writeNode();
}

// The whole of the game's side of force feedback, and there is deliberately very little of it: what
// the engine wants is newton metres at the rim, and everything that turns a tick's result into that
// is `steeringRackTorque`. Nothing here knows what device is attached or whether one is.
void PlayerCar::publishRackTorque(const raceengine::VehicleStep& stepped, const double rackTravel,
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

    // The tick's own interval, plainly — **and the plainness is the fix** (2026-08-21).
    //
    // This was differenced over the *device sample* interval, with a stale-stamp policy on top that
    // held the previous velocity for one tick and zeroed it after. Both existed because a catch-up
    // burst's ticks all read the same device report, so the tick difference came out double on one
    // tick and zero on the next; the policy turned that into "two ticks of velocity, then one or two
    // of nothing", which is not a repair but a **square wave at the frame rate**. The rig's exit
    // trace shows exactly that: rack velocity was exactly zero on 42.4% of ticks, non-zero on 83% of
    // the first two of each burst and 11% of the rest, and the rack's Coulomb friction — saturated
    // at anything over 10 mm/s, which the median 27 mm/s comfortably is — chopped fully on and off
    // with it, 0.4254 N·m of torque step against 0.0003 N·m on a quiet tick. Aging the velocity
    // fixed the *standing* torque the previous repair left and replaced it with a chopping one.
    //
    // Neither policy can be right while the demand only moves once a frame, and neither is needed
    // now that it does not: `DeviceInputSource` reconstructs the steering demand across the burst,
    // so consecutive ticks are consecutive positions of a rack that is genuinely moving, and the
    // honest denominator for two consecutive positions one tick apart is one tick. A wheel that has
    // stopped moving now yields a zero difference because it *is* not moving, which is the same
    // answer the stale policy was trying to reach by rule.
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
        // One engine tick: what one publish is worth in simulated time, which is the slope the
        // writer's reconstruction may continue. The wall clock cannot say this — see `RackFeedback`.
        .publishInterval = deltaTime,
        .damping = scheduledDamping,
        .inputTimestampNanos = sampleNanos});
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
