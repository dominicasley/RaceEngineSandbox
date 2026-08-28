module;

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <limits>
#include <mutex>
#include <optional>
#include <span>
#include <string>
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

// A car re-specified from a setup sheet: the halves the sheet is allowed to move, built whole on
// the main thread and handed over.
//
// Built there rather than here because building one parses a file, allocates a torque curve and two
// vectors of mass components, and none of that belongs inside a fixed-rate deadline loop. What the
// tick does with it is a move.
export struct CarTune
{
    raceengine::VehicleSetup setup;
    raceengine::DrivelineSetup driveline;
    // The pedal cue's thresholds ride along whole rather than as an overlay, so the rebuild-fresh
    // rule costs the sheet's reader nothing: absent keys were already resolved to the defaults on
    // the main thread.
    raceengine::PedalFeedbackSetup pedals{};
    // The electronics arrive whole for the same reason the pedal cue does: absent keys were already
    // resolved against a freshly built default on the main thread, so the tick takes an answer rather
    // than an overlay.
    raceengine::AssistSetup assists{};
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
    // what the pedal does there.
    //
    // **Derived from the rear brake rather than restated as a number**, which is the correction: it
    // was 175, being a third of the 525 the rear brake used to make, and the brake torque changed
    // underneath it on 2026-08-23. A constant that quietly stops being a third of anything is exactly
    // the kind of stale claim this codebase keeps finding in its own comments.
    [[nodiscard]] double handbrakeTorque() const
    {
        return setup.corners[static_cast<std::size_t>(raceengine::Corner::RearLeft)].brakeTorque / 3.0;
    }

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

    // This session's `OSR_BELT_MM`, in metres, or negative for "the car's own figure". Held for the
    // object's life rather than consumed at construction, because a setup sheet lands a whole fresh
    // setup and would otherwise wipe it — see `stampBeltOverride`.
    double beltBridgingOverride = -1.0;

    // This session's `OSR_LOAD_PATH`, or empty for the car's own setting. Held for the object's life
    // and re-stamped after every setup sheet, for `beltBridgingOverride`'s reason exactly — a tune
    // rebuilds the whole car, and an override applied once at construction is wiped by the first
    // sheet that lands. That mistake has been made twice in this file and cost four laps the first
    // time; see `stampBeltOverride`.
    std::optional<bool> loadPathOverride;

    // And this session's `OSR_DRIVELINE_REACTION`, held and re-stamped for the same reason again.
    std::optional<bool> drivelineReactionOverride;

    // And this session's `OSR_TYRE_THERMAL`, likewise.
    std::optional<bool> tyreThermalOverride;

    // And this session's `OSR_BRAKE_THERMAL`, likewise.
    std::optional<bool> brakeThermalOverride;

    // And this session's `OSR_TYRE_CONTACT`, W/(m²·K), likewise — except that it is stamped onto every
    // corner's tyre rather than onto one flag on the setup, because the tread-road interface is a
    // property of the rubber and the road and each corner carries its own copy of both.
    std::optional<double> tyreContactOverride;

    // And this session's `OSR_TYRE_IDEAL`, degrees Celsius — where the compound's grip plateau is
    // centred. Stamped onto every corner's grip curve for `tyreContactOverride`'s reason, and held
    // and re-stamped for the reason all six of these are.
    //
    // **This one is a statement about which tyre the car wears rather than about a physical constant.**
    // The shipped window is a road compound's since 2026-08-28, slid 20 °C down from the track one AC
    // supplied; `OSR_TYRE_IDEAL=85` is the way back and is what every figure older than that date was
    // measured on. `docs/tyre-state-brief.md`.
    std::optional<double> tyreIdealOverride;

    // Where this session's tyres start, degrees Celsius, or empty for the model's own seed. Applied
    // once at construction and never re-stamped, unlike the three overrides above: a setup sheet
    // rebuilds the *car* and this is a property of the tyres' *state*, which a tune has no business
    // resetting. A driver who changes a spring rate mid-session does not get cold tyres for it.
    std::optional<double> tyreTemperature;

    // The weather this session is being driven in. Not an override and not re-stamped, because it is
    // not a property of the car at all: it is handed to `stepVehicle` beside the world every tick,
    // which is where a scene property belongs. Derived once from `OSR_AIR_TEMP` and the sun.
    raceengine::AmbientConditions ambient{};

    // The car's electronics, and their state. **Off unless this run said otherwise** — see
    // `RunOptions::assists` for why the default is not the factory's. With nothing switched on
    // `updateAssists` still runs, still samples the tone rings and still reports its channels, and
    // still answers `commanded == false`, so the vehicle takes the same code path it always did and
    // both parity goldens are untouched.
    raceengine::AssistSetup assists;
    raceengine::AssistState assistState;
    // This session's `OSR_ASSISTS`, held for the object's life rather than consumed at construction.
    // A tune rebuilds the electronics from the car and re-applies the sheet, so an override stamped
    // once would be wiped by the first sheet that landed — which is exactly what `OSR_BELT_MM` did,
    // and it cost four laps before anybody noticed.
    AssistSelection assistOverride;
    // What was last announced, so a reload that changed nothing is silent and a reload that changed
    // something is not. Announced from here rather than from the sheet's reader because this is the
    // only place that has resolved both the sheet and the session override.
    bool assistsReported = false;
    bool reportedAntilock = false;
    bool reportedCornering = false;
    raceengine::TractionMode reportedTraction = raceengine::TractionMode::Off;
    // What the layer reported on the tick just run. Held because the pedal cue is published after the
    // vehicle tick and the command that produced it is a local there.
    raceengine::AssistChannels lastAssistChannels{};

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
    // The car's own thresholds for the pedal cue, as multiples of the tyre's peak slip. The model's
    // defaults until the setup sheet says otherwise — the `pedal.*` keys arrive through `takeTune`
    // with the rest of the sheet, because sensitivity is the driver's dial the way `ffb.gain` is.
    raceengine::PedalFeedbackSetup pedals{};
    // Where the rack was on the previous tick, so its velocity is a difference and not a field
    // somebody has to remember to keep in step. **This is the number the whole threading change was
    // for.** Differenced across the engine's catch-up burst it came out double on one tick and zero
    // on the next — measured on the rig, exactly zero on 42.4% of all ticks — and the rack's Coulomb
    // friction chopped fully on and off with it at the frame rate. Two consecutive ticks of a
    // fixed-rate clock are two consecutive positions of a rack that is genuinely moving, and one
    // tick is the honest interval between them.
    double previousRackTravel = 0.0;
    // Whether `previousRackTravel` is a rack position or still the placeholder it was built with.
    // Zero is a perfectly good rack position, so the difference cannot be told from the value.
    bool rackTravelSeen = false;

