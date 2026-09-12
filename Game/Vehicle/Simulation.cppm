module;

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>
#include <utility>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <Profiling/RaceEngineProfile.hpp>

export module osr.game:Simulation;

import :Options;
import :PoliceCar;
import :SimulatedCar;
import :TrafficDirector;

import raceengine;

namespace osr
{

// The simulation, on a clock of its own.
//
// **This is a timing change and not a performance one, and the distinction shapes everything here.**
// At 17.7 µs median per vehicle tick against a 50 µs budget the physics was never CPU-bound; what it
// lacked was a *cadence*. `Engine::step` ran the fixed step in a catch-up burst inside the frame —
// at 60 fps two ticks back to back in about a quarter of a millisecond, then nothing for the
// remaining 16.4 ms — so a 500 Hz force feedback writer found something new sixty times a second and
// spent the other four hundred and forty wake-ups replaying a value up to 16.7 ms old. Worse than
// the lag: about 8% of the stage-one torque signal's energy sat above the frame-rate Nyquist, which
// is not delayed but *aliased*, folded into the band a driver can feel as something else. Four of
// the nine force-feedback faults found on 2026-08-21 came from that one root, and a fixed-rate clock
// retires the class rather than papering over it once more.
//
// **The thread owns the world and steps every body in it.** Not "the player's car has a thread" —
// the design targets twenty-four cars and a second one has to be a `push_back`, which it is. The
// world is `const` today and that is what makes a reader on another thread safe; the moment anything
// mutates it — debris, moving barriers, a second dynamic body — this class already owns it outright
// and everyone else asks through a snapshot. Cheap to build in now, expensive to retrofit.
//
// What is deliberately *not* here: scene-graph writes, because the graph is single-precision and the
// renderer reads it; the audio update; the steering-wheel mesh; and the setup-sheet reload, which is
// a `stat` and a file read and has no business inside a deadline loop. Those are `PlayerCar`'s, and
// the split is tick-versus-presentation.
export class Simulation
{
public:
    // The rate the vehicle model was validated at, and now the rate the whole tick runs at. It used
    // to be three substeps inside the engine's 120 Hz tick; the substep loop *is* the tick loop now,
    // so the number of vehicle steps a second is exactly what it always was and only their spacing
    // in wall time has changed.
    //
    // 360 is three times the engine's 120, and that divisibility is load-bearing rather than tidy:
    // a capture run advances a whole number of simulation ticks per engine tick, so nothing anywhere
    // keeps an accumulator and the tick count stays a function of the frame number.
    //
    // **Which is why the step is derived from the engine's own constant and not written as 1/360.**
    // `Engine::fixedTimeStep` is a `float`, and widened it is 0.0083333337679505 rather than the
    // 0.0083333333333333 the decimal suggests — so a literal 1.0/360.0 is *not* a third of the
    // engine's tick, it is a third of a slightly different number, and three of them do not add up
    // to one of the engine's. The two clocks would then disagree by 0.4 parts per million, which is
    // physically nothing and is still enough to make "three ticks is one tick" false in the one
    // place the capture handshake needs it to be true. Divided this way, three of these sum back to
    // exactly the engine's step — checked, and it is exact for this value rather than in general.
    static constexpr int ticksPerEngineTick = 3;
    static constexpr double tickSeconds =
        static_cast<double>(raceengine::Engine::fixedTimeStep) / static_cast<double>(ticksPerEngineTick);
    static constexpr double tickRate = 1.0 / tickSeconds;

    // `driven` swaps the scheduling and nothing else. See `advance`.
    Simulation(raceengine::Engine& engine, raceengine::PhysicsWorld track, bool driven);
    ~Simulation();

    Simulation(const Simulation&) = delete;
    Simulation(Simulation&&) = delete;
    Simulation& operator=(const Simulation&) = delete;
    Simulation& operator=(Simulation&&) = delete;

