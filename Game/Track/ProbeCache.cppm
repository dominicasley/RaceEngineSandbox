module;

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <ios>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

export module osr.game:ProbeCache;

namespace osr
{

// The diffuse half of a scene's light probes, kept on disk between runs.
//
// **Why this exists at all.** A city places a probe every sixty metres of street — 220 of them on
// Grand City Parkway — and photographing one costs six full scene passes plus a prefilter plus a
// readback, which the engine spreads over about ten frames so that a capture never becomes a hitch.
// 220 probes is therefore something like 2,200 frames of startup, half a minute, every single run.
// What comes out of all that work is nine spherical harmonic coefficients a probe: 144 bytes. So
// the work is done once and the answer is kept.
//
// **It is a cache and not an export.** Nothing upstream produces it: the map exporter knows nothing
// about this engine's lighting, and requiring it to would tie a rendering change to a re-export.
// The game bakes it on the first run of a given configuration and writes it out; every run after
// that reads it. Delete the file and the next run rebuilds it.
//
// **What it deliberately does not carry** is the specular half. That is a prefiltered cube of about
// a megabyte a probe, it lives in a GPU array of eight slices, and only the handful of probes a
// frame can reflect in ever need one — those are photographed on every run whatever this file says.
// Detail: `RenderContract.cppm`, `probeSpecularSlices`.
//
// This unit imports nothing, which is what keeps `<fstream>` out of a global module fragment that
// would then be merged against every imported BMI — the `CloudNoise.cppm` pattern. It therefore
// deals in plain floats and the scene converts at its own seam.

// Nine coefficients of four floats, which is how the engine carries one probe's irradiance. The
// fourth component of each is padding the GPU layout requires and is carried rather than dropped,
// so that a round trip through this file is the identity.
export inline constexpr std::size_t probeCacheFloatsPerProbe = 36;

// Everything that decides what the probes would photograph and where they stand. A cache written
// under one key and read under another is a city lit by a different hour of a different day, which
// is worse than no cache: it would look plausible and be wrong. So the key is written into the file
// and checked on the way back in, and a mismatch is a miss rather than a fault.
export struct ProbeCacheKey
{
    std::string track;
    double sunElevationDegrees = 0.0;
    double cloudCoverage = 0.0;
    double spacingMetres = 0.0;
    double heightMetres = 0.0;
    double minimumSeparationMetres = 0.0;
    std::size_t probeCount = 0;
    // What the probes photographed, as `probeWorldHash` states it. Added 2026-09-13 after the sky's
    // air constants changed under a cache that still matched: 213 of the city's 220 probes went on
    // lighting the streets with the old sky while the seven near spawn and the sky itself were new.
    std::uint64_t worldHash = 0;
};

export struct ProbeCache
{
    ProbeCacheKey key;
    // `key.probeCount * probeCacheFloatsPerProbe` floats, probe-major, in the scene's own probe
    // order. Order is the whole identity of an entry — there is no name or position stored per
    // probe — which is why the key carries every input that decides that order.
    std::vector<float> coefficients;
};

// Whether a cache found on disk answers the question being asked. The doubles are compared with a
// tolerance rather than exactly: they come from a command line by way of `std::from_chars`, and a
// key that only matched a bit-identical reparse would miss on every run.
export [[nodiscard]] bool probeCacheMatches(const ProbeCacheKey& wanted, const ProbeCacheKey& found);

// A fingerprint of what a probe photographs beyond its sun and its clouds: every shader source in
// `shaderDirectory` (name and bytes, in name order — the sky's air constants, the ozone term, a
// material model, all live there) and the scenery asset's size and write time (a re-export). What
// it deliberately does not cover is the engine's own C++ — the sun's colour restated in
// `RenderRig.cppm`, the capture's face convention — because keying on the binary would re-bake the
// city after every build; a lighting change on that side wants the cache file deleted by hand.
// A directory or file that cannot be read hashes as absent rather than throwing: the cache is a
// convenience, and a miss is its safe answer.
export [[nodiscard]] std::uint64_t probeWorldHash(const std::string& shaderDirectory, const std::string& sceneryAsset);

export [[nodiscard]] std::expected<ProbeCache, std::string> loadProbeCache(const std::string& filePath);

export [[nodiscard]] std::expected<void, std::string> saveProbeCache(const std::string& filePath,
                                                                     const ProbeCache& cache);

} // namespace osr