    // The device report that was already there before the first tick, and whether one has been seen.
    // Anything still carrying this stamp is a report from before the run, not a sample of it.
    std::uint64_t preRunSampleNanos = 0;
    bool deviceSampleSeen = false;

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
                 double heading, DriverChoice driver, double beltBridgingLength, std::optional<bool> loadPath,
                 std::optional<bool> drivelineReaction, std::optional<bool> tyreThermal,
                 std::optional<double> tyreContactConductance, std::optional<double> tyreIdealTemperature,
                 std::optional<bool> brakeThermal, std::optional<double> tyreTemperature,
                 const raceengine::AmbientConditions& ambient, AssistSelection assists);

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

    // This car's electric power steering, stated here beside the rest of its steering box for the
    // same reason the lock-to-lock is: the vehicle model carries a rack *travel* and the box is
    // where a ratio, a friction and a motor belong.
    //
    // **The level is anchored on one target; the knee and the taper are no longer stated at all.**
    // The target is 6 N·m at the rim at the cornering limit, against this model's own measured
    // unassisted 20.04 N·m there (`./EngineTests "[.steering-geometry]"`).
    //
    // The two shaping forces used to be constants here — 500 N and 2500 N — and the 2026-08-22
    // session said what that cost. Measured on Dominic's 201-second Bathurst lap, the assist ratio
    // bottomed at 0.29 in the 10-20 N·m band, and the outside front's aligning moment peaks at
    // **17.15 N·m of rack torque** — inside that band. The taper at 2500 N is 26.5 N·m at the rim,
    // above anything sustained driving reaches, so it never did any work; the boost's own peak sat
    // at 11.9 N·m, which is the *bottom* of the limit region. The motor was at its most compressive
    // across the whole of the cue.
    //
    // They are **derived** now, by `deriveSteeringAssist` below, from where this car's steering limit
    // actually falls — which `steeringLimitLoad` and `tyreAligningPeak` compute from the car's own
    // data, with no seat session and nothing hand-placed. See `assistPlacedAtLimit`.
    // **The one number left, and it is a torque a driver's arms feel rather than a curve
    // parameter.** Six newton metres at the rim with the front axle at its limit, against this
    // model's own unassisted 18.5 N·m there. It was the anchor before and it still is; what changed
    // is that `peakBoost` is now *solved* from it against the placed shape instead of being a fitted
    // constant beside it. The two had already drifted once — 2.757 was fitted against a taper that
    // measurement later showed was doing no work, so the level was carrying a shape nobody ran.
    static constexpr double assistTargetAtLimit = 6.0;

    // The steering system's own rotational inertia at the rim, kg·m². A 2.5 kg wheel at a 0.185 m
    // radius of gyration is 0.086 of it; the two front assemblies rotating about their kingpins
    // add 2 × 0.67/13.80² ≈ 0.007 through the measured steering ratio; the rack referred through a
    // 10.6 mm pinion is 3e-4 and rounds away. **Read `SteeringRack::steeringInertia` before sizing
    // anything with it** — the number is the car's and the oscillator it is used on is half the
    // base's, which is a finding rather than a setting.
    static constexpr double steeringSystemInertia = 0.093;

    // The rate the trace is written at, for whoever reports how long a capture ran.
    static constexpr double telemetryHz = 120.0;

private:
    [[nodiscard]] raceengine::DriverInput demand();
    // Stage one, once per tick: what the road is doing to the steering, in newton metres at the rim.
    // It is computed whether or not anything is plugged in — the trace is the artefact, and a
    // channel that only exists when a wheel is attached is a channel nobody can compare a run
    // against.
    void publishRackTorque(const raceengine::VehicleStep& step, double rackTravel, double deltaTime);
    // The pedals' cue, from the same tick: which wheel has stopped rotating with the road, and under
    // which foot. Stage one only — nothing here knows a motor exists.
    void publishPedalFeedback(const raceengine::VehicleStep& step, const raceengine::VehicleInput& input);
    void publishSnapshot();
    void takeTune();

    // Puts this session's `OSR_ASSISTS` back onto whatever electronics have just landed. Called from
    // the constructor and from `takeTune`, which are the only two places a setup arrives — the same
    // rule and the same reason as `stampBeltOverride`.
    void stampAssistOverride();

    // Says what the car is actually running, the first time and whenever it changes.
    void reportAssists();

    // Puts this session's `OSR_BELT_MM` back onto whatever setup has just landed. Called from the
    // constructor and from `takeTune`, which are the only two places a setup arrives.
    void stampBeltOverride();

    // And this session's `OSR_LOAD_PATH`, onto the same two arrivals and for the same reason.
    void stampLoadPathOverride();

    // And this session's `OSR_DRIVELINE_REACTION`, likewise.
    void stampDrivelineReactionOverride();

    // And this session's `OSR_TYRE_THERMAL`, likewise.
    void stampTyreThermalOverride();

    // And this session's `OSR_BRAKE_THERMAL`, likewise.
    void stampBrakeThermalOverride();

    // And this session's `OSR_TYRE_CONTACT`, likewise — onto all four tyres.
    void stampTyreContactOverride();

    // And this session's `OSR_TYRE_IDEAL`, likewise — onto all four grip curves.
    void stampTyreIdealOverride();

    [[nodiscard]] double groundSpeed() const
    {
        return glm::length(state.chassis.linearVelocity);
    }
};

} // namespace osr

