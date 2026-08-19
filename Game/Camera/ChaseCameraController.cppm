module;

#include <cmath>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

export module osr.game:ChaseCameraController;

import :TrackFrame;

import raceengine;

namespace osr
{

// The camera a driving game is watched from: behind the car, lagging it, and leaning against what
// the car is doing.
//
// It writes the camera's position and direction on every tick and is the only thing that does. That
// is not a coincidence of this level's wiring, it is the whole rule: `FPSCameraController` writes
// the direction from its own yaw and pitch every tick too, so a scene running both would have one
// of them overwritten before the frame — which is exactly the bug that survived several attempts to
// move the spawn view. A scene picks one.
export class ChaseCameraController
{
    // All in metres, in the car's own heading frame.
    static constexpr double distanceBehind = 6.5;
    static constexpr double heightAbove = 2.3;
    static constexpr double aimAbove = 0.9;

    // Seconds. The position lags noticeably and the aim barely does: a camera whose aim lags is a
    // camera that looks where the car was, which reads as a lost frame rather than as weight.
    static constexpr double positionLag = 0.22;
    static constexpr double aimLag = 0.08;

    // Metres per m/s^2, and deliberately small. At one g this offsets the camera by half a metre —
    // enough that the car visibly pulls away from it under power and swings across it in a corner,
    // and nowhere near enough to be a ride of its own. The cap is what a kerb strike cannot get
    // past.
    static constexpr double leanPerAcceleration = 0.055;
    static constexpr double maximumLean = 1.2;

    raceengine::Engine& engine;
    glm::dvec3 position{0.0};
    glm::dvec3 aim{0.0};
    bool placed = false;

public:
    explicit ChaseCameraController(raceengine::Engine& engine) :
        engine(engine)
    {
    }

    void update(Camera& camera, const raceengine::VehicleState& state, const glm::dvec3& acceleration, float delta);
};

} // namespace osr

namespace osr
{

void ChaseCameraController::update(Camera& camera, const raceengine::VehicleState& state,
                                   const glm::dvec3& acceleration, const float delta)
{
    const auto tick = static_cast<double>(delta);

    // The car's heading, not its attitude. Taking the whole orientation would roll the camera with
    // the body over a kerb and pitch it with the body under braking, which is the difference
    // between conveying load transfer and inducing motion sickness — the body's own roll is
    // already visible in the car, which is where it belongs.
    const auto heading = state.chassis.orientation * glm::dvec3(0.0, 0.0, 1.0);
    const auto flat = glm::dvec3(heading.x, 0.0, heading.z);
    const auto flatLength = glm::length(flat);
    const auto forward = flatLength > 1e-9 ? flat / flatLength : glm::dvec3(0.0, 0.0, 1.0);

    // Against the acceleration and not along it: the camera is what the car is leaving behind.
    // Horizontal only, because the vertical component is mostly the road and a camera that rose
    // over every bump would be reporting the suspension rather than the driving.
    auto lean = glm::dvec3(-acceleration.x, 0.0, -acceleration.z) * leanPerAcceleration;
    if (const auto reach = glm::length(lean); reach > maximumLean)
    {
        lean *= maximumLean / reach;
    }

    const auto anchor = state.chassis.position;
    const auto wanted = anchor - forward * distanceBehind + glm::dvec3(0.0, heightAbove, 0.0) + lean;
    const auto wantedAim = anchor + glm::dvec3(0.0, aimAbove, 0.0);

    // Seeded rather than eased in from wherever the level left the camera: a first tick that had to
    // fly across the scene would put a different picture on the first hundred frames, and a capture
    // is one of them.
    if (!placed)
    {
        position = wanted;
        aim = wantedAim;
        placed = true;
    }
    else
    {
        // Exponential rather than a fraction per tick, so the lag is stated in seconds and means
        // the same thing whatever the tick length is.
        position += (wanted - position) * (1.0 - std::exp(-tick / positionLag));
        aim += (wantedAim - aim) * (1.0 - std::exp(-tick / aimLag));
    }

    const auto eye = toWorldUnits(position);
    const auto towards = directionToWorldUnits(aim - position);
    const auto reach = glm::length(towards);
    const auto direction = reach > 1e-9 ? towards / reach : glm::dvec3(0.0, 0.0, 1.0);

    engine.camera().setPosition(camera, static_cast<float>(eye.x), static_cast<float>(eye.y),
                                static_cast<float>(eye.z));
    engine.camera().setDirection(camera, static_cast<float>(direction.x), static_cast<float>(direction.y),
                                 static_cast<float>(direction.z));
}

} // namespace osr
