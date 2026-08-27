module;

#include <cmath>

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
// **That claim was false for roll until 2026-08-27, and the seat is what found it.** Position and
// direction were the only two things written, and a direction carries yaw and pitch and *cannot
// carry roll*: `glm::lookAt` rebuilds the view basis against `Camera::roll`, which the rig sets to
// world up and nothing here ever changed. So the head stayed level with the horizon while the car
// banked, and — the car model being drawn at the body's full attitude — the **cabin rotated in
// front of the driver** by the whole bank angle instead. That is exactly backwards from a real car,
// where the cabin is rigid to your eyes and the world tilts, and it is why the body read as leaning
// far too much from the seat while the same car measured an ordinary 2.8 deg/g on the plate.
//
// Roll is now written, and it is the **only** thing this change touches: pitch and yaw ride the
// forward vector as they always did, and the eye rides `bodyToWorld` as it always did.
//
// **The head is damped and the body is not**, which is the second half of the fix rather than a
// decoration. A camera welded to the body reports every kerb strike and every bump at the frequency
// the suspension passes them, and a driver's head does not: the neck and the vestibular system are
// a low pass on the seat, and what reaches the eyes is the sustained lean of a corner with the
// chatter taken out. So the bank angle goes through a one-pole lag before it becomes the up vector.
//
// **It lags, it does not scale.** A sustained corner still arrives at the body's whole bank angle,
// because a lag has no steady-state error — the head ends up where the seat is, it simply takes a
// moment to get there. A driver's head also counter-rolls a little toward vertical in a long
// corner, and that is a *fraction* rather than a lag; it is deliberately not here, because nobody
// has asked for it and it is one more constant to defend from the seat.
//
// **Beyond that it stays rigid.** It used to carry a small head simulation — a lean under
// acceleration eased through a one-pole lag — and every part of it was removed on request: from the
// seat, the added sway reads as the car moving under a loose head rather than as load, and the
// suspension already tells that story by moving the whole body. The damper below is on the roll the
// body *has*, not on a sway invented for the camera, which is the difference.
//
// Like `ChaseCameraController` this writes position and direction every tick and is the sole writer
// of both, and it now writes the up vector on the same terms. A scene picks one camera controller.
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

    // Seconds, and stated in seconds rather than as a fraction per tick so that it means the same
    // thing at any frame rate — the same reason `ChaseCameraController` states its two lags that
    // way.
    //
    // **Sized against what it has to separate**, which is a corner from a kerb. A one pole at this
    // constant turns over at 1/(2*pi*tau) = 1.06 Hz: the lean of a corner builds over most of a
    // second and comes through at about three quarters of its height, and a 10 Hz kerb rattle comes
    // through at a tenth. Longer and the head starts arriving after the corner does, which reads as
    // a lost frame; shorter and the chatter is back.
    static constexpr double bankLag = 0.15;

    raceengine::Engine& engine;

    // The head's own bank, radians, positive the same way the body's is. Held across ticks because
    // that is what a lag is.
    double bank = 0.0;
    bool settled = false;

public:
    explicit CockpitCameraController(raceengine::Engine& engine) :
        engine(engine)
    {
    }

    void update(Camera& camera, const raceengine::VehicleState& state, float delta);
};

} // namespace osr

namespace osr
{

void CockpitCameraController::update(Camera& camera, const raceengine::VehicleState& state, const float delta)
{
    const auto tick = static_cast<double>(delta);

    const auto seat = glm::dvec3(eyeLeft, eyeHeight, eyeAhead);

    const auto eye = raceengine::bodyToWorld(state.chassis, seat);
    // Along the body's own forward, so the view pitches with the car. This is the line the chase
    // camera deliberately does not take. Roll is *not* in here and never was — a direction has no
    // roll to carry — which is the whole of the bug this function used to have.
    const auto forward = state.chassis.orientation * glm::dvec3(0.0, 0.0, 1.0);
    const auto bodyUp = state.chassis.orientation * glm::dvec3(0.0, 1.0, 0.0);

    // The zero-roll reference: the up vector `lookAt` derives for itself out of world up, which is
    // the one this camera has been getting. Measuring the body's bank *against it* rather than
    // against world up directly is what keeps the angle a pure roll about the view axis, with the
    // pitch already taken out — so damping it cannot disturb the pitch.
    const auto sideways = glm::cross(forward, glm::dvec3(0.0, 1.0, 0.0));
    const auto sidewaysLength = glm::length(sideways);

    // Nose straight up or straight down. There is no horizon left to measure a bank against, so
    // nothing is measured: the head keeps the bank it has and the body's own up carries the frame.
    // Reachable off a ramp, and the alternative is a normalise by zero.
    if (sidewaysLength < 1e-6)
    {
        const auto placedEye = toWorldUnits(eye);
        const auto direction = glm::normalize(directionToWorldUnits(forward));

        engine.camera().setPosition(camera, static_cast<float>(placedEye.x), static_cast<float>(placedEye.y),
                                    static_cast<float>(placedEye.z));
        engine.camera().setDirection(camera, static_cast<float>(direction.x), static_cast<float>(direction.y),
                                     static_cast<float>(direction.z));
        engine.camera().setRoll(camera, static_cast<float>(bodyUp.x), static_cast<float>(bodyUp.y),
                                static_cast<float>(bodyUp.z));

        return;
    }

    const auto level = glm::cross(sideways / sidewaysLength, forward);

    // The signed angle from level to the body's own up, about the view axis. `atan2` of the cross
    // against the dot rather than an `acos` of the dot: the sign is the whole point, and an `acos`
    // loses it and then loses precision as well at the small angles a car actually rolls at.
    const auto wanted = std::atan2(glm::dot(glm::cross(level, bodyUp), forward), glm::dot(level, bodyUp));

    // Seeded on the first tick rather than eased in from zero, for the reason the chase camera
    // seeds its position: a capture is one of the first hundred frames, and a head winding itself
    // up out of level would put a different picture on all of them.
    if (!settled)
    {
        bank = wanted;
        settled = true;
    }
    else
    {
        bank += (wanted - bank) * (1.0 - std::exp(-tick / bankLag));
    }

    const auto up = glm::angleAxis(bank, forward) * level;

    const auto placedEye = toWorldUnits(eye);
    const auto direction = glm::normalize(directionToWorldUnits(forward));

    engine.camera().setPosition(camera, static_cast<float>(placedEye.x), static_cast<float>(placedEye.y),
                                static_cast<float>(placedEye.z));
    engine.camera().setDirection(camera, static_cast<float>(direction.x), static_cast<float>(direction.y),
                                 static_cast<float>(direction.z));
    // The third of the three, and the one that was missing. `syncLayeredCameras` copies it to the
    // car and frame cameras with the rest of the pose, so the layered frame needs nothing.
    engine.camera().setRoll(camera, static_cast<float>(up.x), static_cast<float>(up.y), static_cast<float>(up.z));
}

} // namespace osr
