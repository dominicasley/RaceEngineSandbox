module;

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <ctime>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

export module osr.game:InstrumentCluster;

import :Font;
import :SimulatedCar;

import raceengine;

namespace osr
{

// The instrument cluster the driver reads, driven from the tick's own state: six needles and the
// two rings of lit segments the Golf's model carries. `docs/instrument-cluster-brief.md`.
//
// **What the asset is.** Assetto Corsa tags a needle as a dummy node named `ARROW_*` standing at the
// pivot, with the needle's mesh under it; the four cluster needles (rpm, speed, fuel, water) are
// authored about their own origin and the two on the sport display are not — their mesh frame's
// origin is half a metre from the pivot, so a needle here is a rotation about a *stated* pivot in
// the mesh's own frame. Beside the needles the cluster is a **digital** one: a hundred rpm segments
// and a hundred speed segments on the `cluster_ridges_ON` material, each sitting a fraction of a
// millimetre proud of an always-drawn unlit ridge, and AC shows segment N once the reading reaches
// the N-th step of the series. Nothing in the engine hid a mesh until now, so every segment was lit
// and both dials read full scale from the day the model was loaded.
//
// **Measured, not assumed** (`scripts/measure-instrument-cluster.py`, 2026-09-14): the spin axis of
// every needle is the smallest-variance axis of its own vertices; the pivot is the `ARROW_*` dummy's
// origin carried into the mesh frame; and the reading-to-angle law of the rpm and speed needles is
// read off the rings themselves — the angle of segment N about the pivot against the value AC lights
// it at — because the mod's `analog_instruments.ini` states those two needles in CSP's auto-indexed
// `[ANALOG_INDICATOR_...]` sections and the exporter's parser kept only the last of them (the water
// temperature). Fuel, water and boost keep the file's own `ZERO` / `STEP`. AC's convention, held
// here: a positive angle is clockwise as the driver sees it, so a negative `STEP` (fuel, water — the
// small arcs at the bottom of each dial, pivoted at the dial's centre) sweeps left to right.
//
// **The segments are lit by angle, never by name.** The dummies are numbered with gaps and the mesh
// under `LED_50` is named `LED_51`; what a segment *is* is where it stands, so at construction each
// `cluster_ridges_ON` mesh is placed on the dial whose pivot it rings and given its angle from the
// needle's authored direction, and a segment shows exactly when the needle has swept past it. The
// arc and the needle then agree by construction, whatever the file says.
export class InstrumentCluster
{
public:
    InstrumentCluster(raceengine::Engine& engine, raceengine::RenderableModel& carRenderable);

    // Every needle and every segment from one instant of the car, once per engine tick.
    void update(const CarSnapshot& snapshot, double dtSeconds);

    // The displays (docs/instrument-cluster-brief.md §6): the scene's screen canvas this writes the
    // readouts into every tick, the font it sets them in, and the air the ambient readout shows.
    // `overlay`, when there is one, gets the same quads over the frame — `OSR_DASH_OVERLAY`, the
    // way to see the canvas on the monitor without a dashboard in the way.
    void attachScreen(raceengine::Canvas& canvas, raceengine::Canvas* overlay, const Font& font,
                      double ambientCelsius, bool metric);

    // **The screen canvas, measured** (scripts/measure-instrument-cluster.py, the TEXT ANCHORS
    // section): the two display materials' UV islands are upright and axis-aligned, one unit of u
    // spanning 0.3145 m of dashboard and one of v 0.177 m, so a 16:9 region of the canvas per
    // material gives square texels at 3256 per metre — 1024 x 576 each, the dial displays'
    // `cluster_digi` on the top half and the middle display's `cluster_digi_nav` on the bottom.
    static constexpr float canvasWidth = 1024.0f;
    static constexpr float canvasHeight = 1152.0f;
    static constexpr float regionHeight = 576.0f;
    static constexpr float pixelsPerMetre = 3256.0f;
    // Where each `DISPLAY_DATA*` anchor lands on its island, u and v (v wrapped out of the second
    // tile the islands were authored in), as canvas pixels: the gear readout's anchor on the tacho's
    // display, the speed's on the speedo's, and the middle display's, which two anchors share.
    static constexpr glm::vec2 gearAnchor{0.2199f * canvasWidth, 0.4898f * regionHeight};
    static constexpr glm::vec2 speedAnchor{0.7786f * canvasWidth, 0.4918f * regionHeight};
    static constexpr glm::vec2 middleAnchor{0.4970f * canvasWidth, regionHeight + 0.4917f * regionHeight};
    static constexpr glm::vec2 middleTextAnchor{0.4970f * canvasWidth, regionHeight + 0.4898f * regionHeight};
    // The rects the two screen materials map their UVs through (Material::screen).
    static constexpr glm::vec4 dialScreenRect{0.0f, 0.0f, 1.0f, 0.5f};
    static constexpr glm::vec4 middleScreenRect{0.0f, 0.5f, 1.0f, 0.5f};

