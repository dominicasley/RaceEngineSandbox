module;

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <stop_token>
#include <thread>
#include <utility>
#include <vector>

#include <glm/glm.hpp>

export module osr.game:Simulation;

import :Options;
import :SimulatedCar;

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
                      AssistSelection assists);

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

private:
    void run(const std::stop_token& stopToken);
    void freeRunning(const std::stop_token& stopToken);
    void handshaken(const std::stop_token& stopToken);
    void step();

    raceengine::Engine& engine;
    // Declared before the thread, so the thread is joined before the world it queries is destroyed.
    // Declared before the cars for the same reason they hold a reference to it.
    raceengine::PhysicsWorld track;
    // Behind pointers because a `SimulatedCar` owns mutexes and is therefore immovable, and because
    // a vector that reallocated would invalidate every reference the game holds into it.
    std::vector<std::unique_ptr<SimulatedCar>> cars;
    bool driven;

    // The handshake. Counters rather than a flag, so that a request issued before the thread reached
    // its wait is not lost and two requests cannot collapse into one.
    std::mutex gate;
    std::condition_variable_any wake;
    std::condition_variable_any finished;
    std::uint64_t requested = 0;
    std::uint64_t completed = 0;

    std::mutex waiting;
    std::condition_variable_any sleeping;

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
                              const double beltBridgingLength, const AssistSelection assists)
{
    cars.push_back(std::make_unique<SimulatedCar>(engine, track, grid, heading, driver, beltBridgingLength, assists));

    return *cars.back();
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

void Simulation::step()
{
    for (auto& car : cars)
    {
        car->tick(tickSeconds);
    }
}

void Simulation::run(const std::stop_token& stopToken)
{
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
// change exists to make. At 17.7 µs of work against a 2.78 ms budget it is not a trade that is
// expected to be called in.
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
            next = now + period;
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
