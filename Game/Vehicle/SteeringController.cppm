module;

#include <algorithm>
#include <cmath>

export module osr.game:SteeringController;

namespace osr
{

// The stage between whatever the driver is holding and the number the rack is given.
//
// For a wheel it is the identity — the rim is geared to the rack and the driver's own hands are the
// rate limit — and it exists anyway. A key is 0 or 1 and a stick is a position rather than a rate,
// so both need shaping before a rack can be asked for them; if a raw axis is ever assigned straight
// to a rack angle then adding that shaping later means touching the input path, the physics
// boundary and every calibration that was made against the old numbers. One indirection now costs a
// struct and a line.
export struct SteeringSettings
{
    // Rack input per second, winding lock on and taking it back off. They differ because a driver
    // unwinds faster than they wind on, and because a self-centring rack helps with one and not the
    // other.
    double rateToLock = 2.2;
    double rateToCentre = 4.5;

    // Speed-sensitive range. Full authority up to `fullRangeSpeed` — a car being parked needs all
    // of its lock — falling to `narrowRange` of it by `narrowRangeSpeed` and no further. Without it
    // the key that parks the car spins it at motorway speed, because full lock is a manoeuvre at
    // one end of the range and a crash at the other.
    double fullRangeSpeed = 8.0;
    double narrowRangeSpeed = 45.0;
    double narrowRange = 0.34;
};

// A keyboard, which is the case this stage exists for.
export [[nodiscard]] inline SteeringSettings keyboardSteering()
{
    return SteeringSettings{};
}

// A wheel: no rate limit worth imposing and no range to take away, so this reduces to passing the
// rim's own position through. Stated as settings rather than as a bypass so that the two inputs
// differ in data and not in which code path they take.
//
// The rim's own travel against the car's is not this stage's business and is already done by the
// time a demand arrives here — `rackFromRim` does it, against the range the device itself reports.
// Doing it twice would be a wheel geared to a car geared to a wheel.
export [[nodiscard]] inline SteeringSettings wheelSteering()
{
    return SteeringSettings{
        .rateToLock = 1e9, .rateToCentre = 1e9, .fullRangeSpeed = 0.0, .narrowRangeSpeed = 0.0, .narrowRange = 1.0};
}

// A stick. It is a position rather than a rate like a key, so the rate limit is looser than the
// keyboard's; the speed-sensitive range is the same problem and the same answer, because a thumb
// reaching the stick's corner at motorway speed is asking for exactly what a key held down asks
// for. Wider than the keyboard's narrow range because a stick can ask for half lock and a key
// cannot.
export [[nodiscard]] inline SteeringSettings gamepadSteering()
{
    return SteeringSettings{
        .rateToLock = 4.0, .rateToCentre = 7.0, .fullRangeSpeed = 8.0, .narrowRangeSpeed = 45.0, .narrowRange = 0.45};
}

export class SteeringController
{
    SteeringSettings settings;
    double rack = 0.0;

public:
    explicit SteeringController(const SteeringSettings settings = keyboardSteering()) :
        settings(settings)
    {
    }

    // A wheel switched on mid-session changes what the driver is holding but not where the rack is,
    // so the rack is deliberately kept: reset to centre here and a car would snap straight the
    // instant a device was plugged in.
    void reconfigure(const SteeringSettings& next)
    {
        settings = next;
    }

    // How much of full lock is available at this speed. A wheel's settings collapse the two speeds
    // onto each other, and the zero-span branch is what makes that mean "all of it" rather than a
    // division by nothing.
    [[nodiscard]] double range(const double speed) const
    {
        if (speed <= settings.fullRangeSpeed)
        {
            return 1.0;
        }

        const auto span = settings.narrowRangeSpeed - settings.fullRangeSpeed;
        if (span <= 0.0)
        {
            return settings.narrowRange;
        }

        const auto through = std::clamp((speed - settings.fullRangeSpeed) / span, 0.0, 1.0);

        return 1.0 + (settings.narrowRange - 1.0) * through;
    }

    // `demand` is the raw axis in [-1, 1]: a pair of keys, a stick, or a rim. `speed` is how fast
    // the car is going over the ground in m/s. What comes back is `VehicleInput::steering`.
    double update(const double demand, const double speed, const double deltaTime)
    {
        const auto target = std::clamp(demand, -1.0, 1.0) * range(speed);

        // Which of the two rates applies is a question about the move and not about the target:
        // coming off full left towards a little left is still unwinding.
        const auto unwinding = std::abs(target) < std::abs(rack);
        const auto step = (unwinding ? settings.rateToCentre : settings.rateToLock) * deltaTime;

        rack += std::clamp(target - rack, -step, step);

        return rack;
    }

    [[nodiscard]] double angle() const
    {
        return rack;
    }
};

} // namespace osr