    // PLACED: the readouts' colour (a cool white, the file's `1, 1, 1.7` read as a hue) and where a
    // run sits against the position stated for it — every position here is a run's centre, and a
    // capital's centre is 0.35 em above the baseline in this face. The odometer starts where a
    // three-year-old car might; nothing in the physics carries one.
    static constexpr glm::vec4 readoutColour{0.85f, 0.92f, 1.0f, 1.0f};
    static constexpr float baselineFromCentreEms = 0.35f;
    static constexpr double odometerStartMetres = 50331000.0;
    // PLACED: the gear's and the speed's size in the dial displays, metres per em. 22 mm filled the
    // windows edge to edge; 19 mm is the seat's "reduce the font size slightly".
    static constexpr float dialDigitSizeMetres = 0.019f;
    // Where the runs sit, metres from their anchor (x to the driver's left, y up — the dummies' own
    // axes). The file's `digital_instruments.ini` put the dial digits 13.5 mm below the dial's centre
    // and the middle display's top row at 36.8 mm, which on this model's display meshes read low in
    // the dials and against the middle display's top edge (Dominic, from the seat: "the y position
    // of the text is off"); AC draws its text as quads in the air, so its file never had to fit a
    // mesh. So: the dial digits centred in the dial displays' windows; the middle display's speed
    // centred; its top row 33 mm up, three millimetres inside the display's top edge (the island
    // ends 37.7 mm above the anchor); its bottom row 17.5 mm down, a row above the bottom edge
    // (25.5 mm) where the face prints "trip" and "mi", inset 34 mm each side to clear the dial rings
    // that overlap the display's edges. All MEASURED against `cluster_digi_nav`'s island; PLACED
    // within it.
    static constexpr glm::vec2 dialDigitsMetres{0.0f, 0.0f};
    // MEASURED and kept, written by nothing: the file draws the speed a second time in the middle
    // display and that readout is not written here — one speedometer is enough and the dial's is the
    // one the driver reads (Dominic, from the seat: "remove the centre speed display").
    static constexpr glm::vec2 middleSpeedMetres{0.0f, 0.0f};
    static constexpr float middleTopRowMetres = 0.033f;
    static constexpr float middleBottomRowMetres = -0.0175f;
    static constexpr float middleBottomRowInsetMetres = 0.034f;
    static constexpr float middleAirInsetMetres = 0.052f;

    // PLACED. The one response the display has: a first-order lag on the rpm and speed readings
    // before the needle and the ring read them, so both agree and neither flicks at the frame rate
    // when the engine speed bounces off a shift. The digital cluster in the real car has a display's
    // own lag and no more; zero is the way to switch it off.
    static constexpr double needleLagSeconds = 0.05;

    // The readings nothing in the physics carries yet, held where a car would show them. Fuel is
    // `car.ini [FUEL] FUEL`, the mod's own starting tank (the tank is 55 l); water is the temperature
    // a Golf's gauge sits at once warm — the real one is damped to read 90 across the whole normal
    // range; boost is zero because the driveline has no manifold. Each is a seam a model plugs into.
    static constexpr double fuelLitres = 45.0;
    static constexpr double waterCelsius = 90.0;
    static constexpr double boostBar = 0.0;