namespace osr
{
namespace
{

// Where this car's steering limit sits in newtons at the rack, and the assist placed against it.
//
// **This is the sandbox's job and not the engine's, for the reason the whole file is arranged
// around**: it is the only place that sees both modules. `raceengine.physics` knows the tyre and the
// linkage and can say what the outside front carries at the limit and where its aligning moment
// peaks; `raceengine.input` knows what a rack force is worth at the rim and how a boost curve is
// shaped. Neither imports the other — deliberately, see the note at the top of `RackTorque.cppm` —
// so the conversion between them happens exactly here, where the tick already does it every frame.
//
// And it happens *through `steeringRackTorque` itself* rather than through a second copy of the
// kingpin-and-tie-rod arithmetic. That matters more than it looks: a derivation that re-derived the
// rack force its own way would be a second statement of the number the tick computes, free to drift
// from it, and the assist would then be placed against a limit the car does not actually have.
//
// The manoeuvre modelled is a steady corner at the car's own limit: both front wheels at the slip
// angle where the tyre's aligning moment peaks, the outside carrying `steeringLimitLoad` and the
// inside carrying what is left of the axle. Solved at design ride height and centred rack, because
// where the *limit* falls is a property of the car rather than of the attitude it reaches it in, and
// a lock-dependent answer would make the assist a function of the corner being taken.
[[nodiscard]] std::expected<raceengine::PowerAssist, std::string>
deriveSteeringAssist(const raceengine::VehicleSetup& setup, const double targetRimTorque)
{
    const auto loads = raceengine::steeringLimitLoad(setup);
    if (!(loads.outside > 0.0))
    {
        return std::unexpected("the car states no front axle load to place a steering assist against");
    }

    const auto& tyre = setup.corners[static_cast<std::size_t>(raceengine::Corner::FrontLeft)].tyre;

    auto corners = std::array<raceengine::SteeredCorner, raceengine::steeredCornerLimit>{};

    for (auto index = std::size_t{0}; index < raceengine::steeredCornerLimit; index++)
    {
        const auto& hardpoints = setup.corners[index].hardpoints;

        const auto solved = raceengine::solveCorner(hardpoints, 0.0, 0.0);
        if (!solved)
        {
            return std::unexpected("the steering limit will not solve at design: " + solved.error());
        }

        // Corner 0 is the front left. A positive demand is a right turn, so the left is the outside
        // wheel — the same convention `PublishedCars.cppm` reads its steering ratio against, and the
        // one `outboardSign` states.
        const auto load = index == 0 ? loads.outside : loads.inside;
        const auto limit = raceengine::tyreAligningPeak(tyre, load);

        // Lateral and vertical only, and the lateral one acts across the car — +x is the car's left.
        // Longitudinal force at the patch reaches the rack through the scrub radius too, and on a
        // real lap it is large: measured on Bathurst the outside front runs |Fx|/Fz between 0.60 and
        // 0.73 right through the slip range. But *which way* it acts is a property of what the
        // driver is doing with the throttle and the brakes, not of where the tyre's limit is, and a
        // per-car constant must not carry one lap's driving style.
        corners[index] = raceengine::SteeredCorner{.lowerBallJoint = solved->lowerBallJoint,
                                                   .upperBallJoint = solved->upperBallJoint,
                                                   .steeringArm = solved->steeringArm,
                                                   .rackOuter = hardpoints.steeringRackOuter,
                                                   .contactPatch = solved->contactPatch,
                                                   .patchNormal = glm::dvec3(0.0, 1.0, 0.0),
                                                   .tyreForce = glm::dvec3(limit.lateralForce, load, 0.0),
                                                   .aligningMoment = limit.aligningMoment};
    }

    // An unassisted rack, so what comes back is the road's own force with no motor in it — which is
    // exactly the quantity the boost curve is drawn against.
    auto bare = raceengine::SteeringRack{};
    bare.travelPerInput = setup.rackTravelPerInput;
    bare.lockToLockDegrees = 756.0;
    bare.friction = 0.0;
    bare.damping = 0.0;
    bare.assist = raceengine::PowerAssist{};

    const auto atLimit =
        raceengine::steeringRackTorque(bare, std::span<const raceengine::SteeredCorner>(corners), 0.0, 0.0);
    if (!atLimit.finite || !(std::abs(atLimit.rackForce) > 0.0))
    {
        return std::unexpected("the steering limit came out as no rack force at all");
    }

    return raceengine::assistPlacedAtLimit(bare, std::abs(atLimit.rackForce), targetRimTorque);
}

} // namespace

SimulatedCar::SimulatedCar(raceengine::Engine& engine, const raceengine::PhysicsWorld& world, const glm::dvec3& grid,
                           const double heading, const DriverChoice driver, const double beltBridgingLength,
                           const std::optional<bool> loadPath, const std::optional<bool> drivelineReaction,
                           const std::optional<bool> tyreThermal,
                           const std::optional<double> tyreContactConductance,
                           const std::optional<double> tyreIdealTemperature, const std::optional<bool> brakeThermal,
                           const std::optional<double> startingTemperature,
                           const raceengine::AmbientConditions& conditions, const AssistSelection chosenAssists) :
    engine(engine),
    world(world),
    beltBridgingOverride(beltBridgingLength),
    loadPathOverride(loadPath),
    drivelineReactionOverride(drivelineReaction),
    tyreThermalOverride(tyreThermal),
    brakeThermalOverride(brakeThermal),
    tyreContactOverride(tyreContactConductance),
    tyreIdealOverride(tyreIdealTemperature),
    tyreTemperature(startingTemperature),
    ambient(conditions),
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
        raceengine::fail(built.error());
    }

    setup = std::move(built).value();
    stampBeltOverride();
    stampLoadPathOverride();
    stampDrivelineReactionOverride();
    stampTyreThermalOverride();
    stampBrakeThermalOverride();
    stampTyreContactOverride();
    stampTyreIdealOverride();

    // The electronics are calibrated against the car that was just built rather than against a
    // second copy of its numbers — brake peaks off the corners, the reference radius off the wheel.
    assists = raceengine::golfGtiMk7Assists(setup);

    // Held rather than applied once. The setup sheet lands after this and rebuilds the electronics
    // from the car, so an override consumed here would be wiped by the first sheet that arrived and
    // the log would go on reporting what it had been asked for. That is `stampBeltOverride`'s four
    // lost laps exactly, and it happened again here in the same session: the assignment this replaces
    // never set `assistOverride`, so `OSR_ASSISTS=none` announced itself and changed nothing.
    assistOverride = chosenAssists;
    stampAssistOverride();

    // **Deliberately not logged here.** The setup sheet lands after this constructor runs, so a line
    // printed now would report the pre-sheet state and be wrong for every drive that states an
    // assist — which is precisely the failure `stampBeltOverride` records having cost four laps. What
    // the car ended up with is announced by `PlayerCar` when the sheet is applied.

    // **Zero is not the tyre this car had before the belt**, and the wording says so, because the
    // obvious reading of an A/B against `OSR_BELT_MM=0` is "with and without E2" and it is not that.
    // Turning the length off leaves the grid at seven across, so zero is an uncoupled bed sampled at
    // 7x3 where the pre-E2 car was an uncoupled bed at 3x3, and those read different numbers — the
    // penetration is a quadrature and the sample count is part of the car's configuration. What zero
    // isolates is the coupling alone, which is the more useful comparison and the wrong one to
    // mistake for a revert.
    engine.log().info("Tyre belt: bridging length {:.0f} mm over a {}x{} patch grid, {} solver sweeps ({}).",
                      1000.0 * setup.sampling.beltBridgingLength, setup.sampling.across, setup.sampling.along,
                      setup.sampling.beltIterations,
                      setup.sampling.beltBridgingLength > 0.0 ? "coupled"
                                                              : "uncoupled -- the belt only, not a revert to 3x3");

    rack.travelPerInput = setup.rackTravelPerInput;
    rack.lockToLockDegrees = steeringLockToLock;
    rack.steeringInertia = steeringSystemInertia;

    // The motor, placed against this car's own steering limit rather than against two constants.
    // Fallible in principle and not in practice — the corner has already been solved and validated
    // by `golfGtiMk7` above — so a failure here is a linkage that stopped solving between one call
    // and the next, and the honest answer to that is an unassisted rack rather than a guess.
    if (const auto placed = deriveSteeringAssist(setup, assistTargetAtLimit); placed)
    {
        rack.assist = placed.value();
    }

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
        raceengine::fail(probed.error());
    }

    const auto centreOfMass = state.chassis.centreOfMass;

    state = raceengine::VehicleState{};
    state.chassis.orientation = attitude;
    // The centre of mass is a point in the *body* frame, so a car placed facing anywhere but along
    // +z has to turn it before it can be subtracted: what is being pinned to the grid slot is the
    // body's origin, and `position` is where its centre of mass has to be for that to be true.
    state.chassis.position = grid + attitude * centreOfMass;
    lastRoadTorques = {};

    // And this session's starting tread temperature, after the state has been rebuilt and before a
    // tick has run.
    //
    // **Unset is the TRACK's temperature since 2026-08-28, and that changed with the switch.** While
    // the thermal model shipped off, the default had to be the middle of the compound's plateau: that
    // is the one seed under which turning it on changes nothing, and it is what both parity gates'
    // inertness proof stood on. With the model on for good that argument is spent, and what is left
    // is the physical one — **a car in a garage has cold tyres**, sitting at whatever the tarmac it is
    // parked on is at. It is also the car Dominic drove and accepted, which was
    // `OSR_TYRE_TEMP=ambient` throughout.
    //
    // The track rather than the air, because that is what the rubber is touching. `OSR_TYRE_TEMP=85`
    // is the way back to the old default and `OSR_TYRE_TEMP=<n>` is any other starting point.
    raceengine::seedTyreTemperatures(state, tyreTemperature.value_or(ambient.trackTemperature));

    // The discs start at the air's temperature whatever this run said about the tyres. A car in a
    // garage has cold brakes and there is no case for starting them anywhere else — which is why
    // this is not a knob. **The tyres joined them in that reasoning on 2026-08-28**; they differ only
    // in what they are resting against.
    raceengine::seedDiscTemperatures(state, ambient.airTemperature);

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

