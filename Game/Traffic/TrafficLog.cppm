module;

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>

export module osr.game:TrafficLog;

namespace osr
{

// The traffic position log, `OSR_TRAFFIC_LOG=<file>` — a one-off diagnostic (2026-09-11) for cars
// seen flying across the map or appearing from nowhere: every car's pose and state, written from the
// simulation thread at the end of every tick on which something about the car changed (its mode, its
// lane, its body slot, a move of more than a metre in one tick, a speed past 45 m/s) and, for every
// car, on every twelfth tick (30 Hz) regardless — so a car's whole history stands in the file at 30 Hz
// with every transition on the tick it happened. The player and the population's report ride the
// same file on the periodic ticks. The file is CSV with one header line; the comment line under it
// says what the two special row kinds carry in the car columns.
//
// Written by a thread of its own: the simulation thread only formats into a string and hands the
// block over once a tick under a mutex held for a swap, so a write to disk never lands inside a
// 360 Hz tick. Imports nothing, so the headers above cost this unit nothing to merge.

export struct TrafficLogRow
{
    std::uint64_t tick = 0;
    // 't' periodic, 'e' a change on this tick, 'p' the player, 'r' the population's report, 'x' a restart.
    char kind = 't';
    std::int32_t id = -1;
    const char* mode = "";
    std::int32_t lane = -1;
    double distanceMetres = 0.0;
    std::int32_t target = -1;
    double progress = 0.0;
    std::int32_t slot = -1;
    double lagMetres = 0.0;
    double stillSeconds = 0.0;
    double disturbedSeconds = 0.0;
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    double vx = 0.0;
    double vy = 0.0;
    double vz = 0.0;
    // The heading for a car, the forward for the player.
    double fx = 0.0;
    double fy = 0.0;
    double fz = 0.0;
    // How far the car moved since the tick before, metres.
    double jumpMetres = 0.0;
    bool police = false;
    bool siren = false;
};

export class TrafficLog
{
public:
    explicit TrafficLog(const std::string& path);
    ~TrafficLog();

    TrafficLog(const TrafficLog&) = delete;
    TrafficLog(TrafficLog&&) = delete;
    TrafficLog& operator=(const TrafficLog&) = delete;
    TrafficLog& operator=(TrafficLog&&) = delete;

    [[nodiscard]] bool open() const;

    // Simulation thread. `row` formats into the tick's block; `flush` hands the block to the writer.
    void row(const TrafficLogRow& entry);
    void flush();

private:
    void run(std::stop_token stop);

    std::ofstream file;
    // The tick's block, the simulation thread's alone.
    std::string pending;
    // The blocks handed over and not yet written, under the mutex.
    std::mutex handover;
    std::condition_variable_any wake;
    std::string queued;
    // Last, so it is joined before anything above is destroyed.
    std::jthread writer;
};

} // namespace osr

namespace osr
{

TrafficLog::TrafficLog(const std::string& path) : file(path, std::ios::out | std::ios::trunc)
{
    if (!file)
    {
        return;
    }

    file << "tick,kind,id,mode,lane,dist,target,progress,slot,lag,still,disturbed,x,y,z,vx,vy,vz,fx,fy,fz,jump,"
            "police,siren\n";
    file << "# kind p: the player -- x y z its position, vx vy vz its velocity, fx fy fz its forward. kind r: the "
            "population's report -- lane=cruising dist=embodied target=disturbed progress=stopped slot=pursuing "
            "lag=external still=recycled disturbed=refused x=heldAsPoints y=dropped z=promoted. kind x: the city "
            "was reseeded on this tick.\n";

    pending.reserve(1 << 16);
    writer = std::jthread([this](std::stop_token stop) { run(stop); });
}

TrafficLog::~TrafficLog()
{
    flush();
    writer.request_stop();
    wake.notify_all();
    // The jthread joins itself; the writer drains what is queued before it stops.
}

bool TrafficLog::open() const
{
    return file.is_open();
}

void TrafficLog::row(const TrafficLogRow& entry)
{
    if (!file.is_open())
    {
        return;
    }

    char line[320];

    const auto written = std::snprintf(
        line, sizeof line,
        "%llu,%c,%d,%s,%d,%.2f,%d,%.4f,%d,%.2f,%.2f,%.2f,%.3f,%.3f,%.3f,%.2f,%.2f,%.2f,%.3f,%.3f,%.3f,%.3f,%d,%d\n",
        static_cast<unsigned long long>(entry.tick), entry.kind, entry.id, entry.mode, entry.lane,
        entry.distanceMetres, entry.target, entry.progress, entry.slot, entry.lagMetres, entry.stillSeconds,
        entry.disturbedSeconds, entry.x, entry.y, entry.z, entry.vx, entry.vy, entry.vz, entry.fx, entry.fy,
        entry.fz, entry.jumpMetres, entry.police ? 1 : 0, entry.siren ? 1 : 0);

    if (written > 0)
    {
        pending.append(line, static_cast<std::size_t>(written) < sizeof line ? static_cast<std::size_t>(written)
                                                                              : sizeof line - 1);
    }
}

void TrafficLog::flush()
{
    if (pending.empty())
    {
        return;
    }

    {
        const auto guard = std::lock_guard<std::mutex>(handover);
        queued += pending;
    }

    pending.clear();
    wake.notify_one();
}

void TrafficLog::run(const std::stop_token stop)
{
    auto block = std::string();

    for (;;)
    {
        {
            auto lock = std::unique_lock<std::mutex>(handover);
            wake.wait(lock, stop, [&] { return !queued.empty(); });
            block.swap(queued);
        }

        if (!block.empty())
        {
            file.write(block.data(), static_cast<std::streamsize>(block.size()));
            block.clear();

            continue;
        }

        // Woken with nothing queued: only a stop does that, and the queue was drained above.
        if (stop.stop_requested())
        {
            break;
        }
    }

    file.flush();
}

} // namespace osr
