module;

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <utility>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

export module osr.game:PlayerCar;

import :Options;
import :SimulatedCar;
import :TrackFrame;

import raceengine;

namespace osr
{

// The presentation half of the car the driver is in: the scene node it is watched through, the rim
// that turns in front of them, the noise it makes, and the setup sheet they are editing.
//
// **The tick half is `SimulatedCar` and it runs on another thread.** The split is
// tick-versus-presentation rather than "move the class", and the line between them is drawn by what
// the renderer owns: the scene graph is single precision and is read while a frame records, so a
// node write may not happen on a clock the renderer knows nothing about. Nor may the audio update,
// the steering-wheel mesh transform, or the setup-sheet reload — that last one is a `stat` and a
// file read every frame, and inside a fixed-rate deadline loop it is a stall waiting to happen.
//
// So this runs where it always did, in the stage `Engine::onUpdate` calls the game's own logic, at
// the engine's 120 Hz. `publish` sends what the main thread has decided across; `collect` takes the
// tick's own answer back. Everything in between belongs to the simulation.
export class PlayerCar
{
    // Where golf_gti_2018.glb's own origin sits in the chassis body frame, in metres.
    //
    // Measured off the asset rather than carried over from the car's data: its wheel centres are at
    // z +1.5040 and -1.1341 and its tyres meet y = 0, so its axle midpoint is 0.1850 m ahead of its
    // origin, where the hardpoint frame puts that midpoint at zero and the design contact patch at
    // y = 0. The number a car's own file states is for the mesh that shipped with it and is not
    // transferable: applied here it would bury this one by 77 mm.
    static constexpr double modelOriginAhead = -0.1850;

    // The steering wheel's spin axis in the STEER_HR node's own frame, **measured off the asset
    // rather than assumed**: the disc's smallest-variance axis by PCA over its vertices, because
    // the export keeps the column rake in the vertex data and the node's local frame axis-aligned.
    // The node origin sits on this axis to within 0.3 mm — measured the same way — so rotating
    // about it through the origin spins the rim about the column. If the asset is re-exported with
    // its local +Z on the column, this becomes (0, 0, 1) and nothing else moves.
    static constexpr glm::vec3 steeringWheelAxis{0.0f, -0.92315f, 0.38443f};

    raceengine::Engine& engine;
    SimulatedCar& car;
    SceneNode& node;
    RenderableModel& carRenderable;

    // Index into the renderable's meshes, found once by name; nothing if the model carries no
    // STEER_HR, which is a car whose wheel simply does not turn rather than an error.
    std::optional<std::size_t> steeringWheelMesh;

    // The last thing the simulation published, taken once per engine tick. Everything drawn this
    // tick comes out of here, so the picture is one consistent instant of car rather than several
    // fields read at whatever moment each happened to be asked for.
    CarSnapshot latest{};

    bool telemetryHeld = false;
    std::int32_t captures = 0;

    // Where the setup sheet lives and what it looked like when it was last read. A path rather than
    // a watcher: `last_write_time` is one stat call a frame and needs no thread, no inotify and no
    // second way for the game to be told something, and the thing being watched changes at the speed
    // a human edits a file.
    std::filesystem::path setupPath;
    std::filesystem::file_time_type setupStamp{};
    bool setupSeen = false;
    bool setupMissingReported = false;

    // `OSR_DUMP_RACK`, or empty. Held rather than read from the environment on the way out, so the
    // run's whole configuration is settled in `runOptions` where the cross-checks live.
    std::string rackTracePath;

public:
    PlayerCar(raceengine::Engine& engine, SimulatedCar& car, SceneNode& node, RenderableModel& carRenderable,
              std::string rackTracePath = {});

    // An attended session leaves its steering evidence behind without being asked: the last **ten**
    // minutes of the rack trace and the force feedback service's own summary, written beside the
    // binary on the way out. A wheel complaint is settled by reading these off disk after the
    // drive, rather than by the driver describing a vibration in words — nothing is written on an
    // unattended run, whose gates have no wheel and no driver.
    ~PlayerCar();

    PlayerCar(const PlayerCar&) = delete;
    PlayerCar(PlayerCar&&) = delete;
    PlayerCar& operator=(const PlayerCar&) = delete;
    PlayerCar& operator=(PlayerCar&&) = delete;

    // Main thread to simulation: the sheet and the driver's keys. Called **before** the simulation
    // is advanced, so that a car re-specified this tick is the car this tick steps — under a capture
    // the two are strictly ordered by the handshake, and leaving it to chance would make a future
    // sheet line that moved a spring lag the picture by a tick for reasons nobody could see.
    void publish();