void SimulatedCar::stampBeltOverride()
{
    // Negative is "leave the car's own figure alone", which is the default and the shipped behaviour.
    if (beltBridgingOverride < 0.0)
    {
        return;
    }

    setup.sampling.beltBridgingLength = beltBridgingOverride;

    if (beltBridgingOverride <= 0.0)
    {
        return;
    }

    // **The grid has to move with the length.** At three samples across a 0.20 m patch the elements
    // sit 100 mm apart, and a belt that spreads a load over 30 mm couples essentially nothing at that
    // spacing — the shear rate comes out at 0.04 of an element's radial rate, and the model is the
    // uncoupled bed wearing a different name. Seven across puts them 33 mm apart, which is the
    // coarsest grid that resolves a belt of this length at all. It costs: the contact sampler goes
    // from about a third of the vehicle tick's 50 microsecond budget to four fifths of it.
    setup.sampling.across = 7;

    // And so does the sweep count, for a measured reason: projected Gauss-Seidel on this problem
    // converges more slowly the stiffer the belt. Eight sweeps is within 0.03 N of a
    // twenty-thousand-sweep reference at 30 mm and twenty-one newtons out at 60. Thirty-two covers
    // the range this knob accepts. Still a *fixed* count with no early-out, so the run is still
    // deterministic.
    setup.sampling.beltIterations = 32;
}

