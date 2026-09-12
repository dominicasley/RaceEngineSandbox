module;

#include <condition_variable>
#include <cstddef>
#include <fstream>
#include <mutex>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

export module osr.game:DiagnosticLog;

namespace osr
{

// A CSV file written by a thread of its own, for the position logs (`OSR_TRAFFIC_LOG`,
// `OSR_POLICE_LOG`): the simulation thread only formats into a string and hands the block over once
// a tick under a mutex held for a swap, so a write to disk never lands inside a 360 Hz tick. The
// header lines are written at once, on the caller's thread. Imports nothing, so the headers above
// cost this unit nothing to merge.
export class DiagnosticLog
{
public:
    DiagnosticLog(const std::string& path, std::string_view header);
    ~DiagnosticLog();

    DiagnosticLog(const DiagnosticLog&) = delete;
    DiagnosticLog(DiagnosticLog&&) = delete;
    DiagnosticLog& operator=(const DiagnosticLog&) = delete;
    DiagnosticLog& operator=(DiagnosticLog&&) = delete;

    [[nodiscard]] bool open() const;

    // Simulation thread. `line` appends to the tick's block; `flush` hands the block to the writer.
    void line(std::string_view text);
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

DiagnosticLog::DiagnosticLog(const std::string& path, const std::string_view header)
    : file(path, std::ios::out | std::ios::trunc)
{
    if (!file)
    {
        return;
    }

    file.write(header.data(), static_cast<std::streamsize>(header.size()));

    pending.reserve(1 << 16);
    writer = std::jthread([this](std::stop_token stop) { run(stop); });
}

DiagnosticLog::~DiagnosticLog()
{
    flush();
    writer.request_stop();
    wake.notify_all();
    // The jthread joins itself; the writer drains what is queued before it stops.
}

bool DiagnosticLog::open() const
{
    return file.is_open();
}

void DiagnosticLog::line(const std::string_view text)
{
    if (!file.is_open())
    {
        return;
    }

    pending.append(text.data(), text.size());
}

void DiagnosticLog::flush()
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

void DiagnosticLog::run(const std::stop_token stop)
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
