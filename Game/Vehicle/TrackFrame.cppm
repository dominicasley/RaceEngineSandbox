module;

#include <glm/glm.hpp>

export module osr.game:TrackFrame;

namespace osr
{

// Where the simulated world and the drawn one meet, stated once.
//
// Physics is SI and has an origin of its own: metres, +y up, +z forward, and the proving ground is
// generated about x = 0 with z running forward from zero. The scene is a tenth of a metre to the
// unit and was composed around a building years before there was a track to put under it. The
// handedness already agrees, so nothing here is an axis remap — there is a scale, and there is
// where the level decided to lay the track down.

export inline constexpr double worldUnitsPerMetre = 10.0;

// The proving ground's origin in world units. Chosen so the car's grid slot — ten metres along the
// centreline, with fifty metres of flat tarmac still between it and the kerb band — lands exactly
// where the car has always stood in this scene, which is what keeps the composition of the frame a
// separate question from where the track is.
export [[nodiscard]] inline glm::dvec3 trackOrigin()
{
    return glm::dvec3(230.0, 0.0, -290.0);
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
