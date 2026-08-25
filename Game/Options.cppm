module;

#include <array>
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

// Where the fixed camera stands and which way it points, when this run wants to say rather than
// take the scene's own.
//
// **This exists to reproduce a view somebody else saw, which is a thing this project has needed and
// not had.** A bug reported from the seat — "the ground looks transparent from up here" — is a claim
// about a *view*, and the only way to test it is to stand in the same place looking the same way.
// Every other knob here describes the world; this one describes the observer.
//
// The two halves are stated separately because they are wanted separately: re-aiming from the
// scene's own stand is a common thing to want, and so is standing somewhere else and keeping the
// heading. Whichever is unstated stays the scene's.
export struct CameraPose
{
    // Metres of track coordinates, which is what the scene states its own stand in and what the
    // physics world is in. The tenth-of-a-metre world unit is applied at the seam, as everywhere.
    bool positionStated = false;
    double xMetres = 0.0;
    double yMetres = 0.0;
    double zMetres = 0.0;

    // Degrees. Zero yaw looks along positive z and positive pitch is above the horizon, which is
    // the convention `FPSCameraController` builds its direction back from — the same one the pose
    // the game prints is written in, so what it prints can be pasted back verbatim.
    bool lookStated = false;
    double yawDegrees = 0.0;
    double pitchDegrees = 0.0;
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

    // A multiplier on the rig's fog density. `OSR_FOG`, a number, or the word `off` for zero; unset
    // is 1.0, which is the rig's own figure untouched.
    //
    // Here for `OSR_BELT_MM`'s reason and with the same standing: how much air a scene stands in is
    // a look decision, the look is Dominic's, and a knob that needs a rebuild between two readings
    // is a knob nobody turns. It is a *multiplier* rather than a density because what is being
    // compared is thicker-or-thinner against a settled starting point, and because `off` then has an
    // unambiguous meaning — the same frame this engine drew before there was any fog in it, which is
    // the A/B the whole feature has to be judged against.
    //
    // Outside the cross-variable validation, again for that knob's reason: those three refuse
    // combinations that cannot exist, and the air has no partner to contradict.
    double fogDensityScale = 0.5;

    // How hard the rain falls, 0..1-ish. `OSR_RAIN`, a number, or the word `off`; unset is 0.0,
    // the dry scene — off is the default here where the fog's default is the rig's own figure,
    // because the rig authors a fog and authors no rain. A look knob outside the cross-variable
    // validation for the fog knob's reason exactly.
    double rainIntensity = 1.0;

    // The sun's elevation above the horizon, in degrees. `OSR_SUN`; the default is the rig's own
    // early-morning figure.
    //
    // **This is the time of day, and it is one number because everything else derives from it**: the
    // sky is a scattering integral that follows the light, the probes photograph that sky and hand it
    // back as indirect light, and the fog takes its shafts from the sun's colour and its haze from
    // those probes. Nothing else has to be told the hour.
    //
    // A knob rather than a constant because how low a sunrise wants to be is a look decision made in
    // the seat: at six degrees the world is dim and the drama is all in the sky, and by twelve the
    // light rakes across the track and the ground is legible again. Negative is allowed and is the
    // sun below the horizon — a legitimate scene, and a very dark one.
    double sunElevationDegrees = 6.0;

