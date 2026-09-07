module;

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <charconv>
#include <expected>
#include <fstream>
#include <ios>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <glm/glm.hpp>

export module osr.game:TrafficNetwork;

// The JSON reader this file used to carry in its own anonymous namespace, moved out on 2026-09-06
// when the collider manifest became a second document that needed one. Nothing about it changed in
// the move.
import :Json;

namespace osr
{

// A track's traffic network, as `~/dev/ac-car-data` exports it out of Assetto Corsa's CSP traffic
// data: a lane graph, in metres, in the same right-handed axes glTF uses, so nothing here needs a
// remap.
//
// **This file is deliberately wider than its one consumer today.** What reads it now is the light
// probe placement in `CircuitScene` — a city's indirect light is a street canyon's bounce, and the
// lanes are the only statement this project has of where the streets *are*. What reads it next is
// traffic, which needs every other field: the roles and their speed limits, the per-lane CSP
// switches, the junction links and the dead ends. So the whole document is carried rather than the
// point lists alone, and the geometry helpers below are written for a car following a lane rather
// than for a probe standing beside one.
//
// Two properties of the export are load-bearing and are the reason the probe placement can trust it:
//
//  - **Lane points are at road level.** The exporter samples them against the collision mesh and
//    records a median 0.001 m above it. This is the opposite of an AC AI line, which floats at the
//    recording car's ride height — Bathurst's `fast_lane.ai` sits a median 0.79 m of thin air above
//    the tarmac. So `y` here is the road, and a probe's height above it is a height above the road.
//  - **Points are sparse control points, not a sampled line.** Two of them can span 110 m, and the
//    document does not say how CSP interpolates between them. Everything here therefore treats a
//    lane as a chord polyline, which is what the exporter's own `length_m` measures.
//
// Parsing is pure and answers with `std::expected`, and the reader is written here rather than
// taken from a library for the reason `SetupFile` parses its own sheet by hand: the sandbox names
// no third-party headers, and a header-only parser in a global module fragment is paid for by every
// unit that imports this one. Nothing in this file imports `raceengine`, which is what keeps
// `<fstream>` out of a unit whose global module fragment would then be merged against every
// imported BMI — the `CloudNoise.cppm` and `Options.cppm` pattern.

// One entry of the export's `roles` table: what a class of road is called, how fast traffic runs on
// it and which class wins at a junction. Carried whole because traffic wants all four; the probe
// placement wants none of them.
export struct TrafficRole
{
    std::string name;
    double speedLimitKmh = 0.0;
    double speedLimitMetresPerSecond = 0.0;
    // CSP's own ordering, and it is signed: parking is -8 and a highway is +4.
    int priority = 0;
};

// The per-lane switches CSP carries, verbatim. Nothing reads them yet and traffic reads all of
// them: `maxcars` is a density, the two `allow` flags are what a driver model may do, and `taxi`
// marks a lane that exists to be pulled over on.
export struct TrafficLaneParams
{
    bool taxi = false;
    bool allowUTurns = false;
    bool allowLaneChanges = false;
    int maximumCars = 0;
};

// One lane: a named polyline with a role, a speed limit and a set of switches.
export struct TrafficLane
{
    int id = 0;
    std::string name;
    // The key into `TrafficNetwork::roles`. A name rather than an index because that is what the
    // document states, and a lane naming a role the table does not carry is a fault worth seeing
    // rather than an index silently in range.
    std::string role;
    // The exporter's own figure, which is a sum of chord lengths for the reason above.
    double lengthMetres = 0.0;
    // Stated by the export and false on every lane of Grand City Parkway. The closing segment from
    // the last point back to the first is **not** synthesised by anything here, because no lane in
    // the only export this project carries sets the flag and guessing at one would put a segment
    // through a building. A track that has one is where that decision gets made.
    bool loop = false;
    int priorityOffset = 0;
    double speedLimitKmh = 0.0;
    TrafficLaneParams params;
    // The export's own elevation band for this lane, metres. Grand City Parkway's lanes all sit
    // within 7 cm of zero, which is what makes a flat probe height a reasonable one there and is
    // exactly the assumption a track with a hill would break.
    double elevationMinimumMetres = 0.0;
    double elevationMaximumMetres = 0.0;
    // The control points, metres, in the export's axes.
    std::vector<glm::dvec3> points;
    // Chord distance from the lane's first point to each point, one entry per point and the first
    // always zero. Built at load because every consumer — a resample, a probe, a car placed at a
    // distance along a lane — needs it and none of them should walk the polyline to get it.
    std::vector<double> arcLengthMetres;
};

// One derived link between two lanes. CSP stores no lane graph at all, so the exporter derives
// these from geometry: a *merge* is a lane ending on top of another, a *branch* is one starting on
// top of another, and `distanceMetres` is the evidence for the claim. Traffic needs them to route;
// nothing reads them today.
export struct TrafficJunctionLink
{
    // "merge" or "branch", as the exporter writes it.
    std::string kind;
    int laneId = 0;
    int targetLaneId = 0;
    double distanceMetres = 0.0;
    // Which segment of the target lane the join lands on, and where along the whole target lane it
    // falls as a fraction.
    int targetSegment = 0;
    double positionAlongTarget = 0.0;
    // The dot product of the two headings at the join: 1.0 is the two lanes running the same way.
    double headingAgreement = 0.0;
};

// The counts block, which is the export's own summary of itself. Kept so that a loader can say "the
// document claims twelve lanes and carries eleven" rather than quietly driving on eleven.
export struct TrafficCounts
{
    int lanes = 0;
    int intersections = 0;
    int areas = 0;
    int parking = 0;
    int trafficLights = 0;
    double totalLaneLengthMetres = 0.0;
};

// The whole document.
export struct TrafficNetwork
{
    // Provenance, carried because it is the only thing that says which AC track this came out of.
    std::string source;
    std::string units;
    std::string axes;