void SimulatedCar::stampLoadPathOverride()
{
    // Unset is "leave the car's own setting alone", which is the default and the shipped behaviour.
    if (loadPathOverride.has_value())
    {
        setup.geometricLoadPath = loadPathOverride.value();
    }
}

void SimulatedCar::stampDrivelineReactionOverride()
{
    if (drivelineReactionOverride.has_value())
    {
        setup.drivelineReaction = drivelineReactionOverride.value();
    }
}

void SimulatedCar::stampTyreThermalOverride()
{
    if (tyreThermalOverride.has_value())
    {
        setup.tyreThermal = tyreThermalOverride.value();
    }
}

void SimulatedCar::stampBrakeThermalOverride()
{
    if (brakeThermalOverride.has_value())
    {
        setup.brakeThermal = brakeThermalOverride.value();
    }
}

void SimulatedCar::stampTyreContactOverride()
{
    if (!tyreContactOverride.has_value())
    {
        return;
    }

    // Every corner, because each carries its own copy of the tyre. Zero is the word `perfect` and is
    // the model with no interface resistance at all, which is what the shipped car states.
    for (auto& corner : setup.corners)
    {
        corner.tyre.thermal.roadContactConductance = tyreContactOverride.value();
    }
}

void SimulatedCar::stampTyreIdealOverride()
{
    if (!tyreIdealOverride.has_value())
    {
        return;
    }

    // The curve is *slid*, not re-authored: every knot moves by the same number of degrees and the
    // multipliers are untouched, so what changes is where the compound's window sits and nothing at
    // all about its shape. That is the whole claim the sources support — a road compound's peak is
    // lower than a track compound's — and it is deliberately not a licence to redraw the tails, which
    // are nobody's measurement in either position.
    //
    // Stamped against the car's own `idealTemperature`, which is the plateau's centre and is what the
    // shift is measured from. A curve with no points is left alone, since sliding an empty curve
    // states a window a car has not got.
    for (auto& corner : setup.corners)
    {
        auto& thermal = corner.tyre.thermal;
        if (thermal.grip.count == 0)
        {
            continue;
        }

        const auto shift = tyreIdealOverride.value() - thermal.idealTemperature;
        for (auto point = std::size_t{0}; point < thermal.grip.count; ++point)
        {
            thermal.grip.celsius.at(point) += shift;
        }

        thermal.idealTemperature = tyreIdealOverride.value();
    }
}

void SimulatedCar::applyTune(CarTune tune)
{
    const auto guard = std::lock_guard<std::mutex>(tuneLock);
    pendingTune = std::move(tune);
}

void SimulatedCar::stampAssistOverride()
{
    if (!assistOverride.stated)
    {
        // Nothing on the command line, so the setup sheet's answer stands — which is where a driver
        // changes these and where they belong.
        return;
    }

    assists.antilock.enabled = assistOverride.antilock;
    assists.traction.mode = assistOverride.tractionSport ? raceengine::TractionMode::Sport
                            : assistOverride.traction    ? raceengine::TractionMode::Full
                                                         : raceengine::TractionMode::Off;
    assists.cornering.enabled = assistOverride.cornering;
}

