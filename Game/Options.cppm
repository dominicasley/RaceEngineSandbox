module;

#include <array>
#include <cstddef>
#include <cstdlib>
#include <optional>
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

export enum class TrackChoice {
    // Grand City Parkway: ExPanda's fictional American city, a freeroam layout with 18.7 km of
    // street in it. **The default since 2026-09-06, on Dominic's instruction.**
    GrandCityParkway,
    // Mount Panorama, and the circuit every driving measurement in `docs/` was taken on. **Both
    // frame gates name it explicitly** — see `scripts/verify-parity.sh` — because a golden frame is
    // a photograph of one world and a gate that followed this default would have gone red the day
    // the default moved and reported it as a rendering change.
    Bathurst
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
// Where a session's tyres start. Three answers and no fourth: the car's own seed, the track's
// temperature, or a number this run states.
//
// `Track` cannot be resolved here, and that is deliberate rather than awkward. This file imports
// nothing — it is one of the two translation units in the workspace that pay none of the module
// cost, which is why it is the one place the environment is read — so it cannot call the physics
// that derives a track temperature from an air temperature and a sun. It names the intent and the
// scene resolves it.
export enum class TyreTemperatureSource { CarsOwn, Track, Stated };

export struct TyreTemperatureChoice
{
    TyreTemperatureSource source = TyreTemperatureSource::CarsOwn;
    double celsius = 0.0;
};

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
    // Which circuit the driving scene builds. `OSR_TRACK`, the word `gcp` or the word `bathurst`.
    //
    // It is the one knob here that changes what world the game is, so it is validated against the
    // scene like the camera and the driver are: the apron is a composed fixture with no track in it
    // at all, and naming a track alongside it is a request the game cannot honour.
    TrackChoice track = TrackChoice::GrandCityParkway;
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

    // Which load-path model the car runs, or unset for the car's own. `OSR_LOAD_PATH`, the word
    // `geometric` or the word `springs`.
    //
    // A seat knob, here for `OSR_BELT_MM`'s reason and with the same standing: whether the body
    // leans the right amount is a question only Dominic can answer, and it is answered by flipping
    // between two laps rather than between two builds. `springs` is the control — every newton of
    // load transfer through a spring, which is the model every measured figure in docs/ was taken
    // under and the one that produced the seat report this exists to answer. `geometric` is the
    // car's own setting since 2026-08-27: the tyre's in-plane forces reaching the corner's degree of
    // freedom through the linkage, which is roll centre, jacking, anti-dive and anti-squat at once.
    //
    // Outside the cross-variable validation, again for that knob's reason — a load path has no
    // partner among scene, camera and driver to contradict. `docs/suspension-load-path-brief.md`.
    std::optional<bool> geometricLoadPath;

    // Whether the wheels' own spin reacts on the rest of the car. `OSR_DRIVELINE_REACTION`, the word
    // `on` or the word `off`, unset for the car's own setting — which is **on** for the Golf since
    // 2026-08-27 late, and off on the placeholder.
    //
    // A seat knob for the same reason as the one above and with the same standing. What it adds is
    // the couple a spinning-up or slowing-down wheel takes out of the body, plus the shaft torque's
    // virtual work in the driven corner. It was built and shipped **off** so the verdict would be
    // Dominic's; he drove it and kept it, and `off` is now the control and the way back.
    //
    // **It costs measured things, and the figure to quote is the anti-lock one.** The term is the
    // wheel's own `I·alpha` and anti-lock cycling *is* wheel angular acceleration, so a clean
    // full-pedal stop barely excites it: that fixture says +1.7%, and an ABS stop from 100 km/h says
    // **+5.6%**, 42.18 → 44.54 m. `docs/suspension-fidelity-brief.md`, item 3, and
    // `docs/known-red.md` for the five reds it opened.
    std::optional<bool> drivelineReaction;

    // Whether the city's derived collision stands. `OSR_WORLD_COLLIDERS`, the word `on` or the word
    // `off`, and unset is **on** — which is a change of world rather than a change of setting, so it
    // is here as the control rather than as a preference.
    //
    // What it switches is the pair of collider exports beside the track: about 8150 building hulls
    // and the static street furniture, every one of them a convex solid derived from geometry
    // Assetto Corsa only ever drew. `off` is the city as this game had it before 2026-09-06, which
    // is a city whose facades a car passes straight through. A track that states no collider assets
    // is unaffected either way, and Mount Panorama is one — so both frame gates see nothing of this.
    bool worldColliders = true;

