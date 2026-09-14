module;

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <fstream>
#include <ios>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <glm/glm.hpp>

export module osr.game:RoadGraph;

import :Json;

namespace osr
{

// A track's road graph, as `~/dev/ac-car-data`'s `roads.py` derives it from the collision hull
// (docs/road-network-brief.md; the exporter's own account is its engine brief §7b): the junction
// boxes the author drew cut out of the carriageway, what is left centred and laned by its measured
// width, every arriving lane joined to every leaving one through each box. Metres, in the same
// right-handed axes glTF uses, so nothing here needs a remap.
//
// **Every number in the file is derived and it says so**: nothing in the track states a lane, a
// junction or a right of way, so a give-way rule here is the exporter's default to overrule, not a
// fact about the map. Two audit figures are what say the file is worth driving on, and both are
// carried: the fraction of lane and turn points on the carriageway (0.998 on Grand City Parkway)
// and the fraction of lanes in one circuit (1.0 — traffic put down anywhere can drive to anywhere).
//
// **Every array is indexed by position and an id is its subscript**, and the loader refuses a file
// where that is not so, because every consumer subscripts.
//
// Parsing is pure and answers with `std::expected`; the reader is `:Json`, this project's own, for
// the reason `TrafficNetwork.cppm` gives — the sandbox names no third-party headers. Nothing here
// imports `raceengine`, which keeps `<fstream>` out of a unit whose global module fragment would
// otherwise be merged against every imported BMI.

export struct RoadRole
{
    std::string name;
    // CSP's own ordering, signed: parking is −8 and a highway is +4.
    int priority = 0;
    double speedLimitKmh = 0.0;
};

export enum class RoadNodeKind : std::uint8_t
{
    // A junction box the author drew.
    Junction,
    // Roads meeting with no box drawn — a fork.
    Fork,
    // A road that ends; its lanes turn round.
    Terminal
};

// How a node is controlled: every approach gives way, or the lights run it.
export enum class RoadControl : std::uint8_t
{
    GiveWay,
    Signals
};

export struct RoadNode
{
    int id = 0;
    RoadNodeKind kind = RoadNodeKind::Junction;
    glm::dvec3 positionMetres{0.0};
    std::vector<int> edges;
    std::vector<int> inLanes;
    std::vector<int> outLanes;
    RoadControl control = RoadControl::GiveWay;
    // Signal ids, and the cycle they share, seconds. Empty and zero under give-way.
    std::vector<int> signals;
    double signalCycleSeconds = 0.0;
};

// One traffic light: an approach to a node — one edge in one direction, every lane of it — with
// its stop line at the lanes' own ends and its place in the node's cycle.
//
// **Where the lamps are is the only part of this the map states**, and it states it in the scenery:
// a junction is signalled when signal hardware stands over it (`counts.signalledJunctions`, 183 of
// Grand City Parkway's 184). **The cycle is invented** by the exporter and it says so: facing
// approaches are paired into phases, each phase gets its green and amber, red is the rest, and the
// offsets step by green plus amber so no two phases are ever green together. Nothing in the track
// states a green time. A left turn on green still gives way to the oncoming traffic — the turn's
// `givesWayTo` is read beside the light.
export struct RoadSignal
{
    int id = 0;
    int node = 0;
    int edge = 0;
    bool forward = true;
    // Which phase of the node's cycle this approach runs in, and how many there are.
    int phase = 0;
    int phases = 1;
    // The lanes this light stands over and, for each, how far along it the stop line is — the
    // lane's own length, to the millimetre, in every file so far.
    std::vector<int> lanes;
    std::vector<double> stopDistanceMetres;
    glm::dvec3 stopLineMetres{0.0};
    double greenSeconds = 20.0;
    double amberSeconds = 3.0;
    double redSeconds = 23.0;
    // When this light's green begins, seconds into the node's cycle.
    double offsetSeconds = 0.0;
    double cycleSeconds = 46.0;
    // The evidence: scenery vertices of signal hardware within reach of this stop line. Zero means
    // the approach is signalled because its junction is, not because a lamp stands by it.
    int lampVertices = 0;
};

// One road between two nodes: its centre line, its width and the lanes laid on it.
export struct RoadEdge
{
    int id = 0;
    int nodeA = 0;
    int nodeB = 0;
    // A stub: a road too short to be anything but the approach to a box.
    bool stub = false;
    std::string role;
    int lanesEachWay = 1;
    double lanePitchMetres = 3.5;
    double lengthMetres = 0.0;
    double speedLimitKmh = 0.0;
    int priority = 0;
    double widthMetres = 0.0;
    // Lane ids: `forward` runs along `centreLine`, `backward` against it.
    std::vector<int> forwardLanes;
    std::vector<int> backwardLanes;
    std::vector<glm::dvec3> centreLine;
};

// The lane beside a lane, which is where a lane change goes: on the same road, running the same
// way, the next index out or in.
export struct RoadNeighbour
{
    int lane = 0;
    // +1 to the driver's left, −1 to the right.
    double side = 1.0;
    double offsetMetres = 3.5;
};

export struct RoadLane
{
    int id = 0;
    int edge = 0;
    bool forward = true;
    // Counts outwards from the centre line: 0 is the lane beside oncoming traffic, the highest is
    // the kerb lane.
    int index = 0;
    int fromNode = 0;
    int toNode = 0;
    std::string role;
    double speedLimitKmh = 0.0;
    double widthMetres = 3.5;
    double lengthMetres = 0.0;
    // Turn ids: the movements out of this lane's end, and the ones into its start.
    std::vector<int> successors;
    std::vector<int> predecessors;
    // The lanes those successors land on, in the same order.
    std::vector<int> successorLanes;
    std::vector<RoadNeighbour> neighbours;
    // The signal standing at this lane's end, or −1 where the node it runs into gives way.
    int signal = -1;
    std::vector<glm::dvec3> points;
};

export enum class RoadTurnKind : std::uint8_t
{
    Straight,
    Left,
    Right,
    UTurn
};

// One movement a car may make through a node, with the curve to drive.
export struct RoadTurn
{
    int id = 0;
    int node = 0;
    int fromLane = 0;
    int toLane = 0;
    RoadTurnKind kind = RoadTurnKind::Straight;
    // Signed, left negative, as the exporter writes it.
    double angleDegrees = 0.0;
    double lengthMetres = 0.0;
    double speedLimitKmh = 0.0;
    int priority = 0;
    bool giveWay = false;
    // Turn ids. `conflicts` is every turn whose curve crosses this one or ends on the same lane;
    // `givesWayTo` the subset this one yields to — a pairwise tie break for two cars arriving
    // together and not an order, which the exporter's note says in as many words.
    std::vector<int> givesWayTo;
    std::vector<int> conflicts;
    std::vector<glm::dvec3> points;
};

export struct RoadCounts
{
    int nodes = 0;
    int junctions = 0;
    int forks = 0;
    int deadEnds = 0;
    int edges = 0;
    int lanes = 0;
    int turns = 0;
    int signals = 0;
    int signalledJunctions = 0;
    double roadLengthMetres = 0.0;
    double laneLengthMetres = 0.0;
    double turnLengthMetres = 0.0;
};

export struct RoadGraph
{
    std::string schema;
    std::string track;
    std::string units;
    std::string axes;
    // "right" or "left" — decided on the exporter's command line, not read off the map.
    std::string drive;

