module;

#include <cstddef>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <expected>

export module osr.game:CloudNoise;

namespace osr
{

// A baked noise volume off disk, exactly as the offline baker wrote it and nothing more: the
// dimensions, the channel count and the raw texels. The render rig turns this into a `Texture`;
// keeping the read here keeps `<fstream>` out of a unit that imports `raceengine`, where a std
// header in the global module fragment is merged against every imported BMI and is most of the
// compile — the `Options.cppm` pattern.
export struct CloudVolume
{
    unsigned int width = 0;
    unsigned int height = 0;
    unsigned int depth = 0;
    unsigned int channels = 0;
    std::vector<unsigned char> data;
};

// The `.rawvol` format is two ASCII lines and then the payload: `RAWVOL\n`, then
// `<width> <height> <depth> <channels>\n`, then width*height*depth*channels bytes of uint8 with
// channels interleaved, x fastest, then y, then z. Written by the noise baker beside the gate
// scripts; validated here rather than trusted, because a truncated volume sampled as noise is
// clouds that look almost right.
export [[nodiscard]] std::expected<CloudVolume, std::string> loadCloudVolume(const std::string& filePath);

} // namespace osr

namespace osr
{

std::expected<CloudVolume, std::string> loadCloudVolume(const std::string& filePath)
{
    std::ifstream fileStream(filePath, std::ios::binary);
    if (!fileStream.is_open())
    {
        return std::unexpected("Unable to open cloud noise volume with path " + filePath);
    }

    auto magic = std::string();
    if (!std::getline(fileStream, magic) || magic != "RAWVOL")
    {
        return std::unexpected("Cloud noise volume " + filePath + " does not start with 'RAWVOL': '" + magic + "'");
    }

    auto header = std::string();
    if (!std::getline(fileStream, header))
    {
        return std::unexpected("Cloud noise volume " + filePath + " has no dimension line");
    }

    auto dimensions = std::istringstream(header);
    auto volume = CloudVolume{};
    if (!(dimensions >> volume.width >> volume.height >> volume.depth >> volume.channels))
    {
        return std::unexpected("Cloud noise volume " + filePath +
                               " states no 'width height depth channels' line: '" + header + "'");
    }

    // Sanity rather than policy: a zero axis is not a volume, five channels is not a texel this
    // engine uploads, and past 512 an axis is almost certainly a corrupt header about to ask for
    // gigabytes.
    if (volume.width == 0 || volume.height == 0 || volume.depth == 0 || volume.width > 512 || volume.height > 512 ||
        volume.depth > 512)
    {
        return std::unexpected("Cloud noise volume " + filePath + " states an implausible size: '" + header + "'");
    }

    if (volume.channels == 0 || volume.channels > 4)
    {
        return std::unexpected("Cloud noise volume " + filePath + " states " + std::to_string(volume.channels) +
                               " channels and a texel carries 1 to 4");
    }

    const auto byteCount = static_cast<std::size_t>(volume.width) * volume.height * volume.depth * volume.channels;
    volume.data.resize(byteCount);
    fileStream.read(reinterpret_cast<char*>(volume.data.data()), static_cast<std::streamsize>(byteCount));

    // The byte count is the only thing that can say the read finished — the ResourceService text
    // loader's own lesson: a stream that stopped early reports success and hands over half a file.
    if (static_cast<std::size_t>(fileStream.gcount()) != byteCount)
    {
        return std::unexpected("Cloud noise volume " + filePath + " ends before its own stated size: expected " +
                               std::to_string(byteCount) + " bytes of texels");
    }

    return volume;
}

} // namespace osr