    // Whether the draw walk skips geometry the frame's own prepass proved is hidden.
    // `OSR_OCCLUSION`, the word `on` or the word `off`, and unset is **on**.
    //
    // It is here as a control rather than as a preference, and it is the one visibility test in the
    // frame that is not exact: the grid a box is tested against is a frame or two old, so `off` is
    // the answer to "did the culler delete that". A city is what it exists for — the tall side of
    // Grand City Parkway hides most of an eighteen-million-triangle map from any street in it — and
    // a circuit with nothing standing between the camera and the horizon gets very little from it.
    bool occlusionCulling = true;

    // Where a session's tyres start, in degrees Celsius. `OSR_TYRE_TEMP`: a number, or the word
    // `ambient` for the track's own temperature.
    //
    // **Unset is now `ambient` too, and that changed on 2026-08-28 when the thermal model went on for
    // good.** While it shipped off the default had to be the middle of the compound's plateau, which
    // is the one seed under which switching the model on changes nothing and is what both parity
    // gates' inertness proof stood on. With the model on that argument is spent and the physical one
    // is what is left: a car in a garage has cold tyres. `OSR_TYRE_TEMP=85` is the way back to the
    // old default.
    //
    // **`ambient` is the TRACK's temperature and not the air's**, because that is what the rubber is
    // resting on — 11.5 °C apart under this scene's own sun. The name is the one the knob shipped
    // with and it is kept rather than corrected, because a seat report that says "ambient" means this.
    TyreTemperatureChoice tyreTemperature{};

    // Whether the tyres carry a temperature. `OSR_TYRE_THERMAL`, the word `on` or the word `off`,
    // unset for the car's own setting — which is **on for the Golf since 2026-08-28**, on Dominic's
    // instruction and after he drove it.
    //
    // What it adds is the tread's own heat balance and grip following the tread core through the
    // compound's curve. **`off` is the tyre every performance figure in docs/ was taken on before
    // that date**: one that is permanently at its best. It is the control and the way back, and it is
    // what any figure older than the switch has to be read against. docs/tyre-state-brief.md.
    std::optional<bool> tyreThermal;

    // Whether the air inside the tyres carries a temperature and a pressure. `OSR_TYRE_PRESSURE`,
    // the word `on` or the word `off`, unset for the car's own setting — which is **on for the Golf
    // since 2026-08-29**, on Dominic's instruction: *"put it on. just because i cant feel something
    // doesn't mean its bad."*
    //
    // What it adds is the gas law and two couplings that follow from it: a cold tyre is a softer
    // spring and a draggier one. **`off` is the control and the way back**, and it is what every
    // performance figure older than the switch was measured on — a car permanently at the ideal
    // pressure its own vertical rate and rolling resistance are quoted at.
    // `docs/tyre-state-brief.md`, section 7.
    std::optional<bool> tyrePressure;

    // The thermal contact conductance of the tread-road interface, W/(m²·K). `OSR_TYRE_CONTACT`: a
    // number, or the word `perfect`; unset leaves the car's own figure alone.
    //
    // **The shipped car states the sourced figure, 25200, since 2026-08-29** (NASA TN D-8161, 1976,
    // rubber on asphalt, a lower limit) — worth about a fifth of the road path and one to two degrees
    // on the tread core, both far below anything a seat can resolve, and no golden moved for it.
    // `OSR_TYRE_CONTACT=perfect` is the control and the way back to the pre-2026-08-29 road path.
    // `docs/tyre-state-brief.md`.
    std::optional<double> tyreContactConductance;

    // What fraction of the geometric contact patch conducts into the road. `OSR_TYRE_ROAD_AREA`: a
    // fraction; unset leaves the car's own figure alone.
    //
    // **The Golf states 0.72 since 2026-08-29** — 28% of this tread is groove and a groove does not
    // touch the road — so `OSR_TYRE_ROAD_AREA=1.0` is the control now: the gross patch, which is
    // what the model multiplied its road conductance by until then. A hole in the conducting area
    // rather than a second path, and a different mechanism from `OSR_TYRE_CONTACT`, whose measured
    // figure is a smooth-tread one. `docs/tyre-state-brief.md`.
    std::optional<double> tyreRoadAreaFraction;

    // Where this compound's grip plateau is centred, degrees Celsius. `OSR_TYRE_IDEAL`: a number;
    // unset leaves the car's own window alone, which is **65 °C for the Golf since 2026-08-28**.
    //
    // **It slides the whole curve along its temperature axis and changes nothing about its shape.**
    // AC's `tcurve_semis.lut` came with its plateau at 75–95 °C, corroborated by a Michelin bulletin
    // for the Pilot Sport Cup 2 R — a **track** tyre — while this car's tread depth and mass are a
    // road tyre's. Two published sources put a **summer road** tyre's design operating temperature at
    // around **50 °C**, so the curve was slid down by the smaller of the two bounds those sources give.
    //
    // **`OSR_TYRE_IDEAL=85` is the way back to the track window**, and it is what every performance
    // figure older than 2026-08-28 was measured on. 45 is the far end of the sourced band, where a
    // lap's warm-up is worth half a per cent and the tyre is effectively always ready.
    // `docs/tyre-state-brief.md`.
    std::optional<double> tyreIdealTemperature;