void SimulatedCar::reportAssists()
{
    const auto antilock = assists.antilock.enabled;
    const auto cornering = assists.cornering.enabled;
    const auto traction = assists.traction.mode;

    if (assistsReported && antilock == reportedAntilock && cornering == reportedCornering &&
        traction == reportedTraction)
    {
        return;
    }

    assistsReported = true;
    reportedAntilock = antilock;
    reportedCornering = cornering;
    reportedTraction = traction;

    engine.log().info("Driver assists: ABS {}, traction control {}, XDS {}{}.", antilock ? "on" : "off",
                      traction == raceengine::TractionMode::Full    ? "full"
                      : traction == raceengine::TractionMode::Sport ? "sport"
                                                                    : "off",
                      cornering ? "on" : "off", assistOverride.stated ? " (OSR_ASSISTS, overriding the setup)" : "");
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
    pedals = pendingTune->pedals;
    assists = pendingTune->assists;
    pendingTune.reset();

    // **The whole setup arrives, so anything stamped onto the last one is gone.** A tune is built by
    // rebuilding the car from scratch and re-applying the sheet — deliberately, so that deleting a
    // line reverts to the car's own number rather than leaving the last value in place — which means
    // a session override held on this object has to be re-stamped every time one lands, not once at
    // construction. It cost four laps: `OSR_BELT_MM` was applied in the constructor, the first tune
    // arrived before the first tick, and every drive ran the shipped tyre while the startup log
    // cheerfully reported the setting it had been asked for.
    stampBeltOverride();
    stampLoadPathOverride();
    stampDrivelineReactionOverride();
    stampTyreThermalOverride();
    stampBrakeThermalOverride();
    stampTyreContactOverride();
    stampTyreIdealOverride();
    stampAssistOverride();
    reportAssists();

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
    const auto next =
        CarSnapshot{.state = state,
                    .acceleration = smoothedAcceleration,
                    // The tick's own, from the same step the force feedback came from and for the same reason:
                    // the newest thing this tick knows. Derived here rather than shipped as a `VehicleStep` for
                    // the main thread to derive, because this is pure arithmetic over data the tick already has
                    // in registers and the alternative is copying a suspension solution across a lock.
                    .audio = raceengine::deriveCarAudio(driveline, drivelineState, lastDrivelineTorques, state,
                                                        lastStep, lastInput),
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

    // --- the electronics --------------------------------------------------------------------
    //
    // **Upstream of both actuators and downstream of the driver**, which is the whole architecture:
    // the driver says what they want, this decides how it is delivered, and neither the driveline
    // nor the vehicle learns that it happened. It runs before the driveline because its throttle
    // scale is an input to it, and its brake command is handed to the vehicle tick below.
    //
    // What crosses into it is the four wheel speeds, the yaw rate, the accelerometer and the
    // steering angle — all sensors a Mk7 GTI has on its bus. Nothing else can: `raceengine.assists`
    // is a module that `raceengine.physics` imports, so an import the other way is a dependency
    // cycle and a build failure rather than a review comment.
    auto sensors = raceengine::AssistSensors{};
    sensors.wheelSpeeds = speeds;
    sensors.yawRate = lastTelemetry.yawRate;
    sensors.lateralAcceleration = lastTelemetry.acceleration.x;
    sensors.steeringWheelAngle = lastTelemetry.steeringWheelAngle;

    // The pedal reaches the electronics as the pressure the hydraulics made of it, which the car
    // states and this layer cannot: the servo's runout and the rear circuit's proportioning valve are
    // both properties of the plumbing.
    const auto assistCommand =
        raceengine::updateAssists(assists, assistState, sensors, {.brake = input.brake, .throttle = input.throttle},
                                  raceengine::brakeCircuitPressures(setup, input.brake), deltaTime);

    // The engine channel is throttle closing and nothing else: this model's only lever on engine
    // torque is the pedal, because ignition retard would be a term in the engine model and the
    // driveline is not this work's to change. It makes the engine channel slower than a real one,
    // which is the direction that matters least — the brake channel is what catches a transient.
    input.throttle *= assistCommand.throttleScale;

    // The driveline is stepped from the game's loop rather than from inside the vehicle, because
    // which wheels a car drives is a property of the car and not of its suspension. Its torques are
    // recomputed here every tick rather than trusted from anywhere, which is also why they are not
    // a field on `VehicleInput`.
    const auto driven =
        raceengine::stepDriveline(driveline, drivelineState, speeds, inertias, lastRoadTorques, input, deltaTime);
    if (!driven)
    {
        raceengine::fail(driven.error());
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
            wheelTorques[index] -= std::copysign(std::min(handbrakeTorque(), arresting), speeds[index]);
        }
    }

    const auto stepped =
        raceengine::stepVehicle(setup, state, input, wheelTorques, world, deltaTime, assistCommand.brakes, ambient);
    if (!stepped)
    {
        // Nothing here is a runtime condition: the setup was swept across its own travel at load
        // time and the linkage is clamped inside that range every tick, so a solve that fails is a
        // defect. It goes to main's boundary rather than being carried on from.
        raceengine::fail(stepped.error());
    }

    lastRoadTorques = raceengine::roadTorques(stepped.value());
    lastTelemetry = stepped->telemetry;
    lastDrivelineTorques = driven.value();
    lastStep = stepped.value();
    lastInput = input;

    // **Filled here rather than in the recording branch, because two things read this frame now.**
    //
    // The vehicle tick cannot fill the engine, clutch and gear channels — `:Vehicle` does not import
    // `:Driveline` and must not — so somebody downstream has to, and until the rack trace started
    // carrying vehicle state the only reader was the telemetry recorder, which did it on its own
    // copy. A second reader that took the frame from before this line would have written a flat zero
    // into `Engine RPM` for the whole session and it would have looked exactly like an idling car.
    raceengine::fillDrivelineTelemetry(lastTelemetry, drivelineState, lastDrivelineTorques);
    raceengine::fillAssistTelemetry(lastTelemetry, assistCommand.channels);
    lastAssistChannels = assistCommand.channels;

    // Every tick now, which is the whole point of the clock: a 500 Hz writer sees a fresh value on
    // nearly every output frame instead of one in eight, and `RackFeedback::publishInterval` states
    // a period this publish genuinely keeps.
    publishRackTorque(stepped.value(), input.steering * setup.rackTravelPerInput, deltaTime);
    publishPedalFeedback(stepped.value(), input);

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
        // The tick's own frame, with the driveline's channels already written into it beside the
        // vehicle's, so a run drops into i2 with the engine and the suspension on the same time
        // base. The stamp is this recorder's own, which is why the copy survives.
        auto frame = lastTelemetry;
        frame.time = static_cast<double>(ticks) * deltaTime;

        const auto guard = std::lock_guard<std::mutex>(recorderLock);
        recorder.record(frame);
    }

    publishSnapshot();
}