    // Simulation to main thread: one snapshot, and everything drawn from it.
    void collect();

    // One stat a frame, and a reload when the answer changes. Public so a test or a tool can ask for
    // it directly rather than having to run a frame.
    void reloadSetupIfChanged();

    // The driver's own hand on the recorder, from the seat. Public for the same reason the reload
    // is: something other than a keypress may want to ask for it.
    void toggleTelemetry();

    [[nodiscard]] const raceengine::VehicleState& vehicle() const
    {
        return latest.state;
    }

    // World frame, m/s^2, smoothed. What the camera leans against.
    [[nodiscard]] const glm::dvec3& acceleration() const
    {
        return latest.acceleration;
    }

    [[nodiscard]] double groundSpeed() const
    {
        return glm::length(latest.state.chassis.linearVelocity);
    }

    // Where the body frame's origin is standing, in metres — the point the model's own origin is
    // pinned to, and therefore what says whether the car is on the road or through it.
    [[nodiscard]] glm::dvec3 contactDatum() const
    {
        return raceengine::bodyToWorld(latest.state.chassis, glm::dvec3(0.0, 0.0, 0.0));
    }

private:
    void writeNode() const;
    void turnSteeringWheel() const;
};

} // namespace osr

namespace osr
{

PlayerCar::PlayerCar(raceengine::Engine& engine, SimulatedCar& car, SceneNode& node, RenderableModel& carRenderable,
                     std::string rackTracePath) :
    engine(engine),
    car(car),
    node(node),
    carRenderable(carRenderable),
    rackTracePath(std::move(rackTracePath))
{
    // The sheet this car is tuned by, if there is one. Read on the first update rather than here, so
    // that a file appearing mid-session is picked up on the same path as a file being edited — one
    // code path for "there is a setup now" and "the setup changed", which is one fewer way for the
    // two to disagree.
    setupPath = "assets/Setups/golf-gti-mk7.setup";

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

    // The car is placed before the first frame draws it, from the snapshot the simulated car
    // published as it was built. Without this the first frame would show a default-constructed
    // chassis at the world origin.
    collect();
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
    //
    // Built **here**, on the main thread, and handed over. Parsing a file and assembling a torque
    // curve and two vectors of mass components is exactly the kind of work a fixed-rate tick must
    // never be asked to do; what the tick does with the result is a move.
    auto rebuilt = raceengine::golfGtiMk7();
    if (!rebuilt)
    {
        raceengine::fail(rebuilt.error());
    }

    auto next = CarTune{.setup = std::move(rebuilt).value(), .driveline = raceengine::golfGtiMk7Driveline()};

    raceengine::applyVehicleTune(tune.value(), next.setup);
    raceengine::applyVehicleTune(tune.value(), next.driveline);

    // The electronics, resolved against the car that was just rebuilt so their brake calibration is
    // this sheet's brake torques and not the last one's. Absent keys leave them off.
    next.assists = raceengine::golfGtiMk7Assists(next.setup);
    raceengine::applyVehicleTune(tune.value(), next.assists);

    // The pedal cue's thresholds, resolved against the freshly built defaults for the same reason
    // the setup is rebuilt: a deleted line means the model's own number, not the last one stated.
    // Applied here rather than by a `SetupFile` overload because the consumer's type lives in the
    // input layer, which the physics module cannot name.
    next.pedals.onsetPeaks = tune->pedal.onsetPeaks.value_or(next.pedals.onsetPeaks);
    next.pedals.brakeFullPeaks = tune->pedal.brakeFullPeaks.value_or(next.pedals.brakeFullPeaks);
    next.pedals.throttleFullPeaks = tune->pedal.throttleFullPeaks.value_or(next.pedals.throttleFullPeaks);

    const auto rackTravel = next.setup.rackTravelPerInput;
    const auto frontSpring = next.setup.corners[0].springRate;
    const auto preload = next.driveline.differential.preload;
    const auto brakeFull = next.pedals.brakeFullPeaks;
    const auto throttleFull = next.pedals.throttleFullPeaks;
    // **The electronics are deliberately not reported from here**, and this is the second time this
    // file has had to learn it. What the sheet states is not what the car runs whenever
    // `OSR_ASSISTS` is in play, and a line printed here says the sheet's answer with total
    // confidence — which is what it did, announcing "ABS on, traction control full" on a gate run
    // that had explicitly asked for none. `SimulatedCar` reports them, because it is the only thing
    // that has resolved both.

    car.applyTune(std::move(next));

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
                          "{:.0f} N.m, ffb gain {:.2f} ceiling {:.1f} N.m, pedal full at {:.2f}/{:.1f} peaks",
                          setupPath.string(), rackTravel, frontSpring, preload, engine.forceFeedback().mapping().gain,
                          engine.forceFeedback().mapping().ceilingTorque, brakeFull, throttleFull);
    }
}

