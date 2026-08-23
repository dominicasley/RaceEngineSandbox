module;

#include <cstddef>
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

export enum class CameraChoice { Chase, Cockpit, Fixed };

export enum class DriverChoice {
    // Whoever is at the keyboard, which under an unattended run is nobody.
    Driver,
    // A standing start keyed to the tick count, which is a function of the frame number under
    // RACEENGINE_DUMP_FRAME. This is what puts the driveline in front of the driving gate.
    Launch
};

// Which of the car's electronics this run has switched on.
// A **session override** of what the setup sheet says, and not the place the assists live.
//
// Their home is `assets/Setups/golf-gti-mk7.setup`, where a driver can change them between two laps
// — see `AssistTune`. This exists for the same reason `OSR_BELT_MM` does and behaves the same way:
// something that has to be able to say "these, whatever is on disk" without editing the disk. The
// gates are the case that makes it load-bearing, since a sheet edit would otherwise move a golden.
//
// `stated` is what separates "unset, so the sheet decides" from "explicitly none".
export struct AssistSelection
{
    bool stated = false;
    bool antilock = false;
    bool traction = false;
    bool tractionSport = false;
    bool cornering = false;
};

export struct RunOptions
{
    SceneChoice scene = SceneChoice::Circuit;
    CameraChoice camera = CameraChoice::Chase;
    DriverChoice driver = DriverChoice::Driver;

    // Where to write the rack trace on the way out, if anywhere. Empty is the default and means the
    // attended behaviour: `rack-exit.csv` beside the binary after a session with a wheel in it, and
    // nothing at all from a gate.
    //
    // **It exists because the attended rule makes the trace unrepeatable, and a before-and-after
    // needs a repeatable one.** A driver's session is the right artefact for "the wheel did
    // something odd" and the wrong one for "did this change move the steering", because no two of
    // them drive the same lap. Named alongside `OSR_DRIVE=launch` and `RACEENGINE_DUMP_FRAME_AT`,
    // this dumps the scripted run's own trace, which is a function of the tick count and of nothing
    // else — the same file on any machine, before and after a change.
    std::string rackTrace;

    // How far along the tyre's belt a load spreads, in **millimetres**, or negative to leave the car's
    // own figure alone. `OSR_BELT_MM`.
    //
    // **A seat knob, and it is here for one session rather than forever.** The enveloping model is a
    // single number and its right value is a question about how a kerb should feel, which is not a
    // question any measurement in this workspace can answer — so it is exposed rather than guessed at,
    // and Dominic can walk it up and down inside one drive instead of one value per rebuild.
    // Millimetres because that is the unit the choice is being made in; everything below Physics is SI
    // and this is converted at the seam.
    //
    // It is deliberately outside the cross-variable validation the other knobs get: those three refuse
    // combinations that do not exist, and this one has no partner to contradict. What it *is* checked
    // for is being a number and being sane, because a typo that silently read as zero would look
    // exactly like "the belt does nothing".
    double beltBridgingMillimetres = -1.0;

    // A session override of the setup sheet's assists. `OSR_ASSISTS`, a comma-separated list of
    // `abs`, `tc`, `tc-sport` and `xds`, or the single word `none`; **unset means the sheet decides**,
    // which is where they belong.
    //
    // Beside the cross-variable validation rather than inside it, for `OSR_BELT_MM`'s reason: those
    // three refuse combinations that cannot exist and this has no partner to contradict. It is still
    // checked for naming something, because a typo that read as "off" would look exactly like an
    // assist that does nothing — and `none` exists so that forcing them off is a word rather than
    // the absence of one.
    AssistSelection assists{};
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