    // `OSR_CAM_POS` and `OSR_CAM_LOOK`. Inside the cross-variable validation rather than beside it,
    // unlike the look multipliers: this one *does* have a partner to contradict, because only the
    // fixed camera has a stand for it to name.
    CameraPose cameraPose{};
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

// A look multiplier: a number, or the word `off` for the control. It served two knobs until the lens
// dirt plate was removed (2026-08-24) and is kept as a function rather than inlined into `OSR_FOG`
// because the next one of these will want it and because `off` is worth stating once.
[[nodiscard]] double lookMultiplier(const char* name)
{
    const auto value = setting(name);
    if (value.empty())
    {
        return 1.0;
    }

    // A word rather than a magic zero for the case that matters most, and for `OSR_ASSISTS`'s
    // reason: "none of this effect" is the control every judgement about it is made against, and it
    // should not be spelled as something somebody has to recognise.
    if (value == "off")
    {
        return 0.0;
    }

    auto consumed = std::size_t{0};
    auto scale = 0.0;

    try
    {
        scale = std::stod(value, &consumed);
    }
    catch (const std::exception&)
    {
        throw std::runtime_error(std::string(name) + " is neither a number nor 'off': '" + value + "'.");
    }

    if (consumed != value.size())
    {
        throw std::runtime_error(std::string(name) + " is neither a number nor 'off': '" + value + "'.");
    }

    if (scale < 0.0)
    {
        throw std::runtime_error(std::string(name) + " cannot be negative: '" + value +
                                 "'. It multiplies the scene's own figure; 'off' or 0 is none of it.");
    }

    return scale;
}

// The rain, `OSR_RAIN`: a number for how hard it falls, `off` or unset for the dry scene. Not
// `lookMultiplier`, whose unset means "the rig's own figure untouched" — there is no authored
// rain for a multiplier to leave untouched, so unset here is zero and `off` is the same statement
// made as a word.
[[nodiscard]] double rainIntensity()
{
    const auto value = setting("OSR_RAIN");
    if (value.empty() || value == "off")
    {
        return 0.0;
    }

    auto consumed = std::size_t{0};
    auto intensity = 0.0;

    try
    {
        intensity = std::stod(value, &consumed);
    }
    catch (const std::exception&)
    {
        throw std::runtime_error("OSR_RAIN is neither a number nor 'off': '" + value + "'.");
    }

    if (consumed != value.size())
    {
        throw std::runtime_error("OSR_RAIN is neither a number nor 'off': '" + value + "'.");
    }

    if (intensity < 0.0)
    {
        throw std::runtime_error("OSR_RAIN cannot be negative: '" + value + "'. 'off' or 0 is the dry scene.");
    }

    return intensity;
}

// Degrees rather than a multiplier, so this one is parsed on its own terms: there is no "off" for a
// time of day, and the range refused is the one that is not an elevation at all.
[[nodiscard]] double sunElevationDegrees()
{
    const auto value = setting("OSR_SUN");
    if (value.empty())
    {
        return 6.0;
    }

    auto consumed = std::size_t{0};
    auto degrees = 0.0;

    try
    {
        degrees = std::stod(value, &consumed);
    }
    catch (const std::exception&)
    {
        throw std::runtime_error("OSR_SUN is not a number of degrees: '" + value + "'.");
    }

    if (consumed != value.size())
    {
        throw std::runtime_error("OSR_SUN is not a number of degrees: '" + value + "'.");
    }

    // Below the horizon is night and is allowed; past the zenith is not a sun angle.
    if (degrees < -20.0 || degrees > 90.0)
    {
        throw std::runtime_error("OSR_SUN is an elevation in degrees and lies between -20 and 90: '" + value + "'.");
    }

    return degrees;
}

// A comma-separated list of exactly `count` numbers, refused rather than padded or truncated if it
// is not. Shared by the two halves of the camera pose because they differ only in how many numbers
// they want and in what those numbers mean — and because a knob whose whole purpose is to reproduce
// somebody else's view must refuse a mistyped one loudly, or it stands somewhere else and the
// artefact simply is not there.
[[nodiscard]] std::array<double, 3> numbers(const char* name, const std::string& value, const std::size_t count,
                                            const char* form)
{
    auto parsed = std::array<double, 3>{};
    auto found = std::size_t{0};
    auto from = std::string::size_type{0};

    while (true)
    {
        const auto to = value.find(',', from);
        const auto word = value.substr(from, to == std::string::npos ? std::string::npos : to - from);

        if (found == count)
        {
            throw std::runtime_error(std::string(name) + " takes " + form + ", and this states more: '" + value +
                                     "'.");
        }

        auto consumed = std::size_t{0};

        try
        {
            parsed.at(found) = std::stod(word, &consumed);
        }
        catch (const std::exception&)
        {
            throw std::runtime_error(std::string(name) + " takes " + form + ", and '" + word +
                                     "' is not a number: '" + value + "'.");
        }

        if (consumed != word.size())
        {
            throw std::runtime_error(std::string(name) + " takes " + form + ", and '" + word +
                                     "' is not a number: '" + value + "'.");
        }

        ++found;

        if (to == std::string::npos)
        {
            break;
        }

        from = to + 1;
    }

    if (found != count)
    {
        throw std::runtime_error(std::string(name) + " takes " + form + ", and this states fewer: '" + value + "'.");
    }

    return parsed;
}

[[nodiscard]] CameraPose cameraPose(const CameraChoice chosen)
{
    auto pose = CameraPose{};

    const auto position = setting("OSR_CAM_POS");
    const auto look = setting("OSR_CAM_LOOK");

    if (position.empty() && look.empty())
    {
        return pose;
    }

    // Both name where the *fixed* camera stands, and the other two cameras have no stand to name:
    // the chase and cockpit controllers compute a position and a direction from the car on every
    // tick, so a pose stated for one of those would be overwritten before the first frame — which
    // reads exactly like the variable being ignored, and this file exists to refuse that.
    if (chosen != CameraChoice::Fixed)
    {
        throw std::runtime_error("OSR_CAM_POS and OSR_CAM_LOOK place the fixed camera, and this run asks for the "
                                 "chase or cockpit camera, which writes its own position and direction from the car "
                                 "on every tick. Name OSR_CAMERA=fixed alongside them.");
    }

    if (!position.empty())
    {
        const auto metres = numbers("OSR_CAM_POS", position, 3, "three comma-separated metres, 'x,y,z'");

        pose.positionStated = true;
        pose.xMetres = metres.at(0);
        pose.yMetres = metres.at(1);
        pose.zMetres = metres.at(2);
    }

    if (!look.empty())
    {
        const auto degrees = numbers("OSR_CAM_LOOK", look, 2, "two comma-separated degrees, 'yaw,pitch'");

        pose.lookStated = true;
        pose.yawDegrees = degrees.at(0);
        pose.pitchDegrees = degrees.at(1);
    }

    return pose;
}

} // namespace

RunOptions runOptions()
{
    const auto chosen = scene();
    const auto chosenCamera = camera(chosen);
    const auto chosenAssists = assists();

    return RunOptions{.scene = chosen,
                      .camera = chosenCamera,
                      .driver = driver(chosen),
                      .rackTrace = rackTrace(chosen),
                      .beltBridgingMillimetres = beltBridgingMillimetres(),
                      .assists = chosenAssists,
                      .fogDensityScale = lookMultiplier("OSR_FOG"),
                      .rainIntensity = rainIntensity(),
                      .sunElevationDegrees = sunElevationDegrees(),
                      .cameraPose = cameraPose(chosenCamera)};
}

} // namespace osr