    // The rings, measured. Segment N of the rpm ring stands at `rpmRingZeroDegrees + 2.4966 × N` for
    // N in 0..102 over 0..8000 rpm (fit rms 0.34°, the residual is the ring's own spacing jitter); the
    // speed ring is two AC series, 0..187 km/h over 75 segments and 193..310 over 25, and its two fits
    // meet within a segment at the seam. Speed is km/h because AC's series are, whatever the face
    // says (this one is mph, and the ring's two rates are the face's own scale change at 120 mph).
    static constexpr double rpmRingZeroDegrees = 1.016;
    static constexpr double rpmRingDegreesPerRpm = 2.4966 * 102.0 / 8000.0;
    static constexpr double rpmRingMaxRpm = 8000.0;
    static constexpr std::array<std::array<double, 2>, 4> speedRingKmhToDegrees{
        {{0.0, 3.11}, {187.0, 187.94}, {193.0, 190.57}, {310.0, 250.73}}};

    // The file's own laws for the three needles it still states (degrees per unit, from `ZERO` 0).
    static constexpr double fuelDegreesPerLitre = -0.8909091;
    static constexpr double waterDegreesPerCelsius = -0.49;
    static constexpr double waterZeroCelsius = 50.0;
    static constexpr double waterMaxCelsius = 150.0;
    static constexpr double boostDegreesPerBar = 85.0;

    [[nodiscard]] static double rpmDegrees(const double rpm)
    {
        return rpmRingZeroDegrees + rpmRingDegreesPerRpm * std::clamp(rpm, 0.0, rpmRingMaxRpm);
    }

    [[nodiscard]] static double speedDegrees(const double kmh)
    {
        const auto& table = speedRingKmhToDegrees;
        if (kmh <= table.front()[0])
        {
            return table.front()[1];
        }

        for (auto index = std::size_t{1}; index < table.size(); index++)
        {
            if (kmh <= table[index][0])
            {
                const auto& low = table[index - 1];
                const auto& high = table[index];
                const auto fraction = (kmh - low[0]) / (high[0] - low[0]);

                return low[1] + fraction * (high[1] - low[1]);
            }
        }

        return table.back()[1];
    }

    [[nodiscard]] static double fuelDegrees(const double litres)
    {
        return fuelDegreesPerLitre * std::max(litres, 0.0);
    }

    [[nodiscard]] static double waterDegrees(const double celsius)
    {
        return waterDegreesPerCelsius * (std::clamp(celsius, waterZeroCelsius, waterMaxCelsius) - waterZeroCelsius);
    }

    [[nodiscard]] static double boostDegrees(const double bar)
    {
        return boostDegreesPerBar * std::max(bar, 0.0);
    }

    [[nodiscard]] std::size_t rpmSegmentCount() const
    {
        return rpmSegments.size();
    }

    [[nodiscard]] std::size_t speedSegmentCount() const
    {
        return speedSegments.size();
    }

private:
    struct Needle
    {
        // The mesh's own name in the file — the node under the `ARROW_*` dummy, whose `.001` copy
        // is the mesh the loader keeps under the plain name.
        std::string_view meshName;
        // Unit vector in the mesh's own frame; a positive angle about it is clockwise for the driver.
        glm::vec3 axis;
        // Where the needle turns about, in the mesh's own frame, metres.
        glm::vec3 pivot;
        std::optional<std::size_t> mesh{};
    };

    struct Segment
    {
        std::size_t mesh;
        // Clockwise from the needle's authored direction, 0..360.
        double angleDegrees;
    };

    // The four cluster needles: the mesh frame's −Z, 0.14° off, and the pivot at the origin. The
    // sport display's two: an axis in the mesh frame's general direction (the frame is not the
    // pivot's) and the pivot half a metre away.
    static constexpr glm::vec3 clusterAxis{0.0f, -0.00247f, -1.0f};
    static constexpr glm::vec3 clusterPivot{0.0f};
    static constexpr glm::vec3 sportAxis{0.14236f, 0.90738f, -0.39546f};
    static constexpr glm::vec3 sportPivot{-0.03721f, 0.51572f, 0.68915f};