    RoadCounts counts;
    std::vector<RoadRole> roles;
    std::vector<RoadNode> nodes;
    std::vector<RoadEdge> edges;
    std::vector<RoadLane> lanes;
    std::vector<RoadTurn> turns;
    // Empty on a file written before the lights were read (2026-09-14, earlier the same day): every
    // junction then gives way.
    std::vector<RoadSignal> signals;

    // The two audit figures. Zero where the file carries none.
    double pointsOnCarriageway = 0.0;
    double circulation = 0.0;
};

// The role a lane names, or nothing where the table does not carry it.
export [[nodiscard]] const RoadRole* roadRole(const RoadGraph& graph, std::string_view name);

// How the road graph is turned into places to photograph the world from: along each road's centre
// line, one line per road rather than one per lane, so the two carriageways of a street share their
// probes. The numbers are the ones the lane placement used before it (`LaneProbeOptions`), for the
// reasons given there: a block's spacing, a height inside the canyon, a separation that thins the
// pile at a junction.
export struct RoadProbeOptions
{
    double spacingMetres = 60.0;
    double heightMetres = 6.0;
    double minimumSeparationMetres = 45.0;
};

export struct RoadProbe
{
    glm::dvec3 positionMetres{0.0};
    int edgeId = 0;
    double distanceAlongMetres = 0.0;
};

// Every place along the roads worth standing a probe, in edge order. A candidate list, longer than
// any frame shades; selecting from it is the caller's.
export [[nodiscard]] std::vector<RoadProbe> roadProbePositions(const RoadGraph& graph, const RoadProbeOptions& options);

// The `count` candidates nearest a point, nearest first. Takes the list by value because it sorts it.
export [[nodiscard]] std::vector<RoadProbe> nearestRoadProbes(std::vector<RoadProbe> probes,
                                                              const glm::dvec3& aroundMetres, std::size_t count);

// A place on a lane: where, which way the lane runs there, and which lane.
export struct RoadPlace
{
    glm::dvec3 positionMetres{0.0};
    // Unit, along the lane's direction of travel, flat.
    glm::dvec3 direction{0.0, 0.0, 1.0};
    int laneId = 0;
    int edgeId = 0;
    double distanceAlongMetres = 0.0;
    // How far the query was from the lane, in plan.
    double offsetMetres = 0.0;
};

// The nearest place to a point on any **kerb lane** — the outermost lane of its road in its
// direction, the one beside the pavement (index counts outwards from the centre line, so it is the
// highest index of the edge's lanes running that way; on a road one lane each way it is the only
// one). What the player is put down on: on the road, in the lane by the kerb, facing the way the
// lane runs (2026-09-14, Dominic: "move the player spawn location to be on the road closest the kerb
// facing the right way"). Nothing on a graph with no lanes.
export [[nodiscard]] std::optional<RoadPlace> nearestKerbLanePlace(const RoadGraph& graph, const glm::dvec3& pointMetres);

// Read one off disk. Validated rather than trusted: an id that is not its index, a link past the end
// of the array it names, a lane or a turn with one point, a counts block that disagrees with what
// was read — each is a truncated or a foreign export, and traffic driving on it would read as a
// physics fault.
export [[nodiscard]] std::expected<RoadGraph, std::string> loadRoadGraph(const std::string& filePath);

} // namespace osr