    TrafficCounts counts;
    std::vector<TrafficRole> roles;
    std::vector<TrafficLane> lanes;

    // The derived graph. `junctionToleranceMetres` is the radius the exporter called a join within.
    double junctionToleranceMetres = 0.0;
    std::vector<TrafficJunctionLink> junctionLinks;
    // Lanes whose last point joins nothing. Traffic reaching one has nowhere to go and has to be
    // despawned or turned round — and on Grand City Parkway that is **every** lane, which is a
    // property of the export that traffic will have to answer for rather than a fault here.
    std::vector<int> deadEndLaneIds;
};

// The role a lane names, or nothing where the table does not carry it.
export [[nodiscard]] const TrafficRole* trafficRole(const TrafficNetwork& network, std::string_view name);

// The lane with this id, or nothing. Ids are the export's own and are not contiguous — Grand City
// Parkway numbers twelve lanes 1..13 with 11 missing — so this is a search and not an index.
export [[nodiscard]] const TrafficLane* trafficLane(const TrafficNetwork& network, int id);

// The lane's length as its own points measure it, which is what every helper here works in. The
// export's `lengthMetres` is the same quantity computed by the exporter and the two are a
// cross-check on each other rather than one being a fallback for the other.
export [[nodiscard]] double laneChordLengthMetres(const TrafficLane& lane);

// A place on a lane: where it is, which way the lane runs there, and how far along it that is.
// This is the shape a traffic car wants — a position alone cannot orient one.
export struct LanePoint
{
    glm::dvec3 positionMetres{0.0};
    // Unit tangent along the lane's direction of travel. The segment's own direction, not a
    // smoothed one: the points are sparse, so a smoothed tangent would be inventing a curve the
    // document does not state.
    glm::dvec3 direction{0.0, 0.0, 1.0};
    double distanceMetres = 0.0;
    // Which segment of the polyline this fell in, so a caller stepping along a lane can carry on
    // from here rather than searching again.
    std::size_t segment = 0;
};

// Where a lane is `distanceMetres` from its start, interpolated along the chord. Clamped to the
// lane's two ends; nothing here wraps, for the reason `loop` states above. Nothing where the lane
// carries fewer than two points.
export [[nodiscard]] std::optional<LanePoint> laneSampleAt(const TrafficLane& lane, double distanceMetres);

// The lane walked end to end at a fixed interval, always including its first point and never
// running past its last. A spacing at or below zero is refused with an empty answer rather than
// looping forever.
export [[nodiscard]] std::vector<LanePoint> laneResample(const TrafficLane& lane, double spacingMetres);

// How the lane network is turned into places to photograph the world from.
export struct LaneProbeOptions
{
    // How far apart along a lane, metres. Sixty is about a city block on this map, and a block is
    // the scale the thing being captured actually changes at: what an indirect-light probe records
    // in a street is the bounce off the buildings either side of it, and that answer holds until
    // the buildings change. Denser costs six scene passes per probe at startup and records very
    // nearly the same photograph twice.
    double spacingMetres = 60.0;

