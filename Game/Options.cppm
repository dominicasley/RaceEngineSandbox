module;

#include <cstdlib>
#include <stdexcept>
#include <string>

export module osr.game:Options;

namespace osr
{

// What this run is, read off the environment in one place and settled before anything is built.
//
// Every one of these is an environment variable rather than a compile-time choice because the frame
// gates need one run each and a gate that has to be rebuilt before it can be run is a gate nobody
// runs.
//
// Reading them together is the point rather than a convenience. Two of the three combinations that
// do not exist are combinations *across* the variables — a chase camera and a scene with no vehicle
// to chase — so a scene that read only its own knob would refuse a misspelling and quietly ignore a
// contradiction. Refusing matters here more than it usually does: a run that fell back would compare
// one configuration's capture against another configuration's golden frame and report the difference
// as a rendering change.

export enum class SceneChoice {
    // Bathurst, the car under the vehicle model, and the game as it is played.
    Circuit,
    // The apron: a textured building, a ground plane, a bollard and a car that is placed rather
    // than simulated. Composed to be looked at, not driven on.
    Apron
};

export enum class CameraChoice { Chase, Fixed };

export enum class DriverChoice {
    // Whoever is at the keyboard, which under an unattended run is nobody.
    Driver,
    // A standing start keyed to the tick count, which is a function of the frame number under
    // RACEENGINE_DUMP_FRAME. This is what puts the driveline in front of the driving gate.
    Launch
};

export struct RunOptions
{
    SceneChoice scene = SceneChoice::Circuit;
    CameraChoice camera = CameraChoice::Chase;
    DriverChoice driver = DriverChoice::Driver;
};

export [[nodiscard]] RunOptions runOptions();

} // namespace osr

namespace osr
{

namespace
{

[[nodiscard]] std::string setting(const char* name)
{
    const auto* requested = std::getenv(name);

    return requested == nullptr ? std::string() : std::string(requested);
}

[[nodiscard]] SceneChoice scene()
{
    const auto value = setting("OSR_SCENE");
    if (value.empty() || value == "circuit")
    {
        return SceneChoice::Circuit;
    }

    if (value == "apron")
    {
        return SceneChoice::Apron;
    }

    throw std::runtime_error("OSR_SCENE names a scene this game does not have: '" + value +
                             "'. It takes 'circuit' or 'apron'.");
}

// Each scene states its own camera, so the gates name the scene and get the view that goes with it.
[[nodiscard]] CameraChoice camera(const SceneChoice chosen)
{
    const auto value = setting("OSR_CAMERA");
    if (value.empty())
    {
        return chosen == SceneChoice::Apron ? CameraChoice::Fixed : CameraChoice::Chase;
    }

    if (value == "fixed")
    {
        return CameraChoice::Fixed;
    }

    if (value == "chase")
    {
        if (chosen == SceneChoice::Apron)
        {
            throw std::runtime_error("OSR_CAMERA asks for the chase camera on the apron, which has no vehicle to "
                                     "chase: its car is a transform rather than a simulation.");
        }

        return CameraChoice::Chase;
    }

    throw std::runtime_error("OSR_CAMERA names a camera this game does not have: '" + value +
                             "'. It takes 'chase' or 'fixed'.");
}

[[nodiscard]] DriverChoice driver(const SceneChoice chosen)
{
    const auto value = setting("OSR_DRIVE");
    if (value.empty() || value == "driver")
    {
        return DriverChoice::Driver;
    }

    if (value == "launch")
    {
        if (chosen == SceneChoice::Apron)
        {
            throw std::runtime_error("OSR_DRIVE asks the apron's car to launch, and there is nothing to launch: it "
                                     "is placed rather than driven.");
        }

        return DriverChoice::Launch;
    }

    throw std::runtime_error("OSR_DRIVE names a driver this game does not have: '" + value +
                             "'. It takes 'driver' or 'launch'.");
}

} // namespace

RunOptions runOptions()
{
    const auto chosen = scene();

    return RunOptions{.scene = chosen, .camera = camera(chosen), .driver = driver(chosen)};
}

} // namespace osr