    // Before `start`. The reference stays good for the life of the simulation — the cars are held
    // behind pointers precisely so that a second one cannot move the first out from under whoever
    // is watching it.
    SimulatedCar& add(const glm::dvec3& grid, double heading, DriverChoice driver, double beltBridgingLength,
                      std::optional<bool> loadPath, std::optional<bool> drivelineReaction,
                      std::optional<bool> tyreThermal, std::optional<double> tyreContactConductance,
                      std::optional<double> tyreRoadAreaFraction, std::optional<double> tyreIdealTemperature,
                      std::optional<bool> tyrePressure, std::optional<bool> brakeThermal,
                      std::optional<bool> kerbContact, std::optional<bool> frameAcceleration,
                      std::optional<double> rearWheelRate, std::optional<double> tyreTemperature,
                      const raceengine::AmbientConditions& ambient, AssistSelection assists);

    // Before `start`, and after the cars. The city's traffic, ticked on this thread ahead of every
    // car so that what a car collides against is where traffic is *now* rather than where it was a
    // tick ago.
    //
    // **It lives here rather than in the scene for the reason the world does**: it is written every
    // tick by this thread, and anything else that wants to see it asks through a published snapshot.
    // A director owned by the scene and ticked from the frame would be a second writer on a
    // different clock.
    void enableTraffic(const TrafficNetwork& source, const TrafficSettings& settings,
                       std::optional<raceengine::NavMesh> navmesh);

    // Where the traffic is, for the thread that draws it. Empty when there is none.
    void collectTraffic(std::vector<TrafficSnapshot>& into) const;

    // The R key. Any thread: the request is taken at the top of the next tick, on this thread, and
    // puts every car back on its grid slot, reseeds the traffic and clears the police's meter — a
    // wrecked or an arrested car is a car that cannot drive anywhere, and the process should not
    // have to be restarted to drive again (docs/police-pursuit-brief.md).
    void requestRestart();
    // The number keys, one to five: the police's meter put at that level and a chase started if none
    // is on. Any thread; taken at the top of the next tick like the restart. The last request before
    // the tick wins.
    void requestWantedLevel(int level);

    // Once every body is placed. Nothing ticks until this is called, so a half-built world is never
    // stepped — the same rule a scene keeps when it registers its update callback last.
    void start();

    // Stops and joins. Called explicitly before anything that reads what the tick publishes into is
    // torn down, and again by the destructor if nobody did.
    void stop();

    // Under a capture, run exactly this many ticks and wait for them; otherwise return at once,
    // because the thread is keeping its own time.
    //
    // **This is the whole of the determinism answer.** A capture requires the tick count to be a
    // function of the frame number so a golden frame reproduces across machines and sessions, and a
    // free-running physics thread destroys that outright. The alternative was to keep the in-loop
    // path for capture runs, and this project has already made the argument against that in its own
    // words: a gate is only insensitive to something it does not contain, and a gate exercising a
    // code path the game does not ship is not a gate. So the shipping thread runs the shipping tick
    // either way, and only the thing that decides *when* differs — a handshake per engine tick under
    // capture, a fixed-rate clock otherwise.
    //
    // One engine tick's worth at a time rather than a whole frame's, because the presentation side
    // integrates per engine tick too: the chase camera's lag, the audio update and the smoothed
    // acceleration all advance once per 120 Hz tick, and frame 0 runs two of them.
    void advance(int ticks);

    [[nodiscard]] const raceengine::PhysicsWorld& world() const
    {
        return track;
    }

    // Where the loose props are, for the thread that draws them.
    //
    // **Copied under a lock rather than borrowed**, on `SimulatedCar::snapshot`'s own terms: the
    // world is written by this thread every tick from the moment anything breaks, and a reader
    // walking Jolt's bodies while it integrates them is the one race this design exists to prevent.
    // Empty until something breaks, which is most sessions.
    void freeProps(std::vector<raceengine::PropTransform>& into) const;

private:
    void run(const std::stop_token& stopToken);
    void freeRunning(const std::stop_token& stopToken);
    void handshaken(const std::stop_token& stopToken);
    void step();
    void restart();

    raceengine::Engine& engine;
    // Declared before the thread, so the thread is joined before the world it queries is destroyed.
    // Declared before the cars for the same reason they hold a reference to it.
    raceengine::PhysicsWorld track;
    // Behind pointers because a `SimulatedCar` owns mutexes and is therefore immovable, and because
    // a vector that reallocated would invalidate every reference the game holds into it.
    std::vector<std::unique_ptr<SimulatedCar>> cars;
    // Declared after the cars and before the thread: it is read by the tick and it holds a
    // publication lock of its own, so it must outlive the thread and be destroyed before nothing.
    std::optional<TrafficDirector> traffic;
    bool driven;