    // Whether the brake discs carry a temperature and the pads fade with it. `OSR_BRAKE_THERMAL`,
    // the word `on` or the word `off`, unset for the car's own setting — **on for the Golf since
    // 2026-08-28**, switched on in the same instruction as the tyre's.
    //
    // Independent of it on purpose, and it stays independent now that both default on: `off` here is
    // fade taken away from a car whose tyres still carry heat, which is the control for the fade half
    // alone. It also carries stage 3, the path from the disc into the wheel and the tyre.
    // `docs/brake-thermal-brief.md`.
    std::optional<bool> brakeThermal;

    // The air temperature, degrees Celsius. `OSR_AIR_TEMP`; unset is 20.
    //
    // **A scene property and one number, on the sun's own pattern**: the track temperature is derived
    // from this and from the sun's elevation rather than being a second knob, because a road in the
    // sun is warmer than the air over it by an amount the sun's angle decides and not by an amount
    // somebody types. Nothing in the physics knew the weather at all until 2026-08-28 and one thing
    // reads it now — the tread's heat balance — so it does nothing whatever with `OSR_TYRE_THERMAL`
    // off.
    //
    // Outside the cross-variable validation, like the look knobs: the weather has no partner among
    // scene, camera and driver to contradict.
    double airTemperatureCelsius = 20.0;

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

    // How much of the sky is cloud, 0..1-ish. `OSR_CLOUDS`, a number, or the word `off`; unset is
    // 0.0, the clear sky — parsed on `OSR_RAIN`'s terms exactly and for its reason: the rig
    // authors no cloud, so there is nothing for a multiplier to leave untouched. Rain couples in
    // one direction (a raining scene imposes a coverage floor at the rig), so this knob is what
    // adds cloud to a dry day. Outside the cross-variable validation, like the rain.
    double cloudCoverage = 0.0;

    // The cloud dome map's texel count, `OSR_CLOUD_MAP=<width>x<height>`; unset is 1024x512, the
    // size the rig used before this was a knob. The one cloud setting that trades frame rate for
    // detail directly, which is why it is here and not a constant — see cloudMapSize for what the
    // two axes are worth. Outside the cross-variable validation, like the fog and rain knobs: it
    // is a quality dial, and no combination of it with a scene or a camera is impossible.
    int cloudMapWidth = 1024;
    int cloudMapHeight = 512;

    // How the dome mixes each march into the map it is writing. `OSR_CLOUD_BLEND`: a weight in
    // (0, 1], where 1 is a plain overwrite; the word `alpha` puts back the *transmittance* doing
    // the mixing, which is what this pass did until 2026-08-27 and is now only the control.
    //
    // This is stage 0 of docs/cloud-amortisation-brief.md, settled on Dominic's call, and it is a
    // look decision rather than plumbing. The dome writes `vec4(radiance, transmittance)` into a
    // blending pass, so what the map stored was `radiance * T + previous * (1 - T)` — an
    // accumulation over frames whose weight is the cloud's own transparency, which nobody chose.
    // Its fixed point is the marched answer, so it is a *lag* rather than a different picture; but
    // the lag is worst exactly where the cloud is thickest, because that is where T is smallest,
    // and at T = 0.01 the time constant is a hundred frames. Measured: at frame 600 the map had
    // still not converged, and the shipped sky differs from the marched one by mean 3.72/255 at a
    // worst block of 184.
    //
    // **It is also the recorded run-to-run sky flake, and stating the weight is what closes it.**
    // A hundred-frame time constant is long enough to remember a run's startup transient to the end
    // of the run; a stated weight washes it out in a few frames. Two identical captures are
    // bit-identical at 0.5 and at 1.0, and differ by ~22 pixels of 1 at `alpha` — with the load op
    // explicit either way, so the undefined load the brief suspected was not the cause.
    //
    // And it has to be stated before the march can be amortised at all: hold the map for N frames
    // and the accumulator's time constant is multiplied by N with it. 0.5 rather than 1.0 because
    // the two are visually the same thing — mean 0.05/255 apart — and a weight below one softens
    // the step the cadence introduces for free.
    std::optional<double> cloudBlendWeight = 0.5;

