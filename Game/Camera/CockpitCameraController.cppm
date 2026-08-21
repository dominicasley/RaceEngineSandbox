module;

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

export module osr.game:CockpitCameraController;

import :TrackFrame;

import raceengine;

namespace osr
{

// The view from the driver's seat, and it is the one the feel of a car is judged from.
//
// **It takes the body's whole attitude where the chase camera takes only its heading**, and that is
// the difference between the two rather than a tuning choice. A chase camera that rolled with the
// body would be inducing motion sickness to convey something already visible in the car in front of
// it; a cockpit camera that did *not* roll with the body would be a driver whose head is bolted to
// the horizon, which conveys nothing at all — the whole cue is that the world tilts.
//
// **Beyond that attitude it is rigid.** It used to carry a small head simulation — a lean under
// acceleration eased through a one-pole lag — and every part of it was removed on request: from the
// seat, the added sway reads as the car moving under a loose head rather than as load, and the
// suspension already tells that story by moving the whole body. What is left is a camera bolted to
// the seat, so what the frame shows moving is exactly what the car did.
//
// Like `ChaseCameraController` this writes position and direction every tick and is the sole writer
// of both. A scene picks one camera controller.
export class CockpitCameraController
{
    // The driver's eye, in the chassis body frame, metres. The body's origin is the design contact
    // patch under the wheelbase midpoint, so `y` is height above the road and not above the floor.
    //
    // Placeholder in the sense every figure in this repository's vehicle data is: a Golf's H-point is
    // about 0.52 m up with roughly 0.63 m of eye above it, and the seat sits a little behind the
    // wheelbase midpoint.
    //
    // The **sign was measured rather than reasoned**, and it was wrong the first way round: seated at
    // -0.37 the rendered wheel sat against the left edge of the frame with the glovebox to the right,
    // which is the view from the passenger seat.
    //
    // It is named for the side it is on rather than for a sign, because the sign is only meaningful
    // once you know that **+x is the car's left** — see `outboardSign`, which is the one place that
    // is stated and which spent a day being stated wrongly in five. A left-hand-drive car puts its
    // driver on the left, so the seat is at +0.37. The number here never moved; what changed is that
    // the rest of the codebase now agrees with the picture this was measured off.
    static constexpr double eyeLeft = 0.37;
    static constexpr double eyeHeight = 1.15;
    static constexpr double eyeAhead = -0.10;

    raceengine::Engine& engine;

public:
    explicit CockpitCameraController(raceengine::Engine& engine) :
        engine(engine)
    {
    }

    void update(Camera& camera, const raceengine::VehicleState& state);
};

} // namespace osr

namespace osr
{

void CockpitCameraController::update(Camera& camera, const raceengine::VehicleState& state)
{
    const auto seat = glm::dvec3(eyeLeft, eyeHeight, eyeAhead);

    const auto eye = raceengine::bodyToWorld(state.chassis, seat);
    // Along the body's own forward, so the view rolls and pitches with the car. This is the line the
    // chase camera deliberately does not take.
    const auto forward = state.chassis.orientation * glm::dvec3(0.0, 0.0, 1.0);

    const auto placedEye = toWorldUnits(eye);
    const auto direction = glm::normalize(directionToWorldUnits(forward));

    engine.camera().setPosition(camera, static_cast<float>(placedEye.x), static_cast<float>(placedEye.y),
                                static_cast<float>(placedEye.z));
    engine.camera().setDirection(camera, static_cast<float>(direction.x), static_cast<float>(direction.y),
                                 static_cast<float>(direction.z));
}

} // namespace osr