namespace osr
{

namespace
{

// Eight bytes so the header is aligned and a truncated file cannot half-match it.
constexpr auto probeCacheMagic = std::string_view("OSRPROBE");

// Bumped whenever the meaning of a coefficient changes — a different basis, a different
// convolution, a different padding convention. An old file then misses rather than being read as
// though it were new, which is the difference between a rebuilt cache and a wrongly lit city.
// 2 since 2026-09-13: the header carries `worldHash`, so every version-1 file is a miss and is
// rebuilt under the sky that lit the world on that date rather than the one before it.
constexpr auto probeCacheVersion = std::uint32_t{2};

static_assert(sizeof(float) == 4, "the cache writes raw float32");
static_assert(sizeof(double) == 8, "the cache writes raw float64");

template <typename T> void writeRaw(std::ofstream& stream, const T& value)
{
    stream.write(reinterpret_cast<const char*>(&value), static_cast<std::streamsize>(sizeof(T)));
}

template <typename T> [[nodiscard]] bool readRaw(std::ifstream& stream, T& value)
{
    return static_cast<bool>(stream.read(reinterpret_cast<char*>(&value), static_cast<std::streamsize>(sizeof(T))));
}

[[nodiscard]] bool closeEnough(const double left, const double right)
{
    return std::abs(left - right) <= 1e-9;
}

// FNV-1a, 64 bits: a byte at a time, no table, and good enough to tell two shader sources apart,
// which is the whole of what is asked of it.
constexpr auto fnvOffsetBasis = std::uint64_t{14695981039346656037ULL};
constexpr auto fnvPrime = std::uint64_t{1099511628211ULL};

void hashBytes(std::uint64_t& hash, const char* bytes, const std::size_t count)
{
    for (auto index = std::size_t{0}; index < count; index++)
    {
        hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[index]));
        hash *= fnvPrime;
    }
}

void hashString(std::uint64_t& hash, const std::string& text)
{
    hashBytes(hash, text.data(), text.size());
}

void hashFileContents(std::uint64_t& hash, const std::filesystem::path& path)
{
    auto stream = std::ifstream(path, std::ios::binary);
    if (!stream.is_open())
    {
        hashString(hash, "unreadable");
        return;
    }

    auto buffer = std::vector<char>(65536);
    while (stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size())) || stream.gcount() > 0)
    {
        hashBytes(hash, buffer.data(), static_cast<std::size_t>(stream.gcount()));
    }
}

} // namespace

std::uint64_t probeWorldHash(const std::string& shaderDirectory, const std::string& sceneryAsset)
{
    auto hash = fnvOffsetBasis;

    // The shaders, in name order: a directory listing's order is the file system's and would make
    // the same directory hash differently on two machines.
    auto error = std::error_code{};
    auto shaders = std::vector<std::filesystem::path>();
    for (const auto& entry : std::filesystem::directory_iterator(shaderDirectory, error))
    {
        if (entry.is_regular_file(error) && entry.path().extension() == ".glsl")
        {
            shaders.push_back(entry.path());
        }
    }
    std::sort(shaders.begin(), shaders.end());

    for (const auto& shader : shaders)
    {
        hashString(hash, shader.filename().string());
        hashFileContents(hash, shader);
    }

    // The scenery by size and write time rather than by bytes: it is hundreds of megabytes, and a
    // re-export changes both.
    auto sizeError = std::error_code{};
    auto writtenError = std::error_code{};
    const auto size = std::filesystem::file_size(sceneryAsset, sizeError);
    const auto written = std::filesystem::last_write_time(sceneryAsset, writtenError);
    const auto sizeValue = sizeError ? std::uint64_t{0} : static_cast<std::uint64_t>(size);
    const auto writtenValue =
        writtenError ? std::int64_t{0} : static_cast<std::int64_t>(written.time_since_epoch().count());
    hashString(hash, sceneryAsset);
    hashBytes(hash, reinterpret_cast<const char*>(&sizeValue), sizeof(sizeValue));
    hashBytes(hash, reinterpret_cast<const char*>(&writtenValue), sizeof(writtenValue));

    return hash;
}

bool probeCacheMatches(const ProbeCacheKey& wanted, const ProbeCacheKey& found)
{
    return wanted.track == found.track && wanted.probeCount == found.probeCount &&
           wanted.worldHash == found.worldHash &&
           closeEnough(wanted.sunElevationDegrees, found.sunElevationDegrees) &&
           closeEnough(wanted.cloudCoverage, found.cloudCoverage) && closeEnough(wanted.spacingMetres, found.spacingMetres) &&
           closeEnough(wanted.heightMetres, found.heightMetres) &&
           closeEnough(wanted.minimumSeparationMetres, found.minimumSeparationMetres);
}