    // How many frames apart the dome pass runs at all. `OSR_CLOUD_EVERY`, a whole number of frames;
    // unset is 4. Between runs the pass is held and the frame spends nothing on it. One is every
    // frame, which is what the map was given until 2026-08-27 and is the control this was measured
    // against.
    //
    // Stage 1 of the same brief, and the arithmetic that says it is safe is angular: the map is
    // lat-long in **world direction**, so turning the camera cannot stale it and only two things
    // can — the clouds drifting and the camera translating far enough to parallax a near cloud. The
    // drift is 9 m/s and subtends 0.011 degrees over eight frames at 4 km; 200 km/h for eight frames
    // is 4.9 m, which at a 1.5 km cloud is 0.19 degrees. A 1024-wide map's azimuth texel is 0.35
    // degrees, so eight frames of either is under one texel of the thing being written.
    //
    // Frames rather than a rate in Hz on purpose: a cadence in wall time would make a capture
    // depend on how fast the machine ran, and the gates are the reason nothing here is allowed to.
    int cloudMarchInterval = 4;

    // How many vertical strips one full refresh of the map is spread over. `OSR_CLOUD_STRIPS`;
    // unset is 2, and 1 is the whole map in one pass. **A texel is therefore refreshed every
    // `cloudMarchInterval * cloudMarchStrips` frames — eight, as shipped** — and the two knobs are
    // orthogonal: one says how often the pass runs, the other how much of the map it writes when it
    // does.
    //
    // Stage 2 of the brief, and the reason it is not simply a longer interval is the *worst* frame.
    // Measured 2026-08-27, cockpit at 2560x1440, `OSR_CLOUDS=0.45`, against a 10.93 ms frame with
    // the dome marching every frame and a 6.60 ms frame with no clouds in it at all. All four of
    // these refresh a texel every eight frames:
    //
    //   every 8, 1 strip    mean 7.30 ms   p95 11.34 ms   137 fps   <- one frame in eight pays it all
    //   every 1, 8 strips   mean 8.01 ms   p95  8.78 ms   125 fps   <- flat, and dearer
    //   every 2, 4 strips   mean 7.59 ms   p95  9.15 ms   132 fps
    //   every 4, 2 strips   mean 7.36 ms   p95  9.59 ms   136 fps   <- shipped
    //
    // Splitting the pass is not free — a strip of an eighth costs 1.55 ms where an eighth of the
    // march would be 0.55, so about 1.2 ms of the 4.39 is per-pass and does not divide. That is why
    // the shipped pair takes only two strips and buys the rest of the interval by holding: it keeps
    // the holding scheme's mean and takes two of its three milliseconds of spike away.
    int cloudMarchStrips = 2;

