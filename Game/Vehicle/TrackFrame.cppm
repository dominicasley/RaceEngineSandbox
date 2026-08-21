module;

#include <glm/glm.hpp>

export module osr.game:TrackFrame;

namespace osr
{

// Where the simulated world and the drawn one meet, stated once.
//
// Physics is SI: metres, +y up, +z forward. The scene is a tenth of a metre to the unit. The
// handedness already agrees, so nothing here is an axis remap — there is a scale, and there is
// where the level decided to lay the track down.

export inline constexpr double worldUnitsPerMetre = 10.0;

// Zero, and that is now a decision rather than a default.
//
// The circuit is a real one and arrives in its own world coordinates — Bathurst spans 1.5 km by
// 2.2 km about an origin its own author chose — so laying it down anywhere but where it says it is
// would mean the renderer and the collision mesh, which come out of one file, agreeing only as long
// as two constants agreed. The generated proving ground needed an offset because it was a strip
// about x = 0 that had to be slid under a scene composed years earlier; a track that carries its own
// coordinates needs the opposite. The scale below is the whole of the conversion.
export [[nodiscard]] inline glm::dvec3 trackOrigin()
{
    return glm::dvec3(0.0, 0.0, 0.0);
}

export [[nodiscard]] inline glm::dvec3 toWorldUnits(const glm::dvec3& metres)
{
    return trackOrigin() + metres * worldUnitsPerMetre;
}

// A direction carries the scale and not the origin, which is the whole reason this is two
// functions rather than one applied twice.
export [[nodiscard]] inline glm::dvec3 directionToWorldUnits(const glm::dvec3& metres)
{
    return metres * worldUnitsPerMetre;
}

} // namespace osr