namespace osr
{

namespace
{

[[nodiscard]] std::expected<glm::dvec3, std::string> pointFrom(const JsonValue& entry, const std::string& where)
{
    if (entry.kind != JsonKind::Array || entry.items.size() != 3)
    {
        return std::unexpected("a point is not three numbers" + where);
    }

    for (const auto& part : entry.items)
    {
        if (part.kind != JsonKind::Number)
        {
            return std::unexpected("a point carries something that is not a number" + where);
        }
    }

    return glm::dvec3(entry.items[0].number, entry.items[1].number, entry.items[2].number);
}

[[nodiscard]] std::expected<std::vector<glm::dvec3>, std::string> pointsFrom(const JsonValue& parent,
                                                                             const std::string_view key,
                                                                             const std::string& where)
{
    const auto* list = member(parent, key);
    if (list == nullptr || list->kind != JsonKind::Array)
    {
        return std::unexpected("no '" + std::string(key) + "' array" + where);
    }

    auto points = std::vector<glm::dvec3>();
    points.reserve(list->items.size());

    for (const auto& entry : list->items)
    {
        auto point = pointFrom(entry, where);
        if (!point)
        {
            return std::unexpected(std::move(point).error());
        }

        points.push_back(point.value());
    }

    return points;
}

// An array of ids. A missing key is an empty list — a lane with no neighbours writes `[]`, and a
// file that leaves the key out means the same.
[[nodiscard]] std::vector<int> idsFrom(const JsonValue& parent, const std::string_view key)
{
    auto ids = std::vector<int>();

    const auto* list = member(parent, key);
    if (list == nullptr || list->kind != JsonKind::Array)
    {
        return ids;
    }

    ids.reserve(list->items.size());

    for (const auto& entry : list->items)
    {
        if (entry.kind == JsonKind::Number)
        {
            ids.push_back(static_cast<int>(entry.number));
        }
    }

    return ids;
}

[[nodiscard]] std::expected<RoadNodeKind, std::string> nodeKindFrom(const std::string& name, const std::string& where)
{
    if (name == "junction")
    {
        return RoadNodeKind::Junction;
    }

    if (name == "fork")
    {
        return RoadNodeKind::Fork;
    }

    if (name == "terminal")
    {
        return RoadNodeKind::Terminal;
    }

    return std::unexpected("node kind '" + name + "' is not one this game knows" + where);
}

[[nodiscard]] std::expected<RoadTurnKind, std::string> turnKindFrom(const std::string& name, const std::string& where)
{
    if (name == "straight")
    {
        return RoadTurnKind::Straight;
    }

    if (name == "left")
    {
        return RoadTurnKind::Left;
    }

    if (name == "right")
    {
        return RoadTurnKind::Right;
    }

    if (name == "uturn")
    {
        return RoadTurnKind::UTurn;
    }

    return std::unexpected("turn kind '" + name + "' is not one this game knows" + where);
}

// Every id in `ids` below `limit`, or which one is not.
[[nodiscard]] bool idsWithin(const std::vector<int>& ids, const std::size_t limit)
{
    return std::ranges::all_of(ids, [limit](const int id) { return id >= 0 && static_cast<std::size_t>(id) < limit; });
}

// A polyline's chord length.
[[nodiscard]] double chordLength(const std::vector<glm::dvec3>& points)
{
    auto total = 0.0;

    for (auto index = std::size_t{1}; index < points.size(); index++)
    {
        total += glm::distance(points[index - 1], points[index]);
    }

    return total;
}

// A polyline walked end to end at a fixed interval, always including its first point and never
// running past its last: the position at each step and the distance it stands at.
struct PolylineSample
{
    glm::dvec3 positionMetres{0.0};
    double distanceMetres = 0.0;
};

[[nodiscard]] std::vector<PolylineSample> polylineResample(const std::vector<glm::dvec3>& points, const double spacingMetres)
{
    auto samples = std::vector<PolylineSample>();

    if (spacingMetres <= 0.0 || points.size() < 2)
    {
        return samples;
    }

    auto arc = std::vector<double>();
    arc.reserve(points.size());
    arc.push_back(0.0);

    for (auto index = std::size_t{1}; index < points.size(); index++)
    {
        arc.push_back(arc.back() + glm::distance(points[index - 1], points[index]));
    }

    const auto total = arc.back();

    // Stepped off the index rather than accumulated, so a long road's last sample stands where the
    // arithmetic says and not where the additions drifted to.
    for (auto step = std::size_t{0};; step++)
    {
        const auto wanted = static_cast<double>(step) * spacingMetres;
        if (wanted > total)
        {
            break;
        }

        const auto after = std::ranges::upper_bound(arc, wanted);
        auto segment = static_cast<std::size_t>(after - arc.begin());
        segment = segment == 0 ? 0 : segment - 1;
        segment = std::min(segment, points.size() - 2);

        const auto span = arc[segment + 1] - arc[segment];
        const auto along = span > 0.0 ? (wanted - arc[segment]) / span : 0.0;

        samples.push_back(PolylineSample{.positionMetres = points[segment] + (points[segment + 1] - points[segment]) * along,
                                         .distanceMetres = wanted});
    }

    return samples;
}

} // namespace

const RoadRole* roadRole(const RoadGraph& graph, const std::string_view name)
{
    const auto found = std::ranges::find_if(graph.roles, [name](const RoadRole& role) { return role.name == name; });

    return found == graph.roles.end() ? nullptr : &*found;
}

std::optional<RoadPlace> nearestKerbLanePlace(const RoadGraph& graph, const glm::dvec3& pointMetres)
{
    auto best = std::optional<RoadPlace>{};

    // Which lanes are kerb lanes: per edge and direction, the highest index.
    const auto kerbLane = [&](const RoadLane& lane)
    {
        if (lane.edge < 0 || static_cast<std::size_t>(lane.edge) >= graph.edges.size())
        {
            return false;
        }

        const auto& edge = graph.edges[static_cast<std::size_t>(lane.edge)];
        const auto& beside = lane.forward ? edge.forwardLanes : edge.backwardLanes;

        for (const auto other : beside)
        {
            if (other >= 0 && static_cast<std::size_t>(other) < graph.lanes.size() &&
                graph.lanes[static_cast<std::size_t>(other)].index > lane.index)
            {
                return false;
            }
        }

        return true;
    };

    const auto flat = glm::dvec3(pointMetres.x, 0.0, pointMetres.z);

    for (const auto& lane : graph.lanes)
    {
        if (!kerbLane(lane))
        {
            continue;
        }

        auto along = 0.0;

        for (auto index = std::size_t{1}; index < lane.points.size(); index++)
        {
            const auto& from = lane.points[index - 1];
            const auto& to = lane.points[index];
            const auto chord = glm::dvec3(to.x - from.x, 0.0, to.z - from.z);
            const auto length = glm::length(chord);

            if (length <= 1e-9)
            {
                continue;
            }

            const auto direction = chord / length;
            const auto t = std::clamp(glm::dot(flat - glm::dvec3(from.x, 0.0, from.z), direction), 0.0, length);
            const auto place = from + (to - from) * (t / length);
            const auto offset = glm::length(glm::dvec3(place.x, 0.0, place.z) - flat);

            if (!best || offset < best->offsetMetres)
            {
                best = RoadPlace{.positionMetres = place,
                                 .direction = direction,
                                 .laneId = lane.id,
                                 .edgeId = lane.edge,
                                 .distanceAlongMetres = along + t,
                                 .offsetMetres = offset};
            }

            along += glm::distance(from, to);
        }
    }

    return best;
}

std::vector<RoadProbe> roadProbePositions(const RoadGraph& graph, const RoadProbeOptions& options)
{
    auto probes = std::vector<RoadProbe>();

    for (const auto& edge : graph.edges)
    {
        for (const auto& sample : polylineResample(edge.centreLine, options.spacingMetres))
        {
            // Straight up from the road, in the export's own axes, where +y is up.
            const auto stand = sample.positionMetres + glm::dvec3(0.0, options.heightMetres, 0.0);

            if (options.minimumSeparationMetres > 0.0)
            {
                const auto crowded = std::ranges::any_of(probes,
                                                         [&](const RoadProbe& kept) {
                                                             return glm::distance(kept.positionMetres, stand) <
                                                                    options.minimumSeparationMetres;
                                                         });

                if (crowded)
                {
                    continue;
                }
            }

            probes.push_back(
                RoadProbe{.positionMetres = stand, .edgeId = edge.id, .distanceAlongMetres = sample.distanceMetres});
        }
    }

    return probes;
}

std::vector<RoadProbe> nearestRoadProbes(std::vector<RoadProbe> probes, const glm::dvec3& aroundMetres,
                                         const std::size_t count)
{
    const auto kept = std::min(count, probes.size());

    const auto closer = [&aroundMetres](const RoadProbe& left, const RoadProbe& right)
    {
        const auto toLeft = left.positionMetres - aroundMetres;
        const auto toRight = right.positionMetres - aroundMetres;

        return glm::dot(toLeft, toLeft) < glm::dot(toRight, toRight);
    };

    std::ranges::partial_sort(probes, probes.begin() + static_cast<std::ptrdiff_t>(kept), closer);
    probes.resize(kept);

    return probes;
}

std::expected<RoadGraph, std::string> loadRoadGraph(const std::string& filePath)
{
    auto fileStream = std::ifstream(filePath, std::ios::binary);
    if (!fileStream.is_open())
    {
        return std::unexpected("Unable to open road graph with path " + filePath);
    }

    auto buffer = std::ostringstream();
    buffer << fileStream.rdbuf();
    const auto document = std::move(buffer).str();

    auto reader = JsonReader(document);
    auto parsed = reader.read();
    if (!parsed)
    {
        return std::unexpected("Road graph " + filePath + " is not readable JSON: " + parsed.error());
    }

    const auto& root = parsed.value();
    if (root.kind != JsonKind::Object)
    {
        return std::unexpected("Road graph " + filePath + " is not a JSON object");
    }

    const auto fault = [&filePath](std::string what) { return std::unexpected("Road graph " + filePath + ": " + std::move(what)); };

    auto graph = RoadGraph{};
    graph.schema = textFrom(root, "schema");
    graph.track = textFrom(root, "track");
    graph.units = textFrom(root, "units");
    graph.axes = textFrom(root, "axes");

    // The schema line is the one thing that says this is the file this reader is for and not CSP's
    // own lane export, whose shape is the older `TrafficNetwork.cppm`'s.
    if (!graph.schema.starts_with("ac-car-data road-graph"))
    {
        return fault("schema '" + graph.schema + "' is not an ac-car-data road graph");
    }

    if (const auto* settings = member(root, "settings"); settings != nullptr)
    {
        graph.drive = textFrom(*settings, "drive");
    }

    if (const auto* counts = member(root, "counts"); counts != nullptr)
    {
        graph.counts.nodes = integerFrom(*counts, "nodes", 0);
        graph.counts.junctions = integerFrom(*counts, "junctions", 0);
        graph.counts.forks = integerFrom(*counts, "forks", 0);
        graph.counts.deadEnds = integerFrom(*counts, "dead_ends", 0);
        graph.counts.edges = integerFrom(*counts, "edges", 0);
        graph.counts.lanes = integerFrom(*counts, "lanes", 0);
        graph.counts.turns = integerFrom(*counts, "turns", 0);
        graph.counts.signals = integerFrom(*counts, "signals", 0);
        graph.counts.signalledJunctions = integerFrom(*counts, "signalled_junctions", 0);
        graph.counts.roadLengthMetres = numberFrom(*counts, "road_length_m", 0.0);
        graph.counts.laneLengthMetres = numberFrom(*counts, "lane_length_m", 0.0);
        graph.counts.turnLengthMetres = numberFrom(*counts, "turn_length_m", 0.0);
    }

    if (const auto* audit = member(root, "audit"); audit != nullptr)
    {
        if (const auto* on = member(*audit, "points_on_carriageway"); on != nullptr)
        {
            graph.pointsOnCarriageway = numberFrom(*on, "fraction", 0.0);
        }

        if (const auto* circulation = member(*audit, "circulation"); circulation != nullptr)
        {
            graph.circulation = numberFrom(*circulation, "fraction", 0.0);
        }
    }

    if (const auto* roles = member(root, "roles"); roles != nullptr && roles->kind == JsonKind::Array)
    {
        graph.roles.reserve(roles->items.size());

        for (const auto& entry : roles->items)
        {
            graph.roles.push_back(RoadRole{.name = textFrom(entry, "name"),
                                           .priority = integerFrom(entry, "priority", 0),
                                           .speedLimitKmh = numberFrom(entry, "speedLimit", 0.0)});
        }
    }

    const auto arrayOf = [&](const std::string_view key) -> const JsonValue*
    {
        const auto* list = member(root, key);

        return (list != nullptr && list->kind == JsonKind::Array) ? list : nullptr;
    };

    const auto* nodes = arrayOf("nodes");
    const auto* edges = arrayOf("edges");
    const auto* lanes = arrayOf("lanes");
    const auto* turns = arrayOf("turns");

    if (nodes == nullptr || edges == nullptr || lanes == nullptr || turns == nullptr)
    {
        return fault("carries no 'nodes', 'edges', 'lanes' or 'turns' array");
    }

    // --- nodes -----------------------------------------------------------------------------------
    graph.nodes.reserve(nodes->items.size());

    for (auto index = std::size_t{0}; index < nodes->items.size(); index++)
    {
        const auto& entry = nodes->items[index];
        const auto where = " (node " + std::to_string(index) + ")";

        auto node = RoadNode{};
        node.id = integerFrom(entry, "id", -1);

        if (node.id != static_cast<int>(index))
        {
            return fault("node id " + std::to_string(node.id) + " is not its index" + where);
        }

        auto kind = nodeKindFrom(textFrom(entry, "kind"), where);
        if (!kind)
        {
            return fault(std::move(kind).error());
        }

        node.kind = kind.value();

        if (const auto* position = member(entry, "position"); position != nullptr)
        {
            auto point = pointFrom(*position, where);
            if (!point)
            {
                return fault(std::move(point).error());
            }

            node.positionMetres = point.value();
        }

        node.edges = idsFrom(entry, "edges");
        node.inLanes = idsFrom(entry, "in_lanes");
        node.outLanes = idsFrom(entry, "out_lanes");
        node.control = textFrom(entry, "control") == "signals" ? RoadControl::Signals : RoadControl::GiveWay;
        node.signals = idsFrom(entry, "signals");
        node.signalCycleSeconds = numberFrom(entry, "signal_cycle_s", 0.0);

        graph.nodes.push_back(std::move(node));
    }

    // --- edges -----------------------------------------------------------------------------------
    graph.edges.reserve(edges->items.size());

    for (auto index = std::size_t{0}; index < edges->items.size(); index++)
    {
        const auto& entry = edges->items[index];
        const auto where = " (road " + std::to_string(index) + ")";

        auto edge = RoadEdge{};
        edge.id = integerFrom(entry, "id", -1);

        if (edge.id != static_cast<int>(index))
        {
            return fault("road id " + std::to_string(edge.id) + " is not its index" + where);
        }

        edge.nodeA = integerFrom(entry, "node_a", -1);
        edge.nodeB = integerFrom(entry, "node_b", -1);
        edge.stub = booleanFrom(entry, "stub", false);
        edge.role = textFrom(entry, "role");
        edge.lanesEachWay = integerFrom(entry, "lanes_each_way", 1);
        edge.lanePitchMetres = numberFrom(entry, "lane_pitch_m", 3.5);
        edge.lengthMetres = numberFrom(entry, "length_m", 0.0);
        edge.speedLimitKmh = numberFrom(entry, "speed_limit_kmh", 0.0);
        edge.priority = integerFrom(entry, "priority", 0);

        if (const auto* width = member(entry, "width_m"); width != nullptr)
        {
            edge.widthMetres = numberFrom(*width, "median", 0.0);
        }

        edge.forwardLanes = idsFrom(entry, "forward_lanes");
        edge.backwardLanes = idsFrom(entry, "backward_lanes");

        auto centre = pointsFrom(entry, "centre_line", where);
        if (!centre)
        {
            return fault(std::move(centre).error());
        }

        edge.centreLine = std::move(centre).value();

        if (edge.centreLine.size() < 2)
        {
            return fault("a road's centre line carries fewer than two points" + where);
        }

        graph.edges.push_back(std::move(edge));
    }

    // --- lanes -----------------------------------------------------------------------------------
    graph.lanes.reserve(lanes->items.size());

    for (auto index = std::size_t{0}; index < lanes->items.size(); index++)
    {
        const auto& entry = lanes->items[index];
        const auto where = " (lane " + std::to_string(index) + ")";

        auto lane = RoadLane{};
        lane.id = integerFrom(entry, "id", -1);

        if (lane.id != static_cast<int>(index))
        {
            return fault("lane id " + std::to_string(lane.id) + " is not its index" + where);
        }

        lane.edge = integerFrom(entry, "edge", -1);
        lane.forward = textFrom(entry, "direction") != "backward";
        lane.index = integerFrom(entry, "index", 0);
        lane.fromNode = integerFrom(entry, "from_node", -1);
        lane.toNode = integerFrom(entry, "to_node", -1);
        lane.role = textFrom(entry, "role");
        lane.speedLimitKmh = numberFrom(entry, "speed_limit_kmh", 0.0);
        lane.widthMetres = numberFrom(entry, "width_m", 3.5);
        lane.lengthMetres = numberFrom(entry, "length_m", 0.0);
        lane.successors = idsFrom(entry, "successors");
        lane.predecessors = idsFrom(entry, "predecessors");
        lane.successorLanes = idsFrom(entry, "successor_lanes");
        lane.signal = integerFrom(entry, "signal", -1);

        if (const auto* neighbours = member(entry, "neighbours"); neighbours != nullptr && neighbours->kind == JsonKind::Array)
        {
            lane.neighbours.reserve(neighbours->items.size());

            for (const auto& beside : neighbours->items)
            {
                if (beside.kind != JsonKind::Object)
                {
                    return fault("a neighbour is not an object" + where);
                }

                lane.neighbours.push_back(RoadNeighbour{.lane = integerFrom(beside, "lane", -1),
                                                        .side = numberFrom(beside, "side", 1.0),
                                                        .offsetMetres = numberFrom(beside, "offset_m", 3.5)});
            }
        }

        auto points = pointsFrom(entry, "points", where);
        if (!points)
        {
            return fault(std::move(points).error());
        }

        lane.points = std::move(points).value();

        // A single point is not a lane: nothing can be interpolated along it and traffic put on it has
        // no heading. Refused rather than dropped, because a lane going missing between the export
        // and the game is the kind of fault that reads as a hole in the map months later.
        if (lane.points.size() < 2)
        {
            return fault("a lane carries fewer than two points" + where);
        }

        graph.lanes.push_back(std::move(lane));
    }

    // --- turns -----------------------------------------------------------------------------------
    graph.turns.reserve(turns->items.size());

    for (auto index = std::size_t{0}; index < turns->items.size(); index++)
    {
        const auto& entry = turns->items[index];
        const auto where = " (turn " + std::to_string(index) + ")";

        auto turn = RoadTurn{};
        turn.id = integerFrom(entry, "id", -1);

        if (turn.id != static_cast<int>(index))
        {
            return fault("turn id " + std::to_string(turn.id) + " is not its index" + where);
        }

        turn.node = integerFrom(entry, "node", -1);
        turn.fromLane = integerFrom(entry, "from_lane", -1);
        turn.toLane = integerFrom(entry, "to_lane", -1);

        auto kind = turnKindFrom(textFrom(entry, "kind"), where);
        if (!kind)
        {
            return fault(std::move(kind).error());
        }

        turn.kind = kind.value();
        turn.angleDegrees = numberFrom(entry, "angle_deg", 0.0);
        turn.lengthMetres = numberFrom(entry, "length_m", 0.0);
        turn.speedLimitKmh = numberFrom(entry, "speed_limit_kmh", 0.0);
        turn.priority = integerFrom(entry, "priority", 0);
        turn.giveWay = booleanFrom(entry, "give_way", false);
        turn.givesWayTo = idsFrom(entry, "gives_way_to");
        turn.conflicts = idsFrom(entry, "conflicts");

        auto points = pointsFrom(entry, "points", where);
        if (!points)
        {
            return fault(std::move(points).error());
        }

        turn.points = std::move(points).value();

        if (turn.points.size() < 2)
        {
            return fault("a turn carries fewer than two points" + where);
        }

        graph.turns.push_back(std::move(turn));
    }

    if (graph.lanes.empty() || graph.turns.empty())
    {
        return fault("carries no lanes or no turns");
    }

    // --- signals ---------------------------------------------------------------------------------
    if (const auto* signals = arrayOf("signals"); signals != nullptr)
    {
        graph.signals.reserve(signals->items.size());

        for (auto index = std::size_t{0}; index < signals->items.size(); index++)
        {
            const auto& entry = signals->items[index];
            const auto where = " (signal " + std::to_string(index) + ")";

            auto signal = RoadSignal{};
            signal.id = integerFrom(entry, "id", -1);

            if (signal.id != static_cast<int>(index))
            {
                return fault("signal id " + std::to_string(signal.id) + " is not its index" + where);
            }

            signal.node = integerFrom(entry, "node", -1);
            signal.edge = integerFrom(entry, "edge", -1);
            signal.forward = textFrom(entry, "direction") != "backward";
            signal.phase = integerFrom(entry, "phase", 0);
            signal.phases = integerFrom(entry, "phases", 1);
            signal.lanes = idsFrom(entry, "lanes");

            if (const auto* stops = member(entry, "stop_distance_m"); stops != nullptr && stops->kind == JsonKind::Array)
            {
                for (const auto& stop : stops->items)
                {
                    signal.stopDistanceMetres.push_back(stop.kind == JsonKind::Number ? stop.number : -1.0);
                }
            }

            if (const auto* line = member(entry, "stop_line"); line != nullptr)
            {
                auto point = pointFrom(*line, where);
                if (!point)
                {
                    return fault(std::move(point).error());
                }

                signal.stopLineMetres = point.value();
            }

            signal.greenSeconds = numberFrom(entry, "green_s", 20.0);
            signal.amberSeconds = numberFrom(entry, "amber_s", 3.0);
            signal.redSeconds = numberFrom(entry, "red_s", 23.0);
            signal.offsetSeconds = numberFrom(entry, "offset_s", 0.0);
            signal.cycleSeconds = numberFrom(entry, "cycle_s", signal.greenSeconds + signal.amberSeconds + signal.redSeconds);
            signal.lampVertices = integerFrom(entry, "lamp_vertices_near_the_stop_line", 0);

            if (signal.lanes.empty() || signal.greenSeconds <= 0.0 || signal.cycleSeconds <= 0.0)
            {
                return fault("a signal stands over no lane or runs no cycle" + where);
            }

            graph.signals.push_back(std::move(signal));
        }
    }

    // --- the links, every one of them inside the array it names --------------------------------------
    const auto nodeCount = graph.nodes.size();
    const auto edgeCount = graph.edges.size();
    const auto laneCount = graph.lanes.size();
    const auto turnCount = graph.turns.size();

    for (const auto& node : graph.nodes)
    {
        if (!idsWithin(node.edges, edgeCount) || !idsWithin(node.inLanes, laneCount) || !idsWithin(node.outLanes, laneCount))
        {
            return fault("node " + std::to_string(node.id) + " names a road or a lane that does not exist");
        }
    }

    for (const auto& edge : graph.edges)
    {
        if (edge.nodeA < 0 || static_cast<std::size_t>(edge.nodeA) >= nodeCount || edge.nodeB < 0 ||
            static_cast<std::size_t>(edge.nodeB) >= nodeCount)
        {
            return fault("road " + std::to_string(edge.id) + " names a node that does not exist");
        }

        if (!idsWithin(edge.forwardLanes, laneCount) || !idsWithin(edge.backwardLanes, laneCount))
        {
            return fault("road " + std::to_string(edge.id) + " names a lane that does not exist");
        }
    }

    for (const auto& lane : graph.lanes)
    {
        if (lane.edge < 0 || static_cast<std::size_t>(lane.edge) >= edgeCount)
        {
            return fault("lane " + std::to_string(lane.id) + " names a road that does not exist");
        }

        if (!idsWithin(lane.successors, turnCount) || !idsWithin(lane.predecessors, turnCount))
        {
            return fault("lane " + std::to_string(lane.id) + " names a turn that does not exist");
        }

        if (!idsWithin(lane.successorLanes, laneCount))
        {
            return fault("lane " + std::to_string(lane.id) + " names a successor lane that does not exist");
        }

        for (const auto& beside : lane.neighbours)
        {
            if (beside.lane < 0 || static_cast<std::size_t>(beside.lane) >= laneCount)
            {
                return fault("lane " + std::to_string(lane.id) + " names a neighbour that does not exist");
            }
        }
    }

    for (const auto& turn : graph.turns)
    {
        if (turn.fromLane < 0 || static_cast<std::size_t>(turn.fromLane) >= laneCount || turn.toLane < 0 ||
            static_cast<std::size_t>(turn.toLane) >= laneCount)
        {
            return fault("turn " + std::to_string(turn.id) + " names a lane that does not exist");
        }

        if (!idsWithin(turn.givesWayTo, turnCount) || !idsWithin(turn.conflicts, turnCount))
        {
            return fault("turn " + std::to_string(turn.id) + " names a turn that does not exist");
        }
    }

    const auto signalCount = graph.signals.size();

    for (const auto& signal : graph.signals)
    {
        if (signal.node < 0 || static_cast<std::size_t>(signal.node) >= nodeCount || !idsWithin(signal.lanes, laneCount))
        {
            return fault("signal " + std::to_string(signal.id) + " names a node or a lane that does not exist");
        }
    }

    for (const auto& node : graph.nodes)
    {
        if (!idsWithin(node.signals, signalCount))
        {
            return fault("node " + std::to_string(node.id) + " names a signal that does not exist");
        }
    }

    for (const auto& lane : graph.lanes)
    {
        if (lane.signal >= 0 && static_cast<std::size_t>(lane.signal) >= signalCount)
        {
            return fault("lane " + std::to_string(lane.id) + " names a signal that does not exist");
        }
    }

    // The document's own summary against what was read. A mismatch is an export that lost a road on
    // the way out, which is worth refusing rather than driving on.
    const auto claims = [&](const int claimed, const std::size_t read, const char* what) -> std::expected<void, std::string>
    {
        if (claimed != 0 && static_cast<std::size_t>(claimed) != read)
        {
            return std::unexpected("Road graph " + filePath + " claims " + std::to_string(claimed) + " " + what +
                                   " and carries " + std::to_string(read));
        }

        return {};
    };

    for (const auto& check : {claims(graph.counts.nodes, nodeCount, "nodes"), claims(graph.counts.edges, edgeCount, "roads"),
                              claims(graph.counts.lanes, laneCount, "lanes"), claims(graph.counts.turns, turnCount, "turns"),
                              claims(graph.counts.signals, signalCount, "signals")})
    {
        if (!check)
        {
            return std::unexpected(check.error());
        }
    }

    // Filled in where the file left it out, so a log line can quote a length either way.
    if (graph.counts.laneLengthMetres <= 0.0)
    {
        for (const auto& lane : graph.lanes)
        {
            graph.counts.laneLengthMetres += chordLength(lane.points);
        }
    }

    if (graph.counts.roadLengthMetres <= 0.0)
    {
        for (const auto& edge : graph.edges)
        {
            graph.counts.roadLengthMetres += chordLength(edge.centreLine);
        }
    }

    return graph;
}

} // namespace osr