    // Simulated time, as a count of ticks rather than a clock. It is what the traffic lights run on,
    // and it is a function of the tick number so a captured run sees the same lights on the same
    // frame on any machine.
    std::int64_t tickCount = 0;

    // Scratch the tick reuses, so a city with traffic in it allocates nothing per tick.
    std::vector<raceengine::DynamicObstacle> obstacleScratch;
    std::vector<raceengine::TrafficVehicle> vehicleScratch;

    // The player as the police see it, rebuilt every tick, and the velocity change the player's
    // bodywork took on the last tick, split by what it hit. The police read the hit a tick late,
    // because the traffic — and the director in it — is ticked before the cars are.
    PlayerFrame playerFrame{};
    struct PlayerImpacts
    {
        double police = 0.0;
        double traffic = 0.0;
        double world = 0.0;
    };
    PlayerImpacts pendingImpacts{};
    std::vector<BodyworkHit> hitScratch;

    std::atomic<bool> restartRequested{false};
    // Zero is no request.
    std::atomic<int> wantedLevelRequested{0};

    // The handshake. Counters rather than a flag, so that a request issued before the thread reached
    // its wait is not lost and two requests cannot collapse into one.
    std::mutex gate;
    std::condition_variable_any wake;
    std::condition_variable_any finished;
    std::uint64_t requested = 0;
    std::uint64_t completed = 0;

    std::mutex waiting;
    std::condition_variable_any sleeping;

    // The loose props as of the newest tick, and the lock over the handoff. Written by the
    // simulation thread and read by whoever is drawing — the asymmetry `SimulatedCar` keeps: the
    // tick publishes under a `try_lock` and never waits, because a dropped publish costs the picture
    // one tick of freshness and a stalled tick costs a deadline.
    mutable std::mutex publication;
    std::vector<raceengine::PropTransform> publishedProps;
    // Scratch the tick reuses, so a step that breaks nothing allocates nothing.
    std::vector<raceengine::PropTransform> propScratch;
    std::vector<std::uint32_t> releaseScratch;
    std::vector<raceengine::PropVelocity> velocityScratch;

    // Last, so it stops and joins before anything it touches on the way down is destroyed.
    std::jthread thread;
};

} // namespace osr