// The pedals' half, and it is the same extraction the rack's is: the caller holds the tick, so the
// caller pulls the numbers out and hands them over as plain ones.
//
// **The cue this produces is the one a steering wheel cannot give.** A locked rear does not reach the
// steering — there is no trail from an unsteered axle to the driver's hands — and neither does
// wheelspin. Both reach a foot, because a foot is on the pedal that caused them.
void SimulatedCar::publishPedalFeedback(const raceengine::VehicleStep& stepped, const raceengine::VehicleInput& input)
{
    auto wheels = std::array<raceengine::SlippingWheel, raceengine::cornerCount>{};

    // Which wheels the driveline turns. Only a driven wheel can be spun by the engine, and which
    // those are is a property of the car rather than of its suspension — the same reason
    // `applyDrivelineTorques` is called from here and not from inside the vehicle tick.
    const auto axle = driveline.driven;

    for (auto index = std::size_t{0}; index < raceengine::cornerCount; index++)
    {
        const auto& solution = stepped.corners[index];
        const auto driven =
            axle == raceengine::DrivenAxle::All || (axle == raceengine::DrivenAxle::Front ? index < 2 : index >= 2);

        wheels[index] = raceengine::SlippingWheel{.slipRatio = solution.contact.slip.slipRatio,
                                                  // The tyre's own peak, under the load it is at
                                                  // right now, so the cue moves with the compound
                                                  // instead of firing at a slip somebody typed.
                                                  .peakSlipRatio = solution.contact.tyre.longitudinalPeakSlip,
                                                  .load = solution.forces.tireVertical,
                                                  .inContact = solution.patch.inContact,
                                                  .driven = driven};
    }

    // A quarter of the car's weight: the load a wheel carries standing still, which is what the
    // minimum load share is a share *of*.
    const auto share = 0.25 * state.chassis.mass * 9.80665;

    // How far the anti-lock unit has taken the wheel pressure away from what the pedal is asking for,
    // worst wheel. Zero with the system off or passive, and cycling at whatever the modulator is
    // cycling at when it is not.
    auto displacement = 0.0;
    for (auto index = std::size_t{0}; index < raceengine::cornerCount; index++)
    {
        displacement = std::max(displacement, lastAssistChannels.driverBrakeTorque[index] > 0.0
                                                  ? -lastAssistChannels.antilockBrakeTorque[index] /
                                                        std::max(setup.corners[index].brakeTorque, 1e-6)
                                                  : 0.0);
    }

    engine.forceFeedback().publishPedals(
        raceengine::derivePedalFeedback(pedals, wheels, input.brake, input.throttle, share, displacement));
}