PlayerCar::~PlayerCar()
{
    // Asked for by name, so it is written whatever the run is. This is the repeatable half: a
    // scripted launch under a frame count writes the same trace on any machine, which is what makes
    // a before-and-after of anything in the steering path a comparison rather than an anecdote.
    if (!rackTracePath.empty())
    {
        const auto scripted = engine.forceFeedback().takeTrace();

        if (auto file = std::ofstream(rackTracePath))
        {
            file << raceengine::rackTorqueToCsv(scripted);
        }

        engine.log().info("Rack trace {} written: {} frames", rackTracePath, scripted.size());

        return;
    }

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
//
// Both halves of it run here, on the main thread, and the tick is only ever handed a ring that is
// already built or relieved of the one it holds. That is what keeps "allocates nothing" true now
// that the tick has a deadline.
void PlayerCar::toggleTelemetry()
{
    if (!car.isRecording())
    {
        car.startRecording(SimulatedCar::freshRecorder());
        engine.log().info("Telemetry recording");

        return;
    }

    const auto frames = car.stopRecording();
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
                      static_cast<double>(frames.size()) / SimulatedCar::telemetryHz);

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

void PlayerCar::publish()
{
    reloadSetupIfChanged();

    // Level to edge, the same recovery the paddles need and for the same reason: a held key asks a
    // hundred and twenty times a second and would start and stop the recorder on every one of them.
    const auto telemetryKey = engine.window().keyPressed(raceengine::Key::T);
    if (telemetryKey && !telemetryHeld)
    {
        toggleTelemetry();
    }

    telemetryHeld = telemetryKey;
}

void PlayerCar::collect()
{
    latest = car.snapshot();

    turnSteeringWheel();

    // What the car sounds like, from the tick's own state. Once per engine tick rather than once per
    // simulation tick: a crossfade driven three times as fast as the picture moves is a crossfade
    // being asked to resolve detail no bank carries, and the mixer's whole design is loops that are
    // never restarted.
    engine.audio().update(latest.audio);

    writeNode();
}

// The rim the driver sees, turned to the angle the rack demand means: the demand is a fraction of
// full lock and the rim's lock to lock is this car's own 756 degrees. The sign is negative and was
// **measured from the seat** (2026-08-21, "left and right are inversed") after the right-hand-rule
// guess went the other way — the same lesson every sign in this project keeps: both directions
// produce a plausible wheel, and only the seat can say which is the car's.
void PlayerCar::turnSteeringWheel() const
{
    if (!steeringWheelMesh)
    {
        return;
    }

    // Scaled to the *rig's* lock to lock when a wheel is driving, which is Dominic's call on what
    // the rendered rim is for: it mirrors his hands one to one across the device's whole travel,
    // including past the car's own lock where the demand clamps and a demand-scaled rim would
    // freeze. Without a wheel there are no hands to mirror, and the demand against the car's own
    // lock is the honest stand-in.
    //
    // Through `deviceLink` and not `rimDegrees`, which is the one change the thread forced here:
    // `rimDegrees` reads the copy the simulation's tick owns and would be a race from this thread.
    // `deviceLink` is the device layer's own answer, taken under its lock and safe to ask from
    // anywhere — it exists for exactly this.
    const auto link = engine.input().deviceLink();
    const auto rimRadians = link.connected && link.hasRim
                                ? -glm::radians(link.rimDegrees)
                                : -latest.rackDemand * glm::radians(SimulatedCar::steeringLockToLock) * 0.5;

    carRenderable.meshes[*steeringWheelMesh].localTransform =
        glm::rotate(glm::mat4(1.0f), static_cast<float>(rimRadians), steeringWheelAxis);
}

void PlayerCar::writeNode() const
{
    const auto origin = raceengine::bodyToWorld(latest.state.chassis, glm::dvec3(0.0, 0.0, modelOriginAhead));
    const auto placed = toWorldUnits(origin);

    engine.sceneManager().setPosition(node, static_cast<float>(placed.x), static_cast<float>(placed.y),
                                      static_cast<float>(placed.z));

    // The scene graph is single precision throughout, so the body's attitude narrows here and it
    // does so component by component rather than through a conversion that would do it silently.
    const auto& attitude = latest.state.chassis.orientation;
    engine.sceneManager().setOrientation(node,
                                         glm::quat(static_cast<float>(attitude.w), static_cast<float>(attitude.x),
                                                   static_cast<float>(attitude.y), static_cast<float>(attitude.z)));
}

} // namespace osr