namespace osr
{

Simulation::Simulation(raceengine::Engine& engine, raceengine::PhysicsWorld track, const bool driven) :
    engine(engine),
    track(std::move(track)),
    driven(driven)
{
}

Simulation::~Simulation()
{
    stop();
}

SimulatedCar& Simulation::add(const glm::dvec3& grid, const double heading, const DriverChoice driver,
                              const double beltBridgingLength, const std::optional<bool> loadPath,
                              const std::optional<bool> drivelineReaction, const std::optional<bool> tyreThermal,
                              const std::optional<double> tyreContactConductance,
                              const std::optional<double> tyreRoadAreaFraction,
                              const std::optional<double> tyreIdealTemperature, const std::optional<bool> tyrePressure,
                              const std::optional<bool> brakeThermal, const std::optional<bool> kerbContact,
                              const std::optional<bool> frameAcceleration, const std::optional<double> rearWheelRate,
                              const std::optional<double> tyreTemperature,
                              const raceengine::AmbientConditions& ambient, const AssistSelection assists)
{
    cars.push_back(std::make_unique<SimulatedCar>(engine, track, grid, heading, driver, beltBridgingLength, loadPath,
                                                  drivelineReaction, tyreThermal, tyreContactConductance,
                                                  tyreRoadAreaFraction, tyreIdealTemperature, tyrePressure,
                                                  brakeThermal, kerbContact, frameAcceleration, rearWheelRate,
                                                  tyreTemperature, ambient, assists));

    return *cars.back();
}

void Simulation::enableTraffic(const TrafficNetwork& source, const TrafficSettings& settings,
                               std::optional<raceengine::NavMesh> navmesh)
{
    if (thread.joinable())
    {
        return;
    }

    // A driven run's tick count is a function of the frame, so its route searches stay on this
    // thread at one a tick; a free-running one hands them to a worker (docs/pursuit-radio-brief.md).
    auto chosen = settings;
    chosen.routeOnWorker = !driven;

    traffic.emplace(engine, source, chosen, std::move(navmesh));
}

void Simulation::collectTraffic(std::vector<TrafficSnapshot>& into) const
{
    if (!traffic)
    {
        into.clear();

        return;
    }

    traffic->collect(into);
}

void Simulation::start()
{
    if (thread.joinable())
    {
        return;
    }

    engine.log().info("Simulation running at {:.0f} Hz on its own thread, {}", tickRate,
                      driven ? "driven a tick at a time by the capture" : "keeping its own time");

    thread = std::jthread([this](const std::stop_token& stopToken) { run(stopToken); });
}

void Simulation::stop()
{
    if (!thread.joinable())
    {
        return;
    }

    thread.request_stop();
    wake.notify_all();
    sleeping.notify_all();
    thread.join();
}

void Simulation::freeProps(std::vector<raceengine::PropTransform>& into) const
{
    const auto guard = std::lock_guard<std::mutex>(publication);

    into = publishedProps;
}

void Simulation::requestRestart()
{
    restartRequested.store(true);
}

void Simulation::requestWantedLevel(const int level)
{
    wantedLevelRequested.store(level);
}

void Simulation::restart()
{
    for (auto& car : cars)
    {
        car->restart();
    }

    if (traffic)
    {
        traffic->restart();
    }

    pendingImpacts = PlayerImpacts{};

    engine.log().info("Restarted: the car is back on its grid slot, the traffic is reseeded and the police have "
                      "forgotten you.");
}

void Simulation::step()
{
    RACEENGINE_ZONE_N("simulation tick");

    if (restartRequested.exchange(false))
    {
        restart();
    }

    if (const auto level = wantedLevelRequested.exchange(0); level != 0 && traffic)
    {
        traffic->setWantedLevel(level);
    }

    // **Traffic first, and the order is the whole of the seam.** The city moves, and only then does
    // a car collide against it — so what the contact solver is handed is where a traffic car is on
    // this tick. Ticked before the cars rather than after them for the same reason the prop release
    // happens before the world is stepped: a body that moved after the thing that hit it was
    // resolved is a body that was hit where it no longer is.
    if (traffic)
    {
        vehicleScratch.clear();

        for (const auto& car : cars)
        {
            const auto& chassis = car->vehicle().chassis;

            vehicleScratch.push_back(
                raceengine::TrafficVehicle{.positionMetres = chassis.position,
                                           .velocityMetresPerSecond = chassis.linearVelocity,
                                           .forward = chassis.orientation * glm::dvec3(0.0, 0.0, 1.0)});
        }

        const auto focus = vehicleScratch.empty() ? glm::dvec3(0.0) : vehicleScratch.front().positionMetres;

        // The player, as the police see it: where its body is, which way it is going, how big it is,
        // what its bodywork took last tick — and, for a patrol car's own solver, the car as an
        // obstacle with its real mass, so a ram moves both cars.
        const PlayerFrame* framed = nullptr;
        if (!cars.empty())
        {
            const auto& player = *cars.front();
            const auto& chassis = player.vehicle().chassis;
            const auto& box = player.body();

            playerFrame.player = raceengine::PursuitPlayer{.positionMetres = player.bodyOrigin(),
                                                           .velocityMetresPerSecond = chassis.linearVelocity,
                                                           .forward = chassis.orientation * glm::dvec3(0.0, 0.0, 1.0),
                                                           .lengthMetres = 2.0 * box.halfExtents.z,
                                                           .widthMetres = 2.0 * box.halfExtents.x,
                                                           .impactPoliceMetresPerSecond = pendingImpacts.police,
                                                           .impactTrafficMetresPerSecond = pendingImpacts.traffic,
                                                           .impactWorldMetresPerSecond = pendingImpacts.world};
            playerFrame.obstacle = player.obstacle(playerObstacle);
            framed = &playerFrame;
        }

        pendingImpacts = PlayerImpacts{};

        traffic->tick(tickSeconds, static_cast<double>(tickCount) * tickSeconds, focus, vehicleScratch, track, framed);

        if (!cars.empty())
        {
            auto& player = *cars.front();

            // What the patrol cars' own solves did to the player's body this tick, applied before the
            // player's tick reads its velocity; and it counts as being hit by the police.
            for (const auto& nudge : traffic->playerNudges())
            {
                player.nudge(nudge.deltaLinear, nudge.deltaAngular);
                pendingImpacts.police += nudge.impactMetresPerSecond;
            }

            const auto& chase = traffic->pursuit();
            if (chase.wrecked)
            {
                player.disable("the bodywork has taken all it can");
            }
            else if (chase.busted)
            {
                player.disable("busted");
            }
        }
    }

    for (auto& car : cars)
    {
        obstacleScratch.clear();

        if (traffic)
        {
            traffic->obstaclesNear(car->vehicle().chassis.position, obstacleScratch);
        }

        car->tick(tickSeconds, obstacleScratch);
    }

    // What the cars' own solvers did to the traffic they hit, handed back so a car that was pushed
    // hard enough stops being a point on a lane and becomes a body. Nothing happens on a track with
    // no traffic, and nothing happens on a tick where nothing was touched.
    if (traffic)
    {
        for (auto index = std::size_t{0}; index < cars.size(); index++)
        {
            const auto& car = *cars[index];

            // What the car's bodywork read off its own manifold, as this car's share of each hit it
            // met at a closing speed (`bodyworkHits`) — once, on the tick it arrives, and never the
            // solver's impulse, which carries the position correction on every tick of a push-out.
            hitScratch.clear();
            bodyworkHits(car.contacts(), car.vehicle().chassis.mass, hitScratch);

            traffic->applyContacts(car.contacts(), hitScratch);

            // And the player's own, split by what it hit. The road holds the car up through the
            // tyres and not through this manifold, so ordinary driving reads zero here and a wall, a
            // traffic car or a patrol car reads the hit. Read by the police on the next tick.
            if (index != 0)
            {
                continue;
            }

            for (const auto& hit : hitScratch)
            {
                if (hit.obstacle == raceengine::noObstacle)
                {
                    pendingImpacts.world += hit.selfMetresPerSecond;
                }
                else if (traffic->isPolice(hit.obstacle))
                {
                    pendingImpacts.police += hit.selfMetresPerSecond;
                }
                else
                {
                    pendingImpacts.traffic += hit.selfMetresPerSecond;
                }
            }
        }
    }

    tickCount++;

    // **The street furniture, and this is the only place in this project where the world is
    // written.** The order is the whole of it. Each car has just decided, inside its own tick and
    // before resolving anything, which of the props it is touching have anchors that cannot hold —
    // and has then resolved the collision against them as real bodies. So what is sitting in the
    // manifolds is a decision and a velocity: the props to let go of, and what the two-body solve
    // gave each of them. Both are handed over here, release first because a static body has no
    // velocity to add to, and only then is the world integrated — so a prop that came free this tick
    // starts moving on this tick rather than the next.
    //
    // **No impulse crosses this seam any more, and that is the fix rather than a tidy-up.** The
    // impulse a rigid solver computes against a prop that is still bolted down is sized by the
    // *car's* mass — enough to stop 1400 kg however little the prop weighs — so handing it to a
    // twenty-kilogram bin launched the bin at about seventy times the car's approach speed.
    //
    // Nothing here runs while nothing is loose — `PhysicsWorld::step` returns on a counter, and a
    // manifold that touched no prop produces nothing to hand over — so a session that never hits
    // anything pays a branch.
    releaseScratch.clear();
    velocityScratch.clear();
    for (const auto& car : cars)
    {
        for (const auto& body : car->contacts().bodies)
        {
            if (body.prop == raceengine::noProp)
            {
                continue;
            }

            // The anchor gave way during this tick's solve, so the world has to be told before the
            // velocity below can land on anything: a static body has no motion properties.
            if (body.released)
            {
                releaseScratch.push_back(body.prop);
            }

            velocityScratch.push_back(
                raceengine::PropVelocity{.prop = body.prop, .linear = body.deltaLinear, .angular = body.deltaAngular});
        }
    }

    // And the patrol cars' — the full models the traffic director stepped this tick decided about
    // the props they hit on the same terms, and until 2026-09-11 nobody carried their decisions
    // here: a prop a patrol car hit never broke (docs/police-driving-brief.md §2.2). A prop the
    // player and a patrol car both broke on one tick is listed twice and released once.
    if (traffic)
    {
        const auto releases = traffic->propReleases();
        const auto velocities = traffic->propVelocities();

        releaseScratch.insert(releaseScratch.end(), releases.begin(), releases.end());
        velocityScratch.insert(velocityScratch.end(), velocities.begin(), velocities.end());
    }

    track.releaseProps(releaseScratch);
    track.applyPropVelocities(velocityScratch);
    track.step(tickSeconds);

    track.freeProps(propScratch);
    if (!propScratch.empty())
    {
        if (auto held = std::unique_lock<std::mutex>(publication, std::try_to_lock); held.owns_lock())
        {
            publishedProps = propScratch;
        }
    }

    if (traffic)
    {
        traffic->publish();
    }

    // A frame of its own, on its own track. The render frame and this one are different clocks —
    // that separation is the whole reason this thread exists — and one timeline holding both is what
    // makes a tick that ran late visible as a gap rather than as a slow frame.
    RACEENGINE_FRAME_N("simulation");
}

void Simulation::run(const std::stop_token& stopToken)
{
    // Named from the thread itself, which is the only place Tracy can be told: a profiler showing
    // four numbered threads cannot say which one missed its cadence.
    RACEENGINE_THREAD("simulation");

    if (driven)
    {
        handshaken(stopToken);

        return;
    }

    freeRunning(stopToken);
}

// One tick per wake-up, and **never a burst** — which is the entire point, so it is worth stating
// what happens when the thread is late rather than leaving it to the arithmetic.
//
// Falling behind is handled the way the engine's own spiral guard handles it: the surplus is dropped
// rather than deferred. A thread that ran four ticks back to back to catch up would have rebuilt the
// very thing this replaced, one layer down, and handed the writer a burst again. So simulated time
// can run slightly slow under load, and the publish interval stays regular — which is the trade this
// change exists to make. At 17.7 µs of work against a 2.78 ms budget it was not a trade that was
// expected to be called in; the police's route searches called it in on 2026-09-10 (a 17 ms search
// in the tick), and the trace also showed the loop paying a whole period on top of every overrun:
// after a late tick it waited `period` from *now*, so a tick of D ms cost D + 2.78 ms of wall time for
// one tick of simulated time. Now the next tick starts at once. That is still one tick per wake-up —
// the interval between two ticks is never shorter than the period, because a late tick is by
// definition longer than it — and it is not a burst (docs/pursuit-radio-brief.md, §1.4).
void Simulation::freeRunning(const std::stop_token& stopToken)
{
    const auto period = std::chrono::nanoseconds(static_cast<std::int64_t>(1e9 * tickSeconds));

    auto next = std::chrono::steady_clock::now();

    while (!stopToken.stop_requested())
    {
        step();

        next += period;

        if (const auto now = std::chrono::steady_clock::now(); next < now)
        {
            next = now;
        }

        auto held = std::unique_lock<std::mutex>(waiting);
        static_cast<void>(sleeping.wait_until(held, stopToken, next, [] { return false; }));
    }
}

void Simulation::handshaken(const std::stop_token& stopToken)
{
    while (!stopToken.stop_requested())
    {
        {
            auto held = std::unique_lock<std::mutex>(gate);
            if (!wake.wait(held, stopToken, [this] { return requested > completed; }))
            {
                return;
            }
        }

        // Outside the gate: the tick takes locks of its own — the publish, the tune, the recorder —
        // and holding this one across it would put the main thread's `advance` behind them.
        step();

        {
            const auto guard = std::lock_guard<std::mutex>(gate);
            completed++;
        }

        finished.notify_all();
    }
}

void Simulation::advance(const int ticks)
{
    if (!driven || ticks <= 0)
    {
        return;
    }

    auto held = std::unique_lock<std::mutex>(gate);
    requested += static_cast<std::uint64_t>(ticks);
    held.unlock();

    wake.notify_all();

    held.lock();
    finished.wait(held, [this] { return completed >= requested; });
}

} // namespace osr