    // A segment belongs to the dial whose pivot it rings: both rings are 27.7 mm out and the two
    // pivots are 176 mm apart.
    static constexpr double ringReachMetres = 0.06;

    static constexpr std::string_view litSegmentMaterial = "cluster_ridges_ON";

    enum Reading : std::size_t
    {
        Rpm,
        Speed,
        Fuel,
        Water,
        Boost,
        WaterSport,
        ReadingCount
    };

    raceengine::Engine& engine;
    raceengine::RenderableModel& carRenderable;

    std::array<Needle, ReadingCount> needles{{
        {"geo_arrow_rpm", clusterAxis, clusterPivot, std::nullopt},
        {"geo_arrow_rpm001", clusterAxis, clusterPivot, std::nullopt},
        {"geo_arrow_fuel", clusterAxis, clusterPivot, std::nullopt},
        {"geo_arrow_watertemp", clusterAxis, clusterPivot, std::nullopt},
        {"sport_display_arrow_geo", sportAxis, sportPivot, std::nullopt},
        {"sport_display_arrow_geo001", sportAxis, sportPivot, std::nullopt},
    }};

    std::vector<Segment> rpmSegments;
    std::vector<Segment> speedSegments;

    double shownRpm = 0.0;
    double shownKmh = 0.0;
    bool seeded = false;

    raceengine::Canvas* screen = nullptr;
    raceengine::Canvas* screenOverlay = nullptr;
    const Font* font = nullptr;
    double ambientCelsius = 20.0;
    // Metric readouts (`OSR_UNITS`): km/h and km; otherwise mph and miles, which is what the dial
    // face prints. The ring and the needle work in km/h either way; only the digits change.
    bool metricUnits = true;
    double odometerMetres = odometerStartMetres;
    double tripMetres = 0.0;

    void writeReadouts(const CarSnapshot& snapshot);

    // The dial's frame in model space, from its needle: where it turns, the clockwise axis, and the
    // needle's authored direction as angle zero.
    struct DialFrame
    {
        glm::dvec3 pivot;
        glm::dvec3 clockwise;
        glm::dvec3 zero;
        glm::dvec3 ninety;
    };

    [[nodiscard]] std::optional<DialFrame> dialFrame(const Needle& needle) const;

    void turn(const Needle& needle, double degrees) const;
    void light(const std::vector<Segment>& segments, double needleDegrees) const;
};

} // namespace osr

