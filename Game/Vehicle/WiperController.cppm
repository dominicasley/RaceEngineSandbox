module;

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

export module osr.game:WiperController;

import raceengine;

namespace osr
{

// The stalk, in the order a stalk steps through: off, intermittent, and the two continuous speeds.
export enum class WiperMode { Off, Intermittent, Slow, Fast };

// The wipers on the car's windscreen: which setting the stalk is on, when the current cycle began,
// and the arcs the two blades sweep.
//
// **It states geometry and a timing law, and never a blade angle.** Where the blade is at time t is
// a closed-form function of t, so the shader derives it — and, far more usefully, inverts it to ask
// when a given point of the glass was last swept, which is how the rain is cleared without a texel
// of memory. A controller that published an angle each tick would be publishing the *less* useful
// half of the same function, and the shader would still need the law to do the clearing.
//
// Deliberately not on the setup sheet beside `assist.abs`: a wiper setting is not a tune, it is a
// thing a driver changes because of what they can see, so it is a key.
export class WiperController
{
private:
    WiperMode current = WiperMode::Off;
    WiperMode pending = WiperMode::Off;
    double cycleStart = 0.0;

    // The stalk's own edge state. Level to edge for the reason every other key in this game is: the
    // window answers on all hundred and twenty ticks a second, so a held key would step the stalk a
    // hundred and twenty times.
    bool stalkHeld = false;

public:
    // One setting along the stalk, wrapping back to off. Takes effect when the blades next reach
    // park — see update.
    void step();

    // Level to edge, then act on the edge alone.
    void readStalk(bool pressed);

    // Applies a pending setting once the blades are parked, so a change never happens mid-stroke:
    // switching off halfway across the glass would leave the blade standing in the driver's view,
    // and switching speed would jump it. Engaging from off is immediate, since there is no stroke
    // in progress to finish.
    void update(double simulatedSeconds);

    [[nodiscard]] WiperMode mode() const
    {
        return current;
    }

    [[nodiscard]] bool running() const
    {
        return current != WiperMode::Off;
    }

    // What the scene is told. Off states the default-constructed value — every field zero — which
    // is what makes a car with the wipers off byte-identical to the renderer that had none.
    [[nodiscard]] raceengine::Wipers state() const;
};

} // namespace osr