    if (value == "cockpit")
    {
        if (chosen == SceneChoice::Apron)
        {
            throw std::runtime_error("OSR_CAMERA asks for the cockpit camera on the apron, which has no vehicle to "
                                     "sit in: its car is a transform rather than a simulation.");
        }

        return CameraChoice::Cockpit;
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
                             "'. It takes 'chase', 'cockpit' or 'fixed'.");
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

[[nodiscard]] std::string rackTrace(const SceneChoice chosen)
{
    auto value = setting("OSR_DUMP_RACK");
    if (value.empty())
    {
        return value;
    }

    // The apron's car is a transform: there is no vehicle, no steering rack and nothing publishing
    // a torque, so the file this asked for would be written empty or not at all. Refused rather
    // than ignored, for the reason every other cross-variable check here is — a run that quietly
    // produced nothing would be read as "the change moved no torque".
    if (chosen == SceneChoice::Apron)
    {
        throw std::runtime_error("OSR_DUMP_RACK asks for the apron's rack trace, and the apron has no rack: its car "
                                 "is placed rather than simulated.");
    }

    return value;
}

// A comma-separated list, and an unknown word is refused rather than ignored.
[[nodiscard]] AssistSelection assists()
{
    auto chosen = AssistSelection{};

    const auto value = setting("OSR_ASSISTS");
    if (value.empty())
    {
        return chosen;
    }

    auto from = std::string::size_type{0};
    while (from <= value.size())
    {
        const auto to = value.find(',', from);
        const auto word = value.substr(from, to == std::string::npos ? std::string::npos : to - from);

        if (word == "none")
        {
            // Says nothing beyond "stated", which is the whole point of it: an empty value means the
            // sheet decides, and a gate needs a way to say off that a sheet cannot argue with.
            chosen.stated = true;
        }
        else if (word == "abs")
        {
            chosen.antilock = true;
        }
        else if (word == "tc")
        {
            chosen.traction = true;
        }
        else if (word == "tc-sport")
        {
            chosen.tractionSport = true;
        }
        else if (word == "xds")
        {
            chosen.cornering = true;
        }
        else if (!word.empty())
        {
            throw std::runtime_error("OSR_ASSISTS names something this car does not have: '" + word +
                                     "'. It takes a comma-separated list of 'abs', 'tc', 'tc-sport' and 'xds', or "
                                     "the single word 'none'. Leave it unset to use the setup sheet's.");
        }

        chosen.stated = chosen.stated || !word.empty();

        if (to == std::string::npos)
        {
            break;
        }

        from = to + 1;
    }

    if (chosen.stated && chosen.antilock == false && chosen.traction == false && chosen.tractionSport == false &&
        chosen.cornering == false && value != "none")
    {
        throw std::runtime_error("OSR_ASSISTS was set to '" + value +
                                 "' and names nothing. Leave it unset to use the setup sheet's, or say 'none'.");
    }

    if (chosen.traction && chosen.tractionSport)
    {
        throw std::runtime_error("OSR_ASSISTS asks for both 'tc' and 'tc-sport', which are two settings of one "
                                 "system rather than two systems. Name one.");
    }

    return chosen;
}

[[nodiscard]] double beltBridgingMillimetres()
{
    const auto value = setting("OSR_BELT_MM");
    if (value.empty())
    {
        return -1.0;
    }

    auto consumed = std::size_t{0};
    auto millimetres = 0.0;

    try
    {
        millimetres = std::stod(value, &consumed);
    }
    catch (const std::exception&)
    {
        throw std::runtime_error("OSR_BELT_MM is not a number: '" + value + "'.");
    }

    if (consumed != value.size())
    {
        throw std::runtime_error("OSR_BELT_MM is not a number: '" + value + "'.");
    }

    // Zero is meaningful and is the bed of independent springs, so it is allowed. Negative is not,
    // because negative is this option's own "leave it alone" and a caller cannot mean both.
    if (millimetres < 0.0)
    {
        throw std::runtime_error("OSR_BELT_MM cannot be negative: '" + value +
                                 "'. Zero is the uncoupled bed; leave it unset to use the car's own figure.");
    }

    // A belt that spreads a load further than the contact patch is long is not a tyre, and past about
    // this the solver's fixed sweep count stops converging as well — so it is refused rather than
    // quietly under-solved. 200 mm is already more than the patch is wide.
    if (millimetres > 200.0)
    {
        throw std::runtime_error("OSR_BELT_MM is longer than the contact patch: '" + value +
                                 "'. The fixed iteration budget does not converge past about 200.");
    }

    return millimetres;
}

} // namespace

RunOptions runOptions()
{
    const auto chosen = scene();
    const auto chosenAssists = assists();

    return RunOptions{.scene = chosen,
                      .camera = camera(chosen),
                      .driver = driver(chosen),
                      .rackTrace = rackTrace(chosen),
                      .beltBridgingMillimetres = beltBridgingMillimetres(),
                      .assists = chosenAssists};
}

} // namespace osr
