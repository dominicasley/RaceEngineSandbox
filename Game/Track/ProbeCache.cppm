module;

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <fstream>
#include <ios>
#include <string>
#include <string_view>
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
constexpr auto probeCacheVersion = std::uint32_t{1};

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

} // namespace

bool probeCacheMatches(const ProbeCacheKey& wanted, const ProbeCacheKey& found)
{
    return wanted.track == found.track && wanted.probeCount == found.probeCount &&
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
        !readRaw(stream, cache.key.minimumSeparationMetres) || !readRaw(stream, probeCount))
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
