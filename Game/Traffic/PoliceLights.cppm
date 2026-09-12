module;

#include <cmath>
#include <cstdint>
#include <string>

#include <glm/glm.hpp>

export module osr.game:PoliceLights;

namespace osr
{

// The police light bar (docs/police-lights-brief.md): what it flashes, in what colours, and how
// brightly. The one place these numbers are stated — the pool that draws the traffic reads them for
// the lens and for the lamp alike (TrafficCars), and the rig hands the two colours to the lens
// shader as definitions (RenderRig), so the glass and the light it throws on the road are the same
// colour by construction and not by two people typing the same number.
//
// **The pattern is Dominic's**: three rapid flashes on one side, then three on the other. A modern
// LED bar's flash is instantaneous — no filament to warm or cool — so each flash is a step on and a
// step off. The car's left side (+x in the fleet's frame) is the red side and its right the blue,
// which is where this fleet's Charger carries its red and blue modules and the American convention
// besides. The two sides never light together.

// One instant of the bar: whether each side is lit.
export struct BeaconState
{
    float red = 0.0f;
    float blue = 0.0f;
};

// The cycle. A half per side; in each half, `beaconFlashesPerSide` flashes of `beaconFlashSeconds`
// starting every `beaconFlashSpacingSeconds`, then dark until the other side's half begins. Sixty
// milliseconds on and sixty off is the rate a real bar's triple flash runs at, near enough that the
// count is what one sees rather than a flicker. Placed; Dominic's to move.
export inline constexpr double beaconFlashSeconds = 0.06;
export inline constexpr double beaconFlashSpacingSeconds = 0.12;
export inline constexpr int beaconFlashesPerSide = 3;
export inline constexpr double beaconHalfPeriodSeconds = beaconFlashSpacingSeconds * beaconFlashesPerSide;
export inline constexpr double beaconPeriodSeconds = 2.0 * beaconHalfPeriodSeconds;

// The bar at a moment of its own clock. Any real number of seconds; the cycle repeats.
export [[nodiscard]] inline BeaconState beaconPattern(const double seconds)
{
    auto phase = std::fmod(seconds, beaconPeriodSeconds);
    if (phase < 0.0)
    {
        phase += beaconPeriodSeconds;
    }

    const auto redHalf = phase < beaconHalfPeriodSeconds;
    const auto within = redHalf ? phase : phase - beaconHalfPeriodSeconds;
    const auto lit = std::fmod(within, beaconFlashSpacingSeconds) < beaconFlashSeconds;

    return BeaconState{.red = redHalf && lit ? 1.0f : 0.0f, .blue = !redHalf && lit ? 1.0f : 0.0f};
}

// Where in the cycle a given car's bar is: its own clock runs from the moment its switch was
// thrown, and no two cars threw theirs together. The golden-ratio step spreads any run of ids
// evenly round the cycle, so a swarm reads as a swarm and not as one bar in twenty places.
export [[nodiscard]] inline double beaconPhaseSeconds(const std::uint32_t carId)
{
    const auto turns = static_cast<double>(carId) * 0.6180339887498949;

    return (turns - std::floor(turns)) * beaconPeriodSeconds;
}

// The two colours, linear. Saturated: an emergency LED is a narrow-band emitter, and the small
// leak into the other channels is what keeps the tone-mapped core from reading as pure magenta
// or cyan when it clips.
export inline constexpr glm::vec3 beaconRed{1.0f, 0.02f, 0.005f};
export inline constexpr glm::vec3 beaconBlue{0.03f, 0.10f, 1.0f};

// How bright the lens is when lit, as the radiance the lens variant of the pbr shader adds over the
// shaded glass, in the frame's own units — the dawn sun's diffuse is about 2.2 (RenderRig), so this
// is a surface some fifteen times a sunlit white, which is what a modern LED bar is: it clips to
// white at its core under any exposure and blooms round it. Placed; Dominic's to move.
export inline constexpr float beaconLensRadiance = 30.0f;

// The lamp that lights the street: its irradiance one metre out, in the same units, and how far it
// reaches at all. Inverse square from the first, exactly zero at the second (docs/vulkan-abi.md,
// *Lights*). At three metres the first is 1.3, about the dawn sun on a surface facing it; at ten it
// is a tenth of that, a tint on the road. Placed; Dominic's to move.
export inline constexpr float beaconLampIrradianceAtOneMetre = 12.0f;
export inline constexpr double beaconLampRangeMetres = 30.0;

// A colour as a GLSL literal, for the shader definitions the rig hands the lens.
export [[nodiscard]] inline std::string glslVec3Literal(const glm::vec3& colour)
{
    return "vec3(" + std::to_string(colour.x) + ", " + std::to_string(colour.y) + ", " + std::to_string(colour.z) +
           ")";
}

} // namespace osr