    // How far above the lane point, metres. The lane is at road level, so this is height above the
    // road — and six metres is chosen to sit above anything on the road and still well down inside
    // the canyon, so the probe photographs walls and tarmac rather than mostly sky. Raising it
    // towards the roofline turns every street probe back into a copy of the global one.
    double heightMetres = 6.0;

    // Probes closer together than this, across all lanes, are dropped. This is what stops the two
    // carriageways of one road — parallel lanes fifteen metres apart — from each getting their own
    // probe of the same street, and what thins the pile where lanes overlap at a junction. Zero
    // keeps every sample.
    double minimumSeparationMetres = 45.0;
};

// One candidate probe stand, in metres, with the lane it came off so a report can say where it is.
export struct LaneProbe
{
    glm::dvec3 positionMetres{0.0};
    int laneId = 0;
    double distanceAlongLaneMetres = 0.0;
};

// Every place along the network worth standing a probe, in lane order. This is a *candidate list*
// and it is longer than any renderer will shade: `raceengine::maxIblProbes` is eight, and Grand
// City Parkway's 35 km of lane yields hundreds of these. Selecting from it is the caller's job —
// `nearestLaneProbes` below is the selection the game makes today.
export [[nodiscard]] std::vector<LaneProbe> laneProbePositions(const TrafficNetwork& network,
                                                               const LaneProbeOptions& options);

// The `count` candidates nearest a point, nearest first. Takes the list by value because it sorts
// it: the caller usually has no further use for the unsorted one.
export [[nodiscard]] std::vector<LaneProbe> nearestLaneProbes(std::vector<LaneProbe> probes,
                                                              const glm::dvec3& aroundMetres, std::size_t count);

// Read one off disk. Validated rather than trusted — a lane with one point or a point that is not
// three numbers is a truncated export, and traffic driving on it would read as a physics fault.
export [[nodiscard]] std::expected<TrafficNetwork, std::string> loadTrafficNetwork(const std::string& filePath);

} // namespace osr