    // The sun's elevation above the horizon, in degrees. `OSR_SUN`; the default is half past four
    // in the afternoon, derived rather than picked: at Bathurst's latitude (33.4 degrees south),
    // 16:30 solar time is an hour angle of 67.5 degrees, and at the equinox
    // sin(elevation) = cos(33.4) x cos(67.5) = 0.319, which is 18.6 degrees — stated as 19.
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
    double sunElevationDegrees = 19.0;

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
[[nodiscard]] TrackChoice track(const SceneChoice chosen)
{
    const auto value = setting("OSR_TRACK");
    if (value.empty())
    {
        return TrackChoice::GrandCityParkway;
    }

    // Stated on the apron it is a contradiction rather than a harmless extra: that scene builds no
    // physics world and loads no track, so a run naming one would silently get the scene it asked
    // for and none of the world it asked for.
    if (chosen == SceneChoice::Apron)
    {
        throw std::runtime_error("OSR_TRACK names a circuit for the apron, which has no circuit in it: its world is a "
                                 "building, a ground plane, a bollard and a placed car.");
    }

    if (value == "gcp")
    {
        return TrackChoice::GrandCityParkway;
    }

    if (value == "bathurst")
    {
        return TrackChoice::Bathurst;
    }

    throw std::runtime_error("OSR_TRACK names a circuit this game does not carry: '" + value +
                             "'. It takes 'gcp' or 'bathurst'.");
}

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

[[nodiscard]] std::optional<bool> geometricLoadPath()
{
    const auto value = setting("OSR_LOAD_PATH");
    if (value.empty())
    {
        return std::nullopt;
    }

    if (value == "geometric")
    {
        return true;
    }

    if (value == "springs")
    {
        return false;
    }

    // Two words and no third reading. A number would be the obvious alternative and is refused on
    // purpose: this is not a dial, and "0.5 of a load path" is not a thing a linkage can do.
    throw std::runtime_error("OSR_LOAD_PATH is 'geometric' or 'springs', not '" + value +
                             "'. Unset leaves the car's own setting alone.");
}

[[nodiscard]] std::optional<bool> drivelineReaction()
{
    const auto value = setting("OSR_DRIVELINE_REACTION");
    if (value.empty())
    {
        return std::nullopt;
    }

    if (value == "on")
    {
        return true;
    }

    if (value == "off")
    {
        return false;
    }

    throw std::runtime_error("OSR_DRIVELINE_REACTION is 'on' or 'off', not '" + value +
                             "'. Unset leaves the car's own setting alone.");
}

[[nodiscard]] bool worldColliders()
{
    const auto value = setting("OSR_WORLD_COLLIDERS");
    if (value.empty() || value == "on")
    {
        return true;
    }

    if (value == "off")
    {
        return false;
    }

    throw std::runtime_error("OSR_WORLD_COLLIDERS is 'on' or 'off', not '" + value +
                             "'. Unset is 'on', which is the city with its buildings solid.");
}

[[nodiscard]] bool occlusionCulling()
{
    const auto value = setting("OSR_OCCLUSION");
    if (value.empty() || value == "on")
    {
        return true;
    }

    if (value == "off")
    {
        return false;
    }

    throw std::runtime_error("OSR_OCCLUSION is 'on' or 'off', not '" + value +
                             "'. Unset is 'on', which is the draw walk skipping what the prepass "
                             "proved is hidden.");
}

[[nodiscard]] TyreTemperatureChoice tyreTemperature()
{
    const auto value = setting("OSR_TYRE_TEMP");
    if (value.empty())
    {
        return TyreTemperatureChoice{};
    }

    if (value == "ambient")
    {
        return TyreTemperatureChoice{.source = TyreTemperatureSource::Track};
    }

    auto consumed = std::size_t{0};
    auto degrees = 0.0;

    try
    {
        degrees = std::stod(value, &consumed);
    }
    catch (const std::exception&)
    {
        throw std::runtime_error("OSR_TYRE_TEMP is a temperature in Celsius or the word 'ambient', not '" + value +
                                 "'.");
    }

    if (consumed != value.size())
    {
        throw std::runtime_error("OSR_TYRE_TEMP is a temperature in Celsius or the word 'ambient', not '" + value +
                                 "'.");
    }

    if (degrees < -30.0 || degrees > 250.0)
    {
        throw std::runtime_error("OSR_TYRE_TEMP is a tread temperature in Celsius and lies between -30 and 250: '" +
                                 value + "'.");
    }

    return TyreTemperatureChoice{.source = TyreTemperatureSource::Stated, .celsius = degrees};
}

[[nodiscard]] std::optional<bool> tyreThermal()
{
    const auto value = setting("OSR_TYRE_THERMAL");
    if (value.empty())
    {
        return std::nullopt;
    }

    if (value == "on")
    {
        return true;
    }

    if (value == "off")
    {
        return false;
    }

    throw std::runtime_error("OSR_TYRE_THERMAL is 'on' or 'off', not '" + value +
                             "'. Unset leaves the car's own setting alone.");
}

// The same shape as `OSR_TYRE_THERMAL`, and separate from it because the two models switch
// independently — pressure with the tread's model off is a gas that never warms, which is a
// legitimate control and a dull car.
[[nodiscard]] std::optional<bool> tyrePressure()
{
    const auto value = setting("OSR_TYRE_PRESSURE");
    if (value.empty())
    {
        return std::nullopt;
    }

    if (value == "on")
    {
        return true;
    }

    if (value == "off")
    {
        return false;
    }

    throw std::runtime_error("OSR_TYRE_PRESSURE is 'on' or 'off', not '" + value +
                             "'. Unset leaves the car's own setting alone.");
}

// W/(m²·K), or the word `perfect`, which is zero and is the model with no interface resistance at
// all. Parsed on `OSR_TYRE_TEMP`'s terms — a number or one word — because it is the same shape of
// question: a physical quantity with one named special case.
[[nodiscard]] std::optional<double> tyreContactConductance()
{
    const auto value = setting("OSR_TYRE_CONTACT");
    if (value.empty())
    {
        return std::nullopt;
    }

    if (value == "perfect")
    {
        return 0.0;
    }

    auto consumed = std::size_t{0};
    auto conductance = 0.0;

    try
    {
        conductance = std::stod(value, &consumed);
    }
    catch (const std::exception&)
    {
        throw std::runtime_error("OSR_TYRE_CONTACT is a conductance in W/(m2.K) or the word 'perfect', not '" + value +
                                 "'.");
    }

    if (consumed != value.size())
    {
        throw std::runtime_error("OSR_TYRE_CONTACT is a conductance in W/(m2.K) or the word 'perfect', not '" + value +
                                 "'.");
    }

    // Negative is not a conductance, and zero already has a name that says what it means. The upper
    // end is refused because past about here the series term is arithmetic noise against a road path
    // of a few thousand: the one published measurement of this interface bounds it below 6 × 10⁴.
    if (conductance < 0.0)
    {
        throw std::runtime_error("OSR_TYRE_CONTACT cannot be negative: '" + value +
                                 "'. Use 'perfect' for no interface resistance at all.");
    }

    if (conductance > 1.0e6)
    {
        throw std::runtime_error("OSR_TYRE_CONTACT is far past any published figure for this interface: '" + value +
                                 "'. The measured rubber-on-asphalt value is 2.52e4.");
    }

    return conductance;
}

// A fraction of the patch, and there is deliberately no word for "the car's own" — unset already
// says it. One is the gross patch and is what the shipped car states; what is refused is anything
// that is not a fraction of an area, and the low end is refused because a patch nine tenths groove
// is not a tread anybody has published — performance summer treads run about 25-30% void.
[[nodiscard]] std::optional<double> tyreRoadAreaFraction()
{
    const auto value = setting("OSR_TYRE_ROAD_AREA");
    if (value.empty())
    {
        return std::nullopt;
    }

    auto consumed = std::size_t{0};
    auto fraction = 0.0;

    try
    {
        fraction = std::stod(value, &consumed);
    }
    catch (const std::exception&)
    {
        throw std::runtime_error("OSR_TYRE_ROAD_AREA is the fraction of the contact patch that is rubber rather than "
                                 "groove, not '" +
                                 value + "'.");
    }

    if (consumed != value.size())
    {
        throw std::runtime_error("OSR_TYRE_ROAD_AREA is the fraction of the contact patch that is rubber rather than "
                                 "groove, not '" +
                                 value + "'.");
    }

    if (fraction < 0.1 || fraction > 1.0)
    {
        throw std::runtime_error("OSR_TYRE_ROAD_AREA lies between 0.1 and 1.0: '" + value +
                                 "'. This tread's own figure is 0.72, and 1.0 is the gross patch the shipped car "
                                 "states.");
    }

    return fraction;
}

// Degrees Celsius, and there is deliberately no word for "the car's own" — unset already says it.
// What is refused is the range that is not a tread window: a compound whose grip peaks below freezing
// or above the temperature its own curve is falling off at is not a compound anybody has published.
[[nodiscard]] std::optional<double> tyreIdealTemperature()
{
    const auto value = setting("OSR_TYRE_IDEAL");
    if (value.empty())
    {
        return std::nullopt;
    }

    auto consumed = std::size_t{0};
    auto degrees = 0.0;

    try
    {
        degrees = std::stod(value, &consumed);
    }
    catch (const std::exception&)
    {
        throw std::runtime_error("OSR_TYRE_IDEAL is not a number of degrees Celsius: '" + value + "'.");
    }

    if (consumed != value.size())
    {
        throw std::runtime_error("OSR_TYRE_IDEAL is not a number of degrees Celsius: '" + value + "'.");
    }

    if (degrees < 0.0 || degrees > 150.0)
    {
        throw std::runtime_error("OSR_TYRE_IDEAL is where a tread's grip plateau is centred and lies between 0 and "
                                 "150: '" +
                                 value + "'. The shipped car is 85 and the sourced road-tyre band is 45 to 65.");
    }

    return degrees;
}

// Degrees Celsius, parsed on `OSR_SUN`'s terms: there is no "off" for a weather, and what is refused
// is the range that is not a temperature a car is driven in.
[[nodiscard]] std::optional<bool> brakeThermal()
{
    const auto value = setting("OSR_BRAKE_THERMAL");
    if (value.empty())
    {
        return std::nullopt;
    }

    if (value == "on")
    {
        return true;
    }

    if (value == "off")
    {
        return false;
    }

    throw std::runtime_error("OSR_BRAKE_THERMAL is 'on' or 'off', not '" + value +
                             "'. Unset leaves the car's own setting alone.");
}

[[nodiscard]] double airTemperatureCelsius()
{
    const auto value = setting("OSR_AIR_TEMP");
    if (value.empty())
    {
        return 20.0;
    }

    auto consumed = std::size_t{0};
    auto degrees = 0.0;

    try
    {
        degrees = std::stod(value, &consumed);
    }
    catch (const std::exception&)
    {
        throw std::runtime_error("OSR_AIR_TEMP is not a number of degrees Celsius: '" + value + "'.");
    }

    if (consumed != value.size())
    {
        throw std::runtime_error("OSR_AIR_TEMP is not a number of degrees Celsius: '" + value + "'.");
    }

    if (degrees < -30.0 || degrees > 60.0)
    {
        throw std::runtime_error("OSR_AIR_TEMP is an air temperature in Celsius and lies between -30 and 60: '" +
                                 value + "'.");
    }

    return degrees;
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

// The clouds, `OSR_CLOUDS`: a number for how much of the sky they cover, `off` or unset for the
// clear sky. `rainIntensity`'s shape exactly, including why it is not `lookMultiplier`: there is
// no authored cloud for a multiplier to leave untouched, so unset here is zero and `off` is the
// same statement made as a word.
[[nodiscard]] double cloudCoverage()
{
    const auto value = setting("OSR_CLOUDS");
    if (value.empty() || value == "off")
    {
        return 0.0;
    }

    auto consumed = std::size_t{0};
    auto coverage = 0.0;

    try
    {
        coverage = std::stod(value, &consumed);
    }
    catch (const std::exception&)
    {
        throw std::runtime_error("OSR_CLOUDS is neither a number nor 'off': '" + value + "'.");
    }

    if (consumed != value.size())
    {
        throw std::runtime_error("OSR_CLOUDS is neither a number nor 'off': '" + value + "'.");
    }

    if (coverage < 0.0)
    {
        throw std::runtime_error("OSR_CLOUDS cannot be negative: '" + value + "'. 'off' or 0 is the clear sky.");
    }

    return coverage;
}

// The cloud dome map's size, `OSR_CLOUD_MAP=<width>x<height>`; unset is 1024x512, which is exactly
// what the rig used before this knob existed. A size rather than a multiplier because it is a
// texel count, and a knob rather than a constant because it is the one cloud decision that is a
// straight trade of frame rate against detail — the sort this project has always settled from the
// seat and never from a rebuild.
//
// What it buys, measured 2026-08-27: the map spends its texels over 360 degrees of azimuth and 100
// of elevation, so at 1024x512 azimuth gets 2.84 texels per degree against elevation's 5.12 — the
// pixel aspect does not match the angular coverage and azimuth is 1.80 times coarser. Two texels of
// azimuth is 25 m of cloud at 2 km, and the baked volumes resolve 18.7 m, so past about 1.5 km the
// detail is being generated and then thrown away. 2048x512 matches the two axes at 5.69 and 5.12
// and moves that distance to 3 km, for twice the march rather than the four times a full doubling
// costs.
[[nodiscard]] std::pair<int, int> cloudMapSize()
{
    const auto value = setting("OSR_CLOUD_MAP");
    if (value.empty())
    {
        return {1024, 512};
    }

    const auto cross = value.find('x');
    if (cross == std::string::npos || cross == 0 || cross + 1 == value.size())
    {
        throw std::runtime_error("OSR_CLOUD_MAP is not a '<width>x<height>' size: '" + value + "'.");
    }

    auto width = 0;
    auto height = 0;

    try
    {
        auto consumedWidth = std::size_t{0};
        auto consumedHeight = std::size_t{0};
        const auto left = value.substr(0, cross);
        const auto right = value.substr(cross + 1);
        width = std::stoi(left, &consumedWidth);
        height = std::stoi(right, &consumedHeight);

        if (consumedWidth != left.size() || consumedHeight != right.size())
        {
            throw std::runtime_error("trailing characters");
        }
    }
    catch (const std::exception&)
    {
        throw std::runtime_error("OSR_CLOUD_MAP is not a '<width>x<height>' size: '" + value + "'.");
    }

    // The floor is where the map stops resolving a cloud at all and the ceiling is where one frame's
    // march stops being affordable on any card; both are sanity rather than policy, and the seat
    // picks inside them.
    if (width < 256 || height < 128 || width > 8192 || height > 4096)
    {
        throw std::runtime_error("OSR_CLOUD_MAP lies between 256x128 and 8192x4096: '" + value + "'.");
    }

    return {width, height};
}

// The dome's accumulation weight, `OSR_CLOUD_BLEND`: a number in (0, 1], or the word `alpha` for
// the transmittance-as-weight this pass used until 2026-08-27. Unset is 0.5 — see
// RunOptions::cloudBlendWeight for the measurements that chose it, and note that `alpha` is kept
// as the control rather than as a fallback: it is how the shipped-before look is reproduced.
//
// Zero is refused rather than clamped because it is not a look, it is a map that can never change:
// weight zero keeps the destination and discards every march.
[[nodiscard]] std::optional<double> cloudBlendWeight()
{
    const auto value = setting("OSR_CLOUD_BLEND");
    if (value.empty())
    {
        return 0.5;
    }

    if (value == "alpha")
    {
        return std::nullopt;
    }

    auto consumed = std::size_t{0};
    auto weight = 0.0;

    try
    {
        weight = std::stod(value, &consumed);
    }
    catch (const std::exception&)
    {
        throw std::runtime_error("OSR_CLOUD_BLEND is neither a weight nor 'alpha': '" + value + "'.");
    }

    if (consumed != value.size())
    {
        throw std::runtime_error("OSR_CLOUD_BLEND is neither a weight nor 'alpha': '" + value + "'.");
    }

    if (weight <= 0.0 || weight > 1.0)
    {
        throw std::runtime_error("OSR_CLOUD_BLEND lies in (0, 1]: '" + value +
                                 "'. One is a plain overwrite; zero would freeze the map.");
    }

    return weight;
}

// How many frames apart the dome marches, `OSR_CLOUD_EVERY`; unset is 1, which is every frame.
//
// The ceiling is where the amortisation stops being worth anything: the map's own texel is 0.35
// degrees of azimuth, and past about sixty frames a car at speed has translated far enough to move
// a near cloud across several of them — which is stepping, and stepping is the failure mode this
// whole knob is being judged for. A floor of one is what "every frame" means.
[[nodiscard]] int cloudMarchInterval()
{
    const auto value = setting("OSR_CLOUD_EVERY");
    if (value.empty())
    {
        return 4;
    }

    auto consumed = std::size_t{0};
    auto frames = 0;

    try
    {
        frames = std::stoi(value, &consumed);
    }
    catch (const std::exception&)
    {
        throw std::runtime_error("OSR_CLOUD_EVERY is not a number of frames: '" + value + "'.");
    }

    if (consumed != value.size())
    {
        throw std::runtime_error("OSR_CLOUD_EVERY is not a number of frames: '" + value + "'.");
    }

    if (frames < 1 || frames > 60)
    {
        throw std::runtime_error("OSR_CLOUD_EVERY lies between 1 and 60 frames: '" + value + "'.");
    }

    return frames;
}

// How many vertical strips one refresh of the map is spread over, `OSR_CLOUD_STRIPS`; unset is 1,
// the whole map in one pass. The ceiling is where a strip stops being worth splitting: a strip of a
// sixteenth already costs 1.25 ms against a whole march's 4.39, so the per-pass floor is most of it
// and thinner strips buy almost nothing.
[[nodiscard]] int cloudMarchStrips()
{
    const auto value = setting("OSR_CLOUD_STRIPS");
    if (value.empty())
    {
        return 2;
    }

    auto consumed = std::size_t{0};
    auto strips = 0;

    try
    {
        strips = std::stoi(value, &consumed);
    }
    catch (const std::exception&)
    {
        throw std::runtime_error("OSR_CLOUD_STRIPS is not a number of strips: '" + value + "'.");
    }

    if (consumed != value.size())
    {
        throw std::runtime_error("OSR_CLOUD_STRIPS is not a number of strips: '" + value + "'.");
    }

    if (strips < 1 || strips > 32)
    {
        throw std::runtime_error("OSR_CLOUD_STRIPS lies between 1 and 32: '" + value + "'.");
    }

    return strips;
}

// Degrees rather than a multiplier, so this one is parsed on its own terms: there is no "off" for a
// time of day, and the range refused is the one that is not an elevation at all.
[[nodiscard]] double sunElevationDegrees()
{
    const auto value = setting("OSR_SUN");
    if (value.empty())
    {
        return 19.0;
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
            throw std::runtime_error(std::string(name) + " takes " + form + ", and this states more: '" + value + "'.");
        }

        auto consumed = std::size_t{0};

        try
        {
            parsed.at(found) = std::stod(word, &consumed);
        }
        catch (const std::exception&)
        {
            throw std::runtime_error(std::string(name) + " takes " + form + ", and '" + word + "' is not a number: '" +
                                     value + "'.");
        }

        if (consumed != word.size())
        {
            throw std::runtime_error(std::string(name) + " takes " + form + ", and '" + word + "' is not a number: '" +
                                     value + "'.");
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
    const auto [cloudMapWidth, cloudMapHeight] = cloudMapSize();

    return RunOptions{.scene = chosen,
                      .track = track(chosen),
                      .camera = chosenCamera,
                      .driver = driver(chosen),
                      .rackTrace = rackTrace(chosen),
                      .beltBridgingMillimetres = beltBridgingMillimetres(),
                      .geometricLoadPath = geometricLoadPath(),
                      .drivelineReaction = drivelineReaction(),
                      .worldColliders = worldColliders(),
                      .occlusionCulling = occlusionCulling(),
                      .tyreTemperature = tyreTemperature(),
                      .tyreThermal = tyreThermal(),
                      .tyrePressure = tyrePressure(),
                      .tyreContactConductance = tyreContactConductance(),
                      .tyreRoadAreaFraction = tyreRoadAreaFraction(),
                      .tyreIdealTemperature = tyreIdealTemperature(),
                      .brakeThermal = brakeThermal(),
                      .airTemperatureCelsius = airTemperatureCelsius(),
                      .assists = chosenAssists,
                      .fogDensityScale = lookMultiplier("OSR_FOG"),
                      .rainIntensity = rainIntensity(),
                      .cloudCoverage = cloudCoverage(),
                      .cloudMapWidth = cloudMapWidth,
                      .cloudMapHeight = cloudMapHeight,
                      .cloudBlendWeight = cloudBlendWeight(),
                      .cloudMarchInterval = cloudMarchInterval(),
                      .cloudMarchStrips = cloudMarchStrips(),
                      .sunElevationDegrees = sunElevationDegrees(),
                      .cameraPose = cameraPose(chosenCamera)};
}

} // namespace osr