// The tick's telemetry, as the trace carries it.
//
// **A copy and not a computation**, which is the property the whole arrangement rests on. Every
// number here is worked out in exactly one place — the physics tick — and the heading-invariance
// gate stands over that place, so these columns are covered by it without a second gate and without
// a second chance to disagree. The day this function starts deriving something is the day that stops
// being true, which is why it does not: pneumatic trail is `Mz/Fy` and weight transfer is a
// difference of loads, and both belong to whoever plots the file.
//
// Units stay SI. `rackTorqueToCsv` converts once, at the boundary, like `telemetryToCsv` does.
namespace
{

// The trace, from the tick's telemetry and the tick's electronics.
//
// **The assist state is passed in rather than looked up**, and it is the same copy rule the rest of
// this function is under: `AssistChannels` is what the layer reported on this tick and `AssistSetup`
// is what was fitted when it ran. Reading the setup sheet afterwards answers a different question —
// a lap driven at 12:25 with the anti-lock system on could not be told from one driven at 12:35 with
// it off, because by then the file said something else. That is why these columns exist.
[[nodiscard]] raceengine::VehicleTrace vehicleTraceOf(const raceengine::TelemetryFrame& frame,
                                                      const raceengine::AssistSetup& assists,
                                                      const raceengine::AssistChannels& channels)
{
    auto trace = raceengine::VehicleTrace{
        .centreOfMassX = frame.position.x,
        .centreOfMassZ = frame.position.z,
        .heading = frame.yaw,
        .speed = glm::length(frame.velocity),
        // Body frame, and the roles are the ones `TelemetryFrame::acceleration` states: +x lateral,
        // +z longitudinal. Copied in that order rather than reasoned about again here.
        .lateralAcceleration = frame.acceleration.x,
        .longitudinalAcceleration = frame.acceleration.z,
        .yawRate = frame.yawRate,
        .rideHeightFront = frame.rideHeightFront,
        .rideHeightRear = frame.rideHeightRear,
        .throttle = frame.throttle,
        .brake = frame.brake,
        .clutch = frame.clutch,
        .gear = frame.gear,
        .engineSpeed = frame.engineSpeed,
        .antilockEnabled = assists.antilock.enabled,
        .tractionMode = static_cast<std::uint32_t>(assists.traction.mode),
        .tractionBrakeActive = channels.tractionBrakeActive,
        .tractionEngineActive = channels.tractionEngineActive,
        .corneringEnabled = assists.cornering.enabled,
        .corneringActive = channels.corneringActive,
        .engineTorqueReduction = channels.engineTorqueReduction};

    // By index, both sides. `raceengine::tracedCornerCount` and `raceengine::cornerCount` are the
    // same four corners in the same order — the input partition cannot name the physics module's
    // `Corner`, so it states the count itself and this asserts the two have not drifted apart.
    static_assert(raceengine::tracedCornerCount == raceengine::cornerCount,
                  "the rack trace's corners are the physics module's corners, in its order");

    for (auto index = std::size_t{0}; index < raceengine::cornerCount; index++)
    {
        const auto& wheel = frame.wheels[index];

        trace.wheels[index] = raceengine::WheelTrace{.slipAngle = wheel.slipAngle,
                                                     .slipRatio = wheel.slipRatio,
                                                     .verticalLoad = wheel.verticalLoad,
                                                     .lateralForce = wheel.forceLateral,
                                                     .longitudinalForce = wheel.forceLongitudinal,
                                                     .aligningMoment = wheel.aligningMoment,
                                                     .suspensionTravel = wheel.suspensionTravel,
                                                     .damperVelocity = wheel.damperVelocity,
                                                     .contactingSamples = wheel.contactingSamples,
                                                     .patchDepthSpread = wheel.patchDepthSpread,
                                                     .antilockActive = channels.antilockActive[index],
                                                     .antilockCycles = channels.antilockCycles[index],
                                                     .brakePressure = channels.pressure[index],
                                                     .treadCoreTemperature = wheel.tyreCoreTemperature,
                                                     .discTemperature = wheel.discTemperature,
                                                     .wheelTemperature = wheel.wheelTemperature};
    }

    return trace;
}

} // namespace

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
    // **Nothing on the first tick, because on the first tick there is no previous position.**
    //
    // `previousRackTravel` starts at zero and the rack does not: a session that opens with the wheel
    // anywhere but dead centre differences a real position against a placeholder and calls the
    // result a velocity. The first seat session opened 14.9 mm off centre and read 5347.9 mm/s,
    // which the damping term turned into -4995.8 N of rack force and -52.98 N·m of steering torque
    // on the tick before any of it existed. The safety ramp meant the wheel never felt it — requested,
    // commanded and delivered were all 0.00 there — so the only thing it damaged was every min and
    // max taken over the run afterwards.
    //
    // Suppressed at the source rather than trimmed off in analysis: a file with one poisoned row is
    // a file every future reader has to be warned about, and zero is the honest answer to "how fast
    // is it moving" when only one position has ever been seen.
    // **The report that was already sitting on the device when the simulation started is not a
    // sample this run produced**, and it is the other half of the same first-tick artefact.
    //
    // A wheel nobody is touching sends nothing at all, so `sampleTimestampNanos` still carries
    // whatever arrived when the device was opened — which was before a 251 MiB track finished
    // loading. Measured end to end that read 579.887 ms in the first seat session, against a 10 ms
    // budget, and it is a perfectly accurate measurement of *startup*. What it is not is a steering
    // latency, and it lands in the histogram the gain criterion is read off.
    //
    // Zero is already this field's word for "no latency is measurable here" — a keyboard, or the
    // gate's scripted launch — so the stale report is published under it rather than given a special
    // case downstream. Held by value rather than against a clock: the first genuinely new report has
    // a different stamp, whenever it comes, and everything from there is measured normally.
    const auto deviceSampleNanos = engine.input().sampleTimestampNanos();

    if (!deviceSampleSeen)
    {
        deviceSampleSeen = true;
        preRunSampleNanos = deviceSampleNanos;
    }

    const auto sampleNanos = deviceSampleNanos == preRunSampleNanos ? std::uint64_t{0} : deviceSampleNanos;

    const auto rackVelocity = rackTravelSeen && deltaTime > 0.0 ? (rackTravel - previousRackTravel) / deltaTime : 0.0;

    previousRackTravel = rackTravel;
    rackTravelSeen = true;

    const auto speed = groundSpeed();

    // **Everything the steering knows is in `rack` now, and this line is the whole of it.**
    //
    // It used to be four constants and two schedules written between this call and the publish
    // below — an EPS assist and a parking damper, both applied to every channel on the way out.
    // That seam is exactly where the three-stage split had no name for anything, so it is where two
    // hardware-sized numbers ended up living: an assist justified by what fits under the sheet's
    // ceiling, and a damper sized against an inertia fitted from this base's own limit cycle. See
    // `PowerAssist` for both accounts. What is left here is assembly and a publish.
    const auto torque = raceengine::steeringRackTorque(rack, corners, rackVelocity, speed);

    // **The car used to ask for damping here, and it no longer does — the request has moved to
    // stage two, where device knowledge is allowed to live.**
    //
    // The number was 0.4, scheduled down over 5 m/s, and it was justified as 2·√(K·J) for a released
    // rim returning to centre on the carcass spring. Reckoned honestly for this car, `J` is
    // `SteeringRack::steeringInertia` — 0.093 kg·m² of rim, column and two front corners — and 2·√(K·J)
    // is 0.83. But 0.4 was fitted from *this base's* limit cycle, so its `J` was the base's rim, and
    // the two are 5.5x apart because they are correct about **two different oscillators**.
    //
    // Which of them the simulation contains settles it, and the answer is neither: the rack angle is
    // commanded straight from the demand, so there is no steering degree of freedom anywhere and the
    // only inertia physically in the loop a damper closes is the motor, belt and rim of whatever is
    // plugged in. So 0.4 is a **device** parameter that was misfiled as a car one — the same defect
    // as the assist above wearing different clothes — and shipping 0.83 would have sized a damper
    // for a mass the simulation does not have. It is `ForceMapping::deviceDamping` now.
    //
    // The car's honest steering inertia is kept on the rack for the day it has something to belong
    // to. That day is a rack with a mass and a compliance under the driver's demand, which is a
    // vehicle-model addition and is recorded as one — see `docs/post-m2-remediation-brief.md`.

    // A stage-one refusal is forwarded as one rather than smoothed into a zero. Zero is a torque and
    // is a perfectly good thing for the wheel to hold; what has happened here is that the physics
    // handed the steering a number that is not one, and the honest answer to that is to let go.
    engine.forceFeedback().publish(raceengine::RackFeedback{
        // What the driver's hands get is the assisted rack, because that is what is between the
        // road and the rim in the real car. What the trace keeps beside it is the unassisted one.
        .steeringTorque = torque.finite ? torque.assistedTorque : std::numeric_limits<double>::quiet_NaN(),
        .unassistedTorque = torque.steeringTorque,
        .rackForce = torque.rackForce,
        .tyreRackForce = torque.tyreForce,
        .rackTravel = rackTravel,
        .rackVelocity = rackVelocity,
        // One simulation tick: what one publish is worth in simulated time, which is the slope the
        // writer's reconstruction may continue. The wall clock cannot say this — see `RackFeedback`.
        .publishInterval = deltaTime,
        .inputTimestampNanos = sampleNanos,
        // The tick's own frame, projected. Nothing downstream steers by it.
        .vehicle = vehicleTraceOf(lastTelemetry, assists, lastAssistChannels)});
}

} // namespace osr