namespace osr
{

namespace
{

// One `[x, y, z]` entry of a lane's `point_list`.
[[nodiscard]] std::expected<glm::dvec3, std::string> pointFrom(const JsonValue& entry, const int laneId,
                                                               const std::size_t index)
{
    const auto where = std::string(" (lane ")
                           .append(std::to_string(laneId))
                           .append(", point ")
                           .append(std::to_string(index))
                           .append(")");

    if (entry.kind != JsonKind::Array || entry.items.size() != 3)
    {
        return std::unexpected("a lane point is not three numbers" + where);
    }

    for (const auto& part : entry.items)
    {
        if (part.kind != JsonKind::Number)
        {
            return std::unexpected("a lane point carries something that is not a number" + where);
        }
    }

    return glm::dvec3(entry.items[0].number, entry.items[1].number, entry.items[2].number);
}

[[nodiscard]] std::expected<TrafficLane, std::string> laneFrom(const JsonValue& entry)
{
    if (entry.kind != JsonKind::Object)
    {
        return std::unexpected("a lane is not an object");
    }

    auto lane = TrafficLane{};
    lane.id = integerFrom(entry, "id", 0);
    lane.name = textFrom(entry, "name");
    lane.role = textFrom(entry, "role");
    lane.lengthMetres = numberFrom(entry, "length_m", 0.0);
    lane.loop = booleanFrom(entry, "loop", false);
    lane.priorityOffset = integerFrom(entry, "priority_offset", 0);
    lane.speedLimitKmh = numberFrom(entry, "speed_limit_kmh", 0.0);

    if (const auto* params = member(entry, "params"); params != nullptr)
    {
        lane.params.taxi = booleanFrom(*params, "taxi", false);
        lane.params.allowUTurns = booleanFrom(*params, "allowUTurns", false);
        lane.params.allowLaneChanges = booleanFrom(*params, "allowLaneChanges", false);
        lane.params.maximumCars = integerFrom(*params, "maxcars", 0);
    }

    if (const auto* elevation = member(entry, "elevation"); elevation != nullptr)
    {
        lane.elevationMinimumMetres = numberFrom(*elevation, "min_m", 0.0);
        lane.elevationMaximumMetres = numberFrom(*elevation, "max_m", 0.0);
    }

    const auto* pointList = member(entry, "point_list");
    if (pointList == nullptr || pointList->kind != JsonKind::Array)
    {
        return std::unexpected("lane " + std::to_string(lane.id) + " carries no 'point_list'");
    }

    lane.points.reserve(pointList->items.size());

    for (auto index = std::size_t{0}; index < pointList->items.size(); index++)
    {
        auto point = pointFrom(pointList->items[index], lane.id, index);
        if (!point)
        {
            return std::unexpected(std::move(point).error());
        }

        lane.points.push_back(point.value());
    }

    // A single point is not a lane: nothing can be interpolated along it and traffic put on it has
    // no heading. Refused rather than dropped, because a lane going missing between the export and
    // the game is the kind of fault that reads as a hole in the map months later.
    if (lane.points.size() < 2)
    {
        return std::unexpected("lane " + std::to_string(lane.id) + " carries fewer than two points");
    }

    lane.arcLengthMetres.reserve(lane.points.size());
    lane.arcLengthMetres.push_back(0.0);

    for (auto index = std::size_t{1}; index < lane.points.size(); index++)
    {
        lane.arcLengthMetres.push_back(lane.arcLengthMetres.back() +
                                       glm::distance(lane.points[index - 1], lane.points[index]));
    }

    return lane;
}

} // namespace

const TrafficRole* trafficRole(const TrafficNetwork& network, const std::string_view name)
{
    const auto found = std::ranges::find_if(network.roles, [name](const TrafficRole& role) { return role.name == name; });

    return found == network.roles.end() ? nullptr : &*found;
}

const TrafficLane* trafficLane(const TrafficNetwork& network, const int id)
{
    const auto found = std::ranges::find_if(network.lanes, [id](const TrafficLane& lane) { return lane.id == id; });

    return found == network.lanes.end() ? nullptr : &*found;
}

double laneChordLengthMetres(const TrafficLane& lane)
{
    return lane.arcLengthMetres.empty() ? 0.0 : lane.arcLengthMetres.back();
}

std::optional<LanePoint> laneSampleAt(const TrafficLane& lane, const double distanceMetres)
{
    if (lane.points.size() < 2 || lane.arcLengthMetres.size() != lane.points.size())
    {
        return std::nullopt;
    }

    const auto total = laneChordLengthMetres(lane);
    const auto wanted = std::clamp(distanceMetres, 0.0, total);

    // The last point whose arc length is at or before `wanted`, which is the segment it falls in.
    // `upper_bound` then a step back, so an exact hit on a point's own arc length lands on the
    // segment leaving it rather than the one arriving.
    const auto after = std::ranges::upper_bound(lane.arcLengthMetres, wanted);
    auto segment = static_cast<std::size_t>(after - lane.arcLengthMetres.begin());
    segment = segment == 0 ? 0 : segment - 1;
    segment = std::min(segment, lane.points.size() - 2);

    const auto& from = lane.points[segment];
    const auto& to = lane.points[segment + 1];
    const auto span = lane.arcLengthMetres[segment + 1] - lane.arcLengthMetres[segment];

    // A repeated point is a zero-length segment: it carries no direction of its own, so the answer
    // takes the point and leaves the tangent to whatever the next segment says.
    const auto along = span > 0.0 ? (wanted - lane.arcLengthMetres[segment]) / span : 0.0;
    const auto position = from + (to - from) * along;
    const auto step = to - from;
    const auto direction = span > 0.0 ? step / span : glm::dvec3(0.0, 0.0, 1.0);

    return LanePoint{.positionMetres = position, .direction = direction, .distanceMetres = wanted, .segment = segment};
}

std::vector<LanePoint> laneResample(const TrafficLane& lane, const double spacingMetres)
{
    auto samples = std::vector<LanePoint>();

    if (spacingMetres <= 0.0 || lane.points.size() < 2)
    {
        return samples;
    }

    const auto total = laneChordLengthMetres(lane);

    // Stepped off the index rather than accumulated, so the hundred-and-thirtieth sample of a
    // 7.8 km lane stands where the arithmetic says and not where a hundred and thirty additions
    // drifted to.
    for (auto index = std::size_t{0};; index++)
    {
        const auto distance = static_cast<double>(index) * spacingMetres;
        if (distance > total)
        {
            break;
        }

        auto sample = laneSampleAt(lane, distance);
        if (!sample)
        {
            break;
        }

        samples.push_back(sample.value());
    }

    return samples;
}

std::vector<LaneProbe> laneProbePositions(const TrafficNetwork& network, const LaneProbeOptions& options)
{
    auto probes = std::vector<LaneProbe>();

    for (const auto& lane : network.lanes)
    {
        for (const auto& sample : laneResample(lane, options.spacingMetres))
        {
            // Straight up from the road, in the export's own axes, where +y is up.
            const auto stand = sample.positionMetres + glm::dvec3(0.0, options.heightMetres, 0.0);

            if (options.minimumSeparationMetres > 0.0)
            {
                const auto crowded = std::ranges::any_of(probes,
                                                         [&](const LaneProbe& kept) {
                                                             return glm::distance(kept.positionMetres, stand) <
                                                                    options.minimumSeparationMetres;
                                                         });

                if (crowded)
                {
                    continue;
                }
            }

            probes.push_back(LaneProbe{.positionMetres = stand,
                                       .laneId = lane.id,
                                       .distanceAlongLaneMetres = sample.distanceMetres});
        }
    }

    return probes;
}

std::vector<LaneProbe> nearestLaneProbes(std::vector<LaneProbe> probes, const glm::dvec3& aroundMetres,
                                         const std::size_t count)
{
    const auto kept = std::min(count, probes.size());

    // Squared distance: the ordering is the same and the square root is not.
    const auto closer = [&aroundMetres](const LaneProbe& left, const LaneProbe& right)
    {
        const auto toLeft = left.positionMetres - aroundMetres;
        const auto toRight = right.positionMetres - aroundMetres;

        return glm::dot(toLeft, toLeft) < glm::dot(toRight, toRight);
    };

    std::ranges::partial_sort(probes, probes.begin() + static_cast<std::ptrdiff_t>(kept), closer);
    probes.resize(kept);

    return probes;
}

std::expected<TrafficNetwork, std::string> loadTrafficNetwork(const std::string& filePath)
{
    auto fileStream = std::ifstream(filePath, std::ios::binary);
    if (!fileStream.is_open())
    {
        return std::unexpected("Unable to open traffic network with path " + filePath);
    }

    auto buffer = std::ostringstream();
    buffer << fileStream.rdbuf();
    const auto document = std::move(buffer).str();

    auto reader = JsonReader(document);
    auto parsed = reader.read();
    if (!parsed)
    {
        return std::unexpected("Traffic network " + filePath + " is not readable JSON: " + parsed.error());
    }

    const auto& root = parsed.value();
    if (root.kind != JsonKind::Object)
    {
        return std::unexpected("Traffic network " + filePath + " is not a JSON object");
    }

    auto network = TrafficNetwork{};
    network.source = textFrom(root, "source");
    network.units = textFrom(root, "units");
    network.axes = textFrom(root, "axes");

    if (const auto* counts = member(root, "counts"); counts != nullptr)
    {
        network.counts.lanes = integerFrom(*counts, "lanes", 0);
        network.counts.intersections = integerFrom(*counts, "intersections", 0);
        network.counts.areas = integerFrom(*counts, "areas", 0);
        network.counts.parking = integerFrom(*counts, "parking", 0);
        network.counts.trafficLights = integerFrom(*counts, "traffic_lights", 0);
        network.counts.totalLaneLengthMetres = numberFrom(*counts, "total_lane_length_m", 0.0);
    }

    if (const auto* roles = member(root, "roles"); roles != nullptr && roles->kind == JsonKind::Array)
    {
        network.roles.reserve(roles->items.size());

        for (const auto& entry : roles->items)
        {
            network.roles.push_back(TrafficRole{.name = textFrom(entry, "name"),
                                                .speedLimitKmh = numberFrom(entry, "speed_limit_kmh", 0.0),
                                                .speedLimitMetresPerSecond = numberFrom(entry, "speed_limit_ms", 0.0),
                                                .priority = integerFrom(entry, "priority", 0)});
        }
    }

    const auto* lanes = member(root, "lanes");
    if (lanes == nullptr || lanes->kind != JsonKind::Array)
    {
        return std::unexpected("Traffic network " + filePath + " carries no 'lanes' array");
    }

    network.lanes.reserve(lanes->items.size());

    for (const auto& entry : lanes->items)
    {
        auto lane = laneFrom(entry);
        if (!lane)
        {
            return std::unexpected("Traffic network " + filePath + ": " + lane.error());
        }

        network.lanes.push_back(std::move(lane).value());
    }

    if (network.lanes.empty())
    {
        return std::unexpected("Traffic network " + filePath + " carries no lanes");
    }

    // The document's own summary against what was read. A mismatch is an export that lost a lane on
    // the way out, which is worth refusing rather than driving on: it is the difference between a
    // map with a missing street and a map somebody deliberately shortened.
    if (network.counts.lanes != 0 && static_cast<std::size_t>(network.counts.lanes) != network.lanes.size())
    {
        return std::unexpected("Traffic network " + filePath + " claims " + std::to_string(network.counts.lanes) +
                               " lanes and carries " + std::to_string(network.lanes.size()));
    }

    if (const auto* junctions = member(root, "junctions"); junctions != nullptr)
    {
        network.junctionToleranceMetres = numberFrom(*junctions, "tolerance_m", 0.0);

        if (const auto* links = member(*junctions, "links"); links != nullptr && links->kind == JsonKind::Array)
        {
            network.junctionLinks.reserve(links->items.size());

            for (const auto& entry : links->items)
            {
                network.junctionLinks.push_back(
                    TrafficJunctionLink{.kind = textFrom(entry, "kind"),
                                        .laneId = integerFrom(entry, "lane", 0),
                                        .targetLaneId = integerFrom(entry, "target_lane", 0),
                                        .distanceMetres = numberFrom(entry, "distance_m", 0.0),
                                        .targetSegment = integerFrom(entry, "target_segment", 0),
                                        .positionAlongTarget = numberFrom(entry, "position_along_target", 0.0),
                                        .headingAgreement = numberFrom(entry, "heading_agreement", 0.0)});
            }
        }
    }

    if (const auto* deadEnds = member(root, "dead_ends"); deadEnds != nullptr)
    {
        if (const auto* ids = member(*deadEnds, "lanes"); ids != nullptr && ids->kind == JsonKind::Array)
        {
            network.deadEndLaneIds.reserve(ids->items.size());

            for (const auto& entry : ids->items)
            {
                if (entry.kind == JsonKind::Number)
                {
                    network.deadEndLaneIds.push_back(static_cast<int>(entry.number));
                }
            }
        }
    }

    return network;
}

} // namespace osr