namespace osr
{

InstrumentCluster::InstrumentCluster(raceengine::Engine& engine, raceengine::RenderableModel& carRenderable) :
    engine(engine),
    carRenderable(carRenderable)
{
    struct Candidate
    {
        std::size_t mesh;
        glm::dvec3 centre;
    };
    std::vector<Candidate> candidates;

    auto& storage = engine.memoryStorage();
    for (auto index = std::size_t{0}; index < carRenderable.meshes.size(); index++)
    {
        const auto* mesh = storage.meshes.find(carRenderable.meshes[index].mesh);
        if (mesh == nullptr)
        {
            continue;
        }

        for (auto& needle : needles)
        {
            if (!needle.mesh && mesh->name == needle.meshName)
            {
                needle.mesh = index;
            }
        }

        for (const auto& primitive : mesh->meshPrimitives)
        {
            if (!primitive.material)
            {
                continue;
            }

            const auto* material = storage.materials.find(*primitive.material);
            if (material == nullptr || material->name != litSegmentMaterial)
            {
                continue;
            }

            const auto centre = mesh->modelMatrix * glm::vec4(primitive.boundsCentre, 1.0f);
            candidates.push_back({index, glm::dvec3(centre)});
            break;
        }
    }

    // Each lit segment onto the ring it stands on, at its angle from the needle's rest.
    const auto place = [&](const Needle& needle, std::vector<Segment>& segments) {
        const auto frame = dialFrame(needle);
        if (!frame)
        {
            return;
        }

        for (const auto& candidate : candidates)
        {
            const auto offset = candidate.centre - frame->pivot;
            const auto inPlane = offset - glm::dot(offset, frame->clockwise) * frame->clockwise;
            if (glm::length(inPlane) > ringReachMetres)
            {
                continue;
            }

            auto degrees =
                glm::degrees(std::atan2(glm::dot(inPlane, frame->ninety), glm::dot(inPlane, frame->zero)));
            if (degrees < 0.0)
            {
                degrees += 360.0;
            }

            segments.push_back({candidate.mesh, degrees});
        }
    };
    place(needles[Rpm], rpmSegments);
    place(needles[Speed], speedSegments);

    std::string missing;
    for (const auto& needle : needles)
    {
        if (!needle.mesh)
        {
            missing += missing.empty() ? " (no " : ", ";
            missing += needle.meshName;
        }
    }
    if (!missing.empty())
    {
        missing += ")";
    }

    engine.log().info("Instrument cluster: {} of {} needles{}, {} rpm segments and {} speed segments of {} lit meshes",
                      std::count_if(needles.begin(), needles.end(), [](const Needle& n) { return n.mesh.has_value(); }),
                      needles.size(), missing, rpmSegments.size(), speedSegments.size(), candidates.size());
}

std::optional<InstrumentCluster::DialFrame> InstrumentCluster::dialFrame(const Needle& needle) const
{
    if (!needle.mesh)
    {
        return std::nullopt;
    }

    const auto* mesh = engine.memoryStorage().meshes.find(carRenderable.meshes[*needle.mesh].mesh);
    if (mesh == nullptr || mesh->meshPrimitives.empty())
    {
        return std::nullopt;
    }

    const auto& toModel = mesh->modelMatrix;
    const auto pivot = glm::dvec3(toModel * glm::vec4(needle.pivot, 1.0f));
    const auto clockwise = glm::normalize(glm::dvec3(glm::mat3(toModel) * needle.axis));

    // The needle's own centre, out along the blade from the pivot, is the direction it was authored
    // pointing — its reading of zero.
    const auto centre = glm::dvec3(toModel * glm::vec4(mesh->meshPrimitives.front().boundsCentre, 1.0f));
    const auto reach = centre - pivot;
    const auto zero = glm::normalize(reach - glm::dot(reach, clockwise) * clockwise);

    return DialFrame{.pivot = pivot, .clockwise = clockwise, .zero = zero, .ninety = glm::cross(clockwise, zero)};
}

void InstrumentCluster::update(const CarSnapshot& snapshot, const double dtSeconds)
{
    const auto rpm = snapshot.audio.engineRpm;
    const auto kmh = glm::length(snapshot.state.chassis.linearVelocity) * 3.6;

    if (!seeded)
    {
        shownRpm = rpm;
        shownKmh = kmh;
        seeded = true;
    }

    const auto blend = needleLagSeconds > 0.0 ? 1.0 - std::exp(-dtSeconds / needleLagSeconds) : 1.0;
    shownRpm += (rpm - shownRpm) * blend;
    shownKmh += (kmh - shownKmh) * blend;

    const auto rpmAngle = rpmDegrees(shownRpm);
    const auto speedAngle = speedDegrees(shownKmh);

    turn(needles[Rpm], rpmAngle);
    turn(needles[Speed], speedAngle);
    turn(needles[Fuel], fuelDegrees(fuelLitres));
    turn(needles[Water], waterDegrees(waterCelsius));
    turn(needles[Boost], boostDegrees(boostBar));
    // The sport display's temperature needle has no stated scale (its section was one of those the
    // parser dropped), so it stays as authored until one is read off the pack.

    light(rpmSegments, rpmAngle);
    light(speedSegments, speedAngle);

    odometerMetres += kmh / 3.6 * dtSeconds;
    tripMetres += kmh / 3.6 * dtSeconds;
    writeReadouts(snapshot);
}

void InstrumentCluster::attachScreen(raceengine::Canvas& canvas, raceengine::Canvas* overlay, const Font& typeface,
                                     const double ambient, const bool metric)
{
    screen = &canvas;
    screenOverlay = overlay;
    font = &typeface;
    ambientCelsius = ambient;
    metricUnits = metric;
}

// The readouts, as `digital_instruments.ini` places them (docs/instrument-cluster-brief.md §6.1):
// each item a run at its anchor plus the file's offset in metres, at the file's size in metres per
// em, aligned as the file says. **The file's x is the driver's left**, which is the dummy's own +x
// in the model and what the face's printed labels say: the trip's value (+0.046, left-aligned)
// belongs beside the "trip" printed at the display's left, the odometer's (−0.044, right-aligned)
// beside the "mi" at its right, and the air's (−0.052) beside the "°C" at the top right — so +x is
// a step to the left on the canvas, where u grows to the right. The speed once, in the speedo's
// display — the file draws it in the middle display as well and that one is not written; the gear
// in the tacho's; the clock, the air and the two odometers on the middle display's rows.
void InstrumentCluster::writeReadouts(const CarSnapshot& snapshot)
{
    if (screen == nullptr || font == nullptr)
    {
        return;
    }

    auto text = font->batch();
    const auto place = [&](const std::string& run, const glm::vec2 anchor, const glm::vec2 offsetMetres,
                           const float sizeMetres, const TextAlign align)
    {
        const auto size = sizeMetres * pixelsPerMetre;
        const auto centre = anchor + glm::vec2(-offsetMetres.x, -offsetMetres.y) * pixelsPerMetre;
        font->layout(run, centre + glm::vec2(0.0f, baselineFromCentreEms * size), size, align, readoutColour,
                     text.quads);
    };

    const auto gear = snapshot.gear > 0   ? std::to_string(snapshot.gear)
                      : snapshot.gear < 0 ? std::string("R")
                                          : std::string("N");
    const auto speed = std::to_string(static_cast<int>(std::lround(metricUnits ? shownKmh : shownKmh / 1.609344)));
    const auto distanceUnit = metricUnits ? 1000.0 : 1609.344;

    std::time_t now = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    std::array<char, 16> clock{};
    std::snprintf(clock.data(), clock.size(), "%02d:%02d", local.tm_hour, local.tm_min);
    std::array<char, 16> air{};
    std::snprintf(air.data(), air.size(), "%.0f\u00B0C", ambientCelsius);
    std::array<char, 24> odometer{};
    std::snprintf(odometer.data(), odometer.size(), metricUnits ? "%.0f km" : "%.0f mi", odometerMetres / distanceUnit);
    std::array<char, 24> trip{};
    std::snprintf(trip.data(), trip.size(), "%.1f", tripMetres / distanceUnit);

    place(gear, gearAnchor, dialDigitsMetres, dialDigitSizeMetres, TextAlign::Centre);
    place(speed, speedAnchor, dialDigitsMetres, dialDigitSizeMetres, TextAlign::Centre);
    place(air.data(), middleTextAnchor, glm::vec2(-middleAirInsetMetres, middleTopRowMetres), 0.0078f,
          TextAlign::Right);
    place(clock.data(), middleTextAnchor, glm::vec2(0.0f, middleTopRowMetres), 0.0085f, TextAlign::Centre);
    place(odometer.data(), middleTextAnchor, glm::vec2(-middleBottomRowInsetMetres, middleBottomRowMetres), 0.0072f,
          TextAlign::Right);
    place(trip.data(), middleTextAnchor, glm::vec2(middleBottomRowInsetMetres, middleBottomRowMetres), 0.0072f,
          TextAlign::Left);

    screen->batches.clear();
    screen->batches.push_back(text);
    if (screenOverlay != nullptr)
    {
        screenOverlay->batches.clear();
        screenOverlay->batches.push_back(std::move(text));
    }
}

void InstrumentCluster::turn(const Needle& needle, const double degrees) const
{
    if (!needle.mesh)
    {
        return;
    }

    const auto rotation = glm::rotate(glm::mat4(1.0f), glm::radians(static_cast<float>(degrees)), needle.axis);
    carRenderable.meshes[*needle.mesh].localTransform =
        glm::translate(glm::mat4(1.0f), needle.pivot) * rotation * glm::translate(glm::mat4(1.0f), -needle.pivot);
}

void InstrumentCluster::light(const std::vector<Segment>& segments, const double needleDegrees) const
{
    for (const auto& segment : segments)
    {
        carRenderable.meshes[segment.mesh].visible = segment.angleDegrees <= needleDegrees;
    }
}

} // namespace osr