std::expected<ProbeCache, std::string> loadProbeCache(const std::string& filePath)
{
    auto stream = std::ifstream(filePath, std::ios::binary);
    if (!stream.is_open())
    {
        return std::unexpected("no probe cache at " + filePath);
    }

    auto magic = std::string(probeCacheMagic.size(), '\0');
    if (!stream.read(magic.data(), static_cast<std::streamsize>(magic.size())) || magic != probeCacheMagic)
    {
        return std::unexpected("probe cache " + filePath + " does not start with '" + std::string(probeCacheMagic) +
                               "'");
    }

    auto version = std::uint32_t{0};
    if (!readRaw(stream, version) || version != probeCacheVersion)
    {
        return std::unexpected("probe cache " + filePath + " is version " + std::to_string(version) + ", not " +
                               std::to_string(probeCacheVersion));
    }

    auto trackLength = std::uint32_t{0};
    if (!readRaw(stream, trackLength) || trackLength > 256)
    {
        return std::unexpected("probe cache " + filePath + " states an implausible track name length");
    }

    auto cache = ProbeCache{};
    cache.key.track.resize(trackLength);
    if (trackLength != 0 && !stream.read(cache.key.track.data(), static_cast<std::streamsize>(trackLength)))
    {
        return std::unexpected("probe cache " + filePath + " ends inside its track name");
    }

    auto probeCount = std::uint32_t{0};
    if (!readRaw(stream, cache.key.sunElevationDegrees) || !readRaw(stream, cache.key.cloudCoverage) ||
        !readRaw(stream, cache.key.spacingMetres) || !readRaw(stream, cache.key.heightMetres) ||
        !readRaw(stream, cache.key.minimumSeparationMetres) || !readRaw(stream, cache.key.worldHash) ||
        !readRaw(stream, probeCount))
    {
        return std::unexpected("probe cache " + filePath + " ends inside its header");
    }

    // A ceiling rather than a policy: past this the header is corrupt and about to ask for
    // gigabytes, and a scene with a hundred thousand probes is not something this game builds.
    if (probeCount > 100000)
    {
        return std::unexpected("probe cache " + filePath + " states an implausible probe count");
    }

    cache.key.probeCount = probeCount;
    cache.coefficients.resize(cache.key.probeCount * probeCacheFloatsPerProbe);

    const auto payload = static_cast<std::streamsize>(cache.coefficients.size() * sizeof(float));
    if (payload != 0 && !stream.read(reinterpret_cast<char*>(cache.coefficients.data()), payload))
    {
        return std::unexpected("probe cache " + filePath + " is shorter than the " +
                               std::to_string(cache.key.probeCount) + " probes it states");
    }

    return cache;
}

std::expected<void, std::string> saveProbeCache(const std::string& filePath, const ProbeCache& cache)
{
    if (cache.coefficients.size() != cache.key.probeCount * probeCacheFloatsPerProbe)
    {
        return std::unexpected("probe cache for " + filePath + " states " + std::to_string(cache.key.probeCount) +
                               " probes and carries " + std::to_string(cache.coefficients.size()) + " floats");
    }

    auto stream = std::ofstream(filePath, std::ios::binary | std::ios::trunc);
    if (!stream.is_open())
    {
        return std::unexpected("Unable to write probe cache to " + filePath);
    }

    stream.write(probeCacheMagic.data(), static_cast<std::streamsize>(probeCacheMagic.size()));
    writeRaw(stream, probeCacheVersion);
    writeRaw(stream, static_cast<std::uint32_t>(cache.key.track.size()));
    stream.write(cache.key.track.data(), static_cast<std::streamsize>(cache.key.track.size()));
    writeRaw(stream, cache.key.sunElevationDegrees);
    writeRaw(stream, cache.key.cloudCoverage);
    writeRaw(stream, cache.key.spacingMetres);
    writeRaw(stream, cache.key.heightMetres);
    writeRaw(stream, cache.key.minimumSeparationMetres);
    writeRaw(stream, cache.key.worldHash);
    writeRaw(stream, static_cast<std::uint32_t>(cache.key.probeCount));
    stream.write(reinterpret_cast<const char*>(cache.coefficients.data()),
                 static_cast<std::streamsize>(cache.coefficients.size() * sizeof(float)));

    if (!stream)
    {
        return std::unexpected("Probe cache " + filePath + " was not written whole");
    }

    return {};
}

} // namespace osr