namespace osr
{

namespace
{

// How long a full cycle takes and how much of it the blade spends moving. A continuous setting
// spends all of it moving; intermittent parks for the remainder. Both speeds are a full out-and-back
// per cycle, so `Slow` is about twenty-five double strokes a minute and `Fast` about forty.
struct WiperTiming
{
    float cyclePeriod;
    float sweepSeconds;
};

[[nodiscard]] WiperTiming timingFor(const WiperMode mode)
{
    switch (mode)
    {
    case WiperMode::Intermittent:
        return {6.0f, 2.4f};
    case WiperMode::Slow:
        return {2.4f, 2.4f};
    case WiperMode::Fast:
        return {1.5f, 1.5f};
    case WiperMode::Off:
    default:
        return {0.0f, 0.0f};
    }
}

// The Golf's windscreen, measured off `golf_gti_2018.glb` rather than guessed (2026-08-25).
//
// `Glass_Int` is one mesh holding every interior pane, so the windscreen is not the material's uv
// range — it is one island inside it, found by clustering the mesh's triangles by normal and taking
// the symmetric forward-facing one. It occupies u [0.2192, 0.2736] and v [-0.7435, -0.7968], which
// at that island's own 24.374 model units per u and 17.048 per v measures **1.33 m by 0.91 m** — a
// Golf windscreen, which is what says the island was identified correctly.
//
// Both directions were then confirmed from a capture rather than reasoned from the model's axes,
// because the axes are not what the shader sees: **v increases upward and u increases to the
// driver's right.** Angles below follow from that, measured from "pointing right" toward "pointing
// up", and no arc may straddle +/-pi — which a windscreen wiper's never does.
constexpr float paneMinU = 0.2192f;
constexpr float paneMaxU = 0.2736f;
constexpr float paneMinV = -0.7968f;
constexpr float paneMaxV = -0.7435f;

// One model unit in units of u, from that island's 24.374 model units per u. What lets the blade
// lengths below be stated as the millimetres the parts catalogue gives.
constexpr float uPerMetre = 1.0f / 24.374f;

// How many units of u one unit of v spans on this pane: 17.048 model units per v against 24.374 per
// u. Without it an arc stated in uv is an ellipse on the glass. Measured from the island's own
// triangles — the same 108 that give the 1.33 m by 0.91 m above — rather than derived in the shader
// from its fragment's Jacobian, which is what made the clearing depend on the render resolution.
constexpr float paneAspect = 17.048f / 24.374f;

// The reach is the pivot-to-tip distance: the blade plus the arm that carries it, in the metres the
// model is authored in. Wipers do not clear the whole of a windscreen and these do not either — the
// driver blade reaches 0.70 m up a 0.91 m pane, which is the unswept band above every real one.
//
// **Both numbers below were corrected against a capture** (2026-08-25): the first pair covered a
// lens in the lower middle and never reached the right third of the glass, which is not what a
// tandem pair does. What made it visible was colouring each fragment by *which* arc test rejected
// it — past the outer radius, inside the inner, or outside the sweep — rather than by the result,
// because "no drops cleared here" is the same picture for all three.
// Lengthened on Dominic's call, 2026-08-25: the pair reached too little of the glass to read as a
// windscreen wiper. The driver blade now sweeps 0.80 m up a 0.91 m pane, which leaves the unswept
// band every real one leaves and no more.
constexpr float driverReachMetres = 0.80f;
constexpr float driverRootMetres = 0.12f;
constexpr float passengerReachMetres = 0.72f;
constexpr float passengerRootMetres = 0.10f;

// Where the two pivots sit across the pane, as a fraction of its width, and how far below its
// bottom edge — a wiper pivots at the cowl, under the glass, which is why a fragment near the
// bottom edge is inside the arc rather than under its inner radius. Set so the two fans overlap in
// the middle and each reaches its own outer edge, which is the coverage the pair exists to give.
//
// **Set against a real car, which Dominic went and looked at** (2026-08-25, after a mirrored guess
// and a nudge that were both wrong). The driver's arm pivots in front of the **A-pillar** and
// reaches across to the driver's right; the passenger's arm pivots at the **centre** of the screen
// and reaches the same way. Both arms therefore sit on the driver's half and sweep the far half
// between them, which is what a tandem pair is — and it is not the symmetric arrangement a mirror
// produces, which is why mirroring the earlier guess made it worse rather than better.
//
// u increases to the driver's right and this car is left-hand drive, so the driver's own A-pillar
// is the low end of u.
constexpr float driverPivotAcross = 0.02f;
constexpr float passengerPivotAcross = 0.45f;
constexpr float pivotBelowPane = 0.035f;

// Where the blades rest and how far they sweep, in the pane's own uv with u to the right and v up.
// Zero points along u, so parked at ten degrees both arms lie along the bottom edge reaching to the
// driver's right, and both sweep up and back across. The passenger blade parks with its tip past
// the edge of the glass, as the real one does, tucked under the trim where there are no fragments
// to draw it on.
//
// The arc runs from 10 degrees to 107 and so never touches atan's cut at +/-pi, which it must not:
// a windscreen wiper that sweeps through "pointing left" is a car nobody builds, and the shader's
// inversion has no answer for one.
constexpr float parkDegrees = 10.0f;
constexpr float driverSweepDegrees = 80.0f;
constexpr float passengerSweepDegrees = 92.0f;

// How wide the blade draws, in metres. The rubber is about 20 mm; this is the rubber and the arm.
constexpr float bladeWidthMetres = 0.03f;

} // namespace

void WiperController::step()
{
    switch (pending)
    {
    case WiperMode::Off:
        pending = WiperMode::Intermittent;
        break;
    case WiperMode::Intermittent:
        pending = WiperMode::Slow;
        break;
    case WiperMode::Slow:
        pending = WiperMode::Fast;
        break;
    case WiperMode::Fast:
    default:
        pending = WiperMode::Off;
        break;
    }
}

void WiperController::readStalk(const bool pressed)
{
    if (pressed && !stalkHeld)
    {
        step();
    }

    stalkHeld = pressed;
}

void WiperController::update(const double simulatedSeconds)
{
    if (pending == current)
    {
        return;
    }

    // Parked is the only moment a setting may change: mid-stroke the blade would jump, and a driver
    // switching off halfway across would be left with a blade standing in front of them. From off
    // there is no stroke to finish, and the new cycle starts *now* rather than at some multiple of
    // the period, which is what stops the first sweep beginning halfway along.
    const auto parked = current == WiperMode::Off ||
                        simulatedSeconds - cycleStart >= static_cast<double>(timingFor(current).sweepSeconds);
    if (!parked)
    {
        return;
    }

    current = pending;
    cycleStart = simulatedSeconds;
}

raceengine::Wipers WiperController::state() const
{
    if (current == WiperMode::Off)
    {
        return {};
    }

    const auto timing = timingFor(current);

    const auto paneWidth = paneMaxU - paneMinU;
    const auto paneHeightV = paneMaxV - paneMinV;
    const auto pivotV = paneMinV - paneHeightV * pivotBelowPane;

    return raceengine::Wipers{.cyclePeriod = timing.cyclePeriod,
                              .sweepSeconds = timing.sweepSeconds,
                              .cycleStart = static_cast<float>(cycleStart),
                              .bladeHalfWidth = 0.5f * bladeWidthMetres * uPerMetre,
                              .paneAspect = paneAspect,
                              .bladeA = glm::vec4(paneMinU + paneWidth * driverPivotAcross, pivotV,
                                                  driverRootMetres * uPerMetre, driverReachMetres * uPerMetre),
                              .bladeB = glm::vec4(paneMinU + paneWidth * passengerPivotAcross, pivotV,
                                                  passengerRootMetres * uPerMetre, passengerReachMetres * uPerMetre),
                              .parkAngle = glm::vec2(glm::radians(parkDegrees), glm::radians(parkDegrees)),
                              .sweepAngle =
                                  glm::vec2(glm::radians(driverSweepDegrees), glm::radians(passengerSweepDegrees))};
}

} // namespace osr
