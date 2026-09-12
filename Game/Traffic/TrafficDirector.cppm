module;

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <Profiling/RaceEngineProfile.hpp>

export module osr.game:TrafficDirector;

import :PoliceCar;
import :TrafficLog;
import :TrafficNetwork;

import raceengine;

namespace osr
{

// The game's half of traffic: it turns the track's CSP lane export into the network the engine's
// traffic module wants, ticks that module on the simulation's thread, and publishes a pose per car
// to whichever thread is drawing.
//
// **The model itself is not here and deliberately so.** `raceengine.traffic` takes plain lane
// geometry and knows nothing about assets, JSON or Assetto Corsa, which is what lets every
// derivation in it — the lane graph, the driver model, the recovery — be unit tested with no file on
// disk. What is left for this class is the two seams that cannot be: reading the export, and the
// handoff between the thread that ticks and the thread that draws.
//
// The publish follows `Simulation::freeProps` exactly, and for the same reason: the tick writes
// under a `try_lock` and never waits, because a dropped publish costs the picture one tick of
// freshness and a stalled tick costs a deadline.

// One car, as the thing that draws it needs to see it. Metres — the conversion to world units is
// the renderer's, the same way it is for the player's car.
export struct TrafficSnapshot
{
    glm::dvec3 positionMetres{0.0};
    glm::dquat orientation{1.0, 0.0, 0.0, 0.0};
    // How far the car has rolled forward since it was placed, metres, unwrapped. The renderer turns
    // it into a wheel angle against the radius of the asset it draws this car with.
    double rolledMetres = 0.0;
    std::uint32_t id = 0;
    // Which body shape and which colour this car is, as indices into whatever fleet the renderer
    // owns. Drawn once when the car was created and never changed, so a car does not change colour
    // when it goes round a corner.
    std::uint8_t body = 0;
    std::uint8_t colour = 0;
    // Whether the car is a wreck rather than traffic — not merely a body, which a car near the player
    // is too. Read by nothing today; it is what a future hazard light or a damaged body variant would
    // switch on.
    bool disturbed = false;
    // For the ear rather than the eye: the Doppler shift wants the car's velocity and the virtual
    // gearbox its acceleration along the lane. The renderer reads neither.
    glm::dvec3 velocityMetresPerSecond{0.0};
    double accelerationMetresPerSecondSquared = 0.0;
    // A patrol car, and whether its light bar and siren are on — which they are from the moment it
    // joins a chase until the chase is over for it (docs/police-pursuit-brief.md). Read by whoever
    // draws or sounds the car.
    bool police = false;
    bool siren = false;
};

export struct TrafficSettings
{
    bool enabled = true;

    // Cars per kilometre of lane. Grand City Parkway carries 35.1 km, so eleven is about 380 cars.
    double densityPerKilometre = 11.0;
    std::size_t maximumAgents = 512;
    std::size_t maximumDisturbed = 24;

    // How far from the car the vehicle model is offered traffic to hit. Forty metres is well past
    // anything a car can reach inside one tick and short enough that the list stays a handful.
    double obstacleRadiusMetres = 40.0;

    // Inside the first a traffic car is a rigid body following its lane rather than a point on it;
    // outside the second it is a point again. Zero is off. The scene sets the first from the
    // nearest level of detail, because what a body buys — the springs, the camber, a pull-out
    // steered rather than slid — is only visible at that range.
    double embodyRadiusMetres = 0.0;
    double disembodyRadiusMetres = 0.0;

    // How many body shapes and how many paint colours the renderer has.
    std::uint8_t bodyCount = 1;
    std::uint8_t colourCount = 10;

    // --- the police (docs/police-pursuit-brief.md) -----------------------------------------------
    //
    // Which body shape is the patrol car, or none; what fraction of the city is police; and how many
    // of them may be the full vehicle model at once. Six full models is six more vehicle ticks a
    // tick, about what the player's own costs each, and the rest of a swarm stays a rigid body.
    std::uint8_t policeBody = raceengine::noPoliceBody;
    double policeShare = 0.08;
    std::size_t maximumFullModels = 6;
    // The pursuit's route searches on a thread of their own (docs/pursuit-radio-brief.md, §1.1). Off,
    // they run on the simulation thread at one a tick — which is what a driven capture needs, its
    // tick count being a function of the frame, and what stalled a 360 Hz tick by six on the parkway.
    bool routeOnWorker = true;

    // The position log, `OSR_TRAFFIC_LOG=<file>` (TrafficLog.cppm): every car's pose and state at
    // 30 Hz and on every tick something about it changes. Empty is off, which is every run that did
    // not ask for it.
    std::string logPath;

    std::uint64_t seed = 0x9E3779B97F4A7C15ULL;
};

// The player, as the simulation tells the traffic about it each tick: what the pursuit director
// judges, and the car as an obstacle a patrol car's own solver can hit.
export struct PlayerFrame
{
    raceengine::PursuitPlayer player{};
    raceengine::DynamicObstacle obstacle{};
};

// What a patrol car's own solve did to the player's body this tick, for the simulation to apply, and
// the player's share of the hit for its damage ledger (`BodyworkHit`).
export struct PlayerNudge
{
    glm::dvec3 deltaLinear{0.0};
    glm::dvec3 deltaAngular{0.0};
    double impactMetresPerSecond = 0.0;
};

export class TrafficDirector
{
public:
    // Fallible in the same way the rest of the load path is: a lane graph that cannot be derived is
    // a broken export, and a city with silently no traffic in it is the kind of fault nobody
    // notices until somebody asks why the streets are empty.
    //
    // `navmesh` is the track's drivable area for the police to route over, or nothing where the
    // track names none (docs/pursuit-navigation-brief.md, stage 1b); the lanes are the road either way.
    TrafficDirector(raceengine::Engine& engine, const TrafficNetwork& source, const TrafficSettings& settings,
                    std::optional<raceengine::NavMesh> navmesh);

    TrafficDirector(const TrafficDirector&) = delete;
    TrafficDirector(TrafficDirector&&) = delete;
    TrafficDirector& operator=(const TrafficDirector&) = delete;
    TrafficDirector& operator=(TrafficDirector&&) = delete;

    // One tick, on the simulation's thread, before the cars are stepped. `player` is the police's
    // business — null on a run with no car in it — and with it the pursuit director runs, the
    // patrol cars it has taken as full models are stepped, and their poses go back to the population.
    void tick(double deltaTime, double simulatedSeconds, const glm::dvec3& focusMetres,
              std::span<const raceengine::TrafficVehicle> vehicles, const raceengine::PhysicsWorld& world,
              const PlayerFrame* player);

    // What the full-model patrol cars' solves did to the player's body on this tick. Simulation
    // thread, after `tick` and before the player's own step.
    [[nodiscard]] std::span<const PlayerNudge> playerNudges() const;
    // What the full models' own solves decided about the street furniture this tick, for the
    // simulation to hand to the world beside the player's: the props whose anchors gave way, and
    // the velocities the two-body solves gave each prop they touched (docs/police-driving-brief.md
    // §2.2). Before this a prop a patrol car hit never broke in the world.
    [[nodiscard]] std::span<const std::uint32_t> propReleases() const;
    [[nodiscard]] std::span<const raceengine::PropVelocity> propVelocities() const;

    // The chase as the director sees it. Simulation thread.
    [[nodiscard]] const raceengine::PursuitStatus& pursuit() const;
    [[nodiscard]] bool isPolice(std::uint32_t id) const;
    // The navigation mesh the track carries, or null. Any thread: it is immutable after load.
    [[nodiscard]] const raceengine::NavMesh* navMesh() const;

    // The city laid out again, the chase forgotten, every full model handed back.
    void restart();
    // The meter put at a level, one to five, and a chase started if none is on: the number keys.
    // Simulation thread.
    void setWantedLevel(int level);

    // The traffic within reach of a point, as obstacles a vehicle tick can be handed. Appended, and
    // the caller is expected to reuse the vector so a tick allocates nothing.
    void obstaclesNear(const glm::dvec3& pointMetres, std::vector<raceengine::DynamicObstacle>& into) const;

    // Whatever a vehicle's contact solver did to those obstacles, and what its bodywork read off the
    // same manifold (`bodyworkHits`), so a patrol car the player hit is charged the player's solve's
    // share of that hit. Simulation thread, after the tick that produced the manifold.
    void applyContacts(const raceengine::ContactManifold& manifold, std::span<const BodyworkHit> hits);

    // Simulation thread: hand the newest poses over. Render thread: take a copy.
    void publish();
    void collect(std::vector<TrafficSnapshot>& into) const;

    [[nodiscard]] std::size_t count() const;

private:
    // The pursuit director's units that are full models: hand a pursuing body over, or take one back.
    void manageFullModels(const raceengine::PhysicsWorld& world);
    void stepFullModels(double deltaTime, const PlayerFrame& player, const raceengine::PhysicsWorld& world);
    [[nodiscard]] PoliceCar* fullModel(std::uint32_t id);
    void reportChase();

    raceengine::Engine& engine;
    TrafficSettings settings;
    std::optional<raceengine::TrafficPopulation> population;
    // The last values the two diagnostics above were logged at.
    std::size_t reportedHeld = 0;
    std::size_t reportedDropped = 0;

    // The position log (`OSR_TRAFFIC_LOG`), and what each car last logged, so a row is written on the
    // tick something about a car changes and not only on the periodic tick.
    struct LogMemory
    {
        bool seen = false;
        raceengine::AgentMode mode = raceengine::AgentMode::Cruising;
        std::size_t lane = 0;
        std::size_t slot = 0;
        glm::dvec3 positionMetres{0.0};
    };
    void writeLog();
    std::optional<TrafficLog> trafficLog;
    std::vector<LogMemory> logMemory;
    std::uint64_t tickIndex = 0;
    glm::dvec3 logFocusMetres{0.0};
    glm::dvec3 logPlayerVelocity{0.0};
    glm::dvec3 logPlayerForward{0.0};
    bool logRestart = false;

    // --- the police (docs/police-pursuit-brief.md) -----------------------------------------------
    // The ground the pursuit routes over, held for the life of the track; the road stays the lanes.
    std::optional<raceengine::NavMesh> navmesh;
    // The router over the population's lanes and the mesh above, built once they both stand
    // (docs/pursuit-navigation-brief.md, stage 1b). After the population, whose network it borrows;
    // before the director, which borrows it.
    std::optional<raceengine::PursuitRouter> router;
    // ...or the same router on its own thread, which is the game's setting. One of the two stands.
    // Declared after the router's inputs and before the director, which borrows it; its thread is
    // joined when this class is destroyed, after the director and before the mesh.
    std::optional<raceengine::PursuitRouteWorker> routeWorker;
    raceengine::PursuitDirector pursuitDirector;
    // The full models, `settings.maximumFullModels` of them, each live while it holds a unit.
    std::vector<PoliceCar> policeCars;
    std::vector<PlayerNudge> nudges;
    std::vector<std::uint32_t> propReleaseScratch;
    std::vector<raceengine::PropVelocity> propVelocityScratch;
    std::vector<raceengine::DynamicObstacle> policeObstacles;
    // The caller's vehicles and the live full models, as the lane occupants the population is told
    // about: a patrol car the population is not stepping is otherwise a car no traffic queues behind.
    std::vector<raceengine::TrafficVehicle> vehicleScratch;
    // The chase as it was last reported, so the log carries transitions and not a line a tick.
    bool reportedActive = false;
    int reportedLevel = 0;
    bool reportedSwarm = false;
    bool reportedWrecked = false;
    bool reportedSearching = false;
    std::size_t reportedUnits = 0;
    double reportedRouteWorstSeconds = 0.0;

    mutable std::mutex publication;
    std::vector<TrafficSnapshot> published;
    // Scratch the tick reuses, so a publish allocates nothing after the first one.
    std::vector<TrafficSnapshot> scratch;
};

} // namespace osr

namespace osr
{

namespace
{

// The export's lanes as the engine's traffic module wants them.
//
// Two things are decided here and nowhere else. The speed limit is the lane's own where it states
// one and the role's otherwise — CSP writes both and they agree on this map, but a lane that states
// nothing has to fall back on the class of road it is. And `allowLaneChanges` is carried straight
// through: it is a property of the road the author stated, not something to derive.
[[nodiscard]] std::vector<raceengine::LaneSource> laneSourcesFrom(const TrafficNetwork& source)
{
    auto sources = std::vector<raceengine::LaneSource>();
    sources.reserve(source.lanes.size());

    for (const auto& lane : source.lanes)
    {
        auto limit = lane.speedLimitKmh / 3.6;

        if (limit <= 0.0)
        {
            if (const auto* role = trafficRole(source, lane.role); role != nullptr)
            {
                limit = role->speedLimitMetresPerSecond;
            }
        }

        sources.push_back(raceengine::LaneSource{.id = lane.id,
                                                 .name = lane.name,
                                                 .speedLimitMetresPerSecond = limit > 0.0 ? limit : 13.9,
                                                 .allowLaneChanges = lane.params.allowLaneChanges,
                                                 .allowUTurns = lane.params.allowUTurns,
                                                 .points = lane.points});
    }

    return sources;
}

// The mode, as the position log names it.
[[nodiscard]] const char* modeName(const raceengine::AgentMode mode)
{
    switch (mode)
    {
    case raceengine::AgentMode::Cruising:
        return "Cruising";
    case raceengine::AgentMode::Embodied:
        return "Embodied";
    case raceengine::AgentMode::Disturbed:
        return "Disturbed";
    case raceengine::AgentMode::PullingOver:
        return "PullingOver";
    case raceengine::AgentMode::Stopped:
        return "Stopped";
    case raceengine::AgentMode::Pursuing:
        return "Pursuing";
    case raceengine::AgentMode::External:
        return "External";
    }

    return "Unknown";
}

} // namespace

TrafficDirector::TrafficDirector(raceengine::Engine& engine, const TrafficNetwork& source,
                                 const TrafficSettings& chosen, std::optional<raceengine::NavMesh> ground) :
    engine(engine),
    settings(chosen),
    navmesh(std::move(ground))
{
    auto built = raceengine::buildLaneNetwork(laneSourcesFrom(source));
    if (!built)
    {
        raceengine::fail(built.error());
    }

    auto network = std::move(built).value();

    // What the derivation found, reported once. It is the only place anybody will ever see that a
    // lane loops or that two lanes are adjacent, because the export states neither — so a city whose
    // traffic all drives into a dead end is diagnosed from this line rather than from the seat.
    auto loops = std::size_t{0};
    auto links = std::size_t{0};
    auto pairings = std::size_t{0};
    auto pairedMetres = 0.0;

    for (const auto& lane : network.lanes)
    {
        loops += lane.loop ? 1 : 0;
        links += lane.successors.size();
        pairings += lane.neighbours.size();

        for (const auto& run : lane.neighbours)
        {
            pairedMetres += run.toMetres - run.fromMetres;
        }
    }

    engine.log().info("Traffic lanes: {} lanes, {:.1f} km, {} loop, {} join on, {} adjacency runs over {:.1f} km of lane",
                      network.lanes.size(), network.totalLengthMetres / 1000.0, loops, links, pairings,
                      pairedMetres / 1000.0);

    auto options = raceengine::TrafficPopulationOptions{};
    options.densityPerKilometre = settings.densityPerKilometre;
    options.maximumAgents = settings.maximumAgents;
    options.maximumDisturbed = settings.maximumDisturbed;
    options.embodyRadiusMetres = settings.embodyRadiusMetres;
    options.disembodyRadiusMetres = settings.disembodyRadiusMetres;
    options.bodyCount = settings.bodyCount;
    options.colourCount = settings.colourCount;
    options.seed = settings.seed;

    // The police: a stated body shape makes a stated fraction of the city patrol cars; none stated
    // leaves the draw exactly what it was.
    const auto policed = settings.policeBody != raceengine::noPoliceBody && settings.policeBody < settings.bodyCount;
    options.policeBody = policed ? settings.policeBody : raceengine::noPoliceBody;
    options.policeShare = settings.policeShare;

    population.emplace(std::move(network), options, raceengine::defaultDriverProfiles());
    population->seed();

    // The way to the player: the lanes, and the mesh where the track carries one. What the build found
    // is one line, because it is the only place anybody will see how much of the road stands on the
    // mesh — a lane that runs a metre and a half off it is a lane the ground layer cannot leave from.
    auto way = raceengine::PursuitRouter(population->network(), navmesh ? &*navmesh : nullptr);
    engine.log().info("Police routing: {} lane places ({} on the mesh), {} lane edges, ground layer {}, searched {}",
                      way.lanePlaceCount(), way.lanePlacesOnMesh(), way.laneEdgeCount(),
                      way.hasGround() ? "the navmesh" : "none (lanes alone)",
                      settings.routeOnWorker ? "on a worker thread" : "on the simulation thread, one a tick");

    if (settings.routeOnWorker)
    {
        routeWorker.emplace(std::move(way));
        pursuitDirector.setRouteService(&*routeWorker);
    }
    else
    {
        router.emplace(std::move(way));
        pursuitDirector.setRouter(&*router);
    }

    if (policed && settings.maximumFullModels > 0)
    {
        // The Charger, built once and copied into every slot, with its electronics on: a patrol car
        // has them and its driver is not the player.
        auto charger = raceengine::dodgeChargerPolice();
        if (!charger)
        {
            raceengine::fail(charger.error());
        }

        auto assists = raceengine::dodgeChargerPoliceAssists(charger.value());
        assists.antilock.enabled = true;
        assists.traction.mode = raceengine::TractionMode::Full;
        assists.cornering.enabled = true;

        const auto driveline = raceengine::dodgeChargerPoliceDriveline();

        policeCars.reserve(settings.maximumFullModels);
        for (auto index = std::size_t{0}; index < settings.maximumFullModels; index++)
        {
            policeCars.emplace_back(charger.value(), driveline, assists);
        }

        engine.log().info("Police: {} patrol cars in the city, up to {} of them the full vehicle model in a chase",
                          population->policeIds().size(), settings.maximumFullModels);
    }

    const auto counts = population->profileCounts();
    const auto& profiles = population->driverProfiles();

    auto mix = std::string();
    for (auto index = std::size_t{0}; index < counts.size() && index < profiles.size(); index++)
    {
        mix += (index == 0 ? "" : ", ") + std::to_string(counts[index]) + " " + profiles[index].name;
    }

    engine.log().info("Traffic: {} cars ({})", population->agents().size(), mix);

    if (!settings.logPath.empty())
    {
        trafficLog.emplace(settings.logPath);
        engine.log().info("Traffic log: {} {}",
                          trafficLog->open() ? "every car at 30 Hz and on every change, to" : "could not open",
                          settings.logPath);
    }
}

void TrafficDirector::tick(const double deltaTime, const double simulatedSeconds, const glm::dvec3& focusMetres,
                           const std::span<const raceengine::TrafficVehicle> vehicles,
                           const raceengine::PhysicsWorld& world, const PlayerFrame* player)
{
    RACEENGINE_ZONE_N("traffic director tick");

    nudges.clear();
    propReleaseScratch.clear();
    propVelocityScratch.clear();

    if (!population)
    {
        return;
    }

    tickIndex++;
    logFocusMetres = focusMetres;
    if (!vehicles.empty())
    {
        logPlayerVelocity = vehicles.front().velocityMetresPerSecond;
        logPlayerForward = vehicles.front().forward;
    }

    // The occupants the population cannot see for itself: the caller's cars first — the population
    // reads the player's heading off the front entry — and then every patrol car this class is
    // stepping as the full model. The population skips its `External` agents in its own occupant
    // pass on the understanding that they arrive here; on the second seat (2026-09-09) they did not,
    // and the traffic drove through every boxed patrol car as through an empty lane.
    vehicleScratch.assign(vehicles.begin(), vehicles.end());
    for (const auto& car : policeCars)
    {
        if (!car.live())
        {
            continue;
        }

        // A full model is a patrol car in a chase with its siren on: the traffic behind it pulls
        // aside, and the pursuit's corridor does not count it as traffic (docs/police-driving-brief.md).
        const auto pose = car.pose();
        vehicleScratch.push_back(raceengine::TrafficVehicle{.positionMetres = pose.originMetres,
                                                            .velocityMetresPerSecond = pose.velocityMetresPerSecond,
                                                            .forward = pose.orientation * glm::dvec3(0.0, 0.0, 1.0),
                                                            .lengthMetres = car.lengthMetres(),
                                                            .siren = true});
    }

    const auto& report = population->update(raceengine::TrafficUpdate{.deltaTimeSeconds = deltaTime,
                                                                      .simulatedSeconds = simulatedSeconds,
                                                                      .focusMetres = focusMetres,
                                                                      .vehicles = vehicleScratch},
                                            world);

    // The near tier's two map diagnostics, said when they first happen and each time they double,
    // so a lane that runs through a kerb or a wall is read off the log rather than off the seat.
    if (report.heldAsPoints > 0 && report.heldAsPoints >= 2 * reportedHeld)
    {
        reportedHeld = report.heldAsPoints;
        engine.log().info("Traffic: {} near cars held as points so far (no ground under a wheel, or the world inside "
                          "the box where their plan is)",
                          report.heldAsPoints);
    }

    if (report.dropped > 0 && report.dropped >= 2 * reportedDropped)
    {
        reportedDropped = report.dropped;
        engine.log().info("Traffic: {} bodies dropped so far (driven deep into geometry by their plan, or past {} m/s)",
                          report.dropped, 60);
    }

    // --- the police --------------------------------------------------------------------------------
    //
    // After the population, so the director reads this tick's poses; before the cars, so the aims it
    // writes are what the pursuing bodies and the full models drive at on this tick.
    if (player == nullptr || population->policeIds().empty())
    {
        return;
    }

    pursuitDirector.update(deltaTime, player->player, *population, world);

    manageFullModels(world);
    stepFullModels(deltaTime, *player, world);
    reportChase();
}

PoliceCar* TrafficDirector::fullModel(const std::uint32_t id)
{
    for (auto& car : policeCars)
    {
        if (car.live() && car.agent() == id)
        {
            return &car;
        }
    }

    return nullptr;
}

// Who is a full model. A unit whose car the population has taken back — the chase ended for it, or
// it was written off — frees its slot; a pursuing body takes a free slot in roster order, so the
// first six to join are the six with the model and a swarm's tail stays cheap.
void TrafficDirector::manageFullModels(const raceengine::PhysicsWorld& world)
{
    const auto agents = population->agents();

    for (auto& car : policeCars)
    {
        if (!car.live())
        {
            continue;
        }

        const auto id = car.agent();
        if (id >= agents.size() || agents[id].mode != raceengine::AgentMode::External)
        {
            pursuitDirector.setFullModel(id, false);
            car.release();
        }
    }

    for (const auto& unit : pursuitDirector.units())
    {
        if (unit.agent >= agents.size() || agents[unit.agent].mode != raceengine::AgentMode::Pursuing)
        {
            continue;
        }

        auto* free = static_cast<PoliceCar*>(nullptr);
        for (auto& car : policeCars)
        {
            if (!car.live())
            {
                free = &car;
                break;
            }
        }

        if (free == nullptr)
        {
            break;
        }

        if (!population->takeExternal(unit.agent))
        {
            continue;
        }

        const auto& agent = agents[unit.agent];
        free->place(unit.agent,
                    PoliceCarPose{.originMetres = agent.positionMetres,
                                  .orientation = agent.orientation,
                                  .velocityMetresPerSecond = agent.velocityMetresPerSecond,
                                  .rolledMetres = agent.rolledMetres},
                    world);
        pursuitDirector.setFullModel(unit.agent, true);
    }
}

// One tick of every full model: the director's driver, the traffic and the other patrol cars and
// the player as obstacles, the pose back to the population, the damage to the director. What a
// patrol car's solve did to the player is kept for the simulation to apply; what it did to traffic
// goes to the population the way the player's own does.
void TrafficDirector::stepFullModels(const double deltaTime, const PlayerFrame& player,
                                     const raceengine::PhysicsWorld& world)
{
    RACEENGINE_ZONE_N("police full models");

    for (auto& car : policeCars)
    {
        if (!car.live())
        {
            continue;
        }

        const auto id = car.agent();
        const auto* unit = pursuitDirector.unit(id);
        if (unit == nullptr)
        {
            pursuitDirector.setFullModel(id, false);
            car.release();

            continue;
        }

        const auto drive = pursuitDirector.drive(*unit, car.pursuitPose());
        const auto origin = car.pose().originMetres;

        policeObstacles.clear();
        population->obstaclesNear(origin, settings.obstacleRadiusMetres, policeObstacles);
        for (const auto& other : policeCars)
        {
            if (other.live() && other.agent() != id)
            {
                policeObstacles.push_back(other.obstacle());
            }
        }
        policeObstacles.push_back(player.obstacle);

        car.tick(deltaTime, drive, policeObstacles, world);

        const auto pose = car.pose();
        population->setExternalPose(id, pose.originMetres, pose.orientation, pose.velocityMetresPerSecond,
                                    pose.rolledMetres);
        pursuitDirector.reportUnitImpact(id, car.impactMetresPerSecond());

        // The far side of each hit this solve met: the player's share rides the nudge for the
        // simulation's ledger, another patrol car's goes to the director. The velocities the solve
        // settled on go to their owners whatever the closing speed was.
        for (const auto& hit : car.hits())
        {
            if (hit.obstacle == playerObstacle)
            {
                continue;
            }

            if (fullModel(hit.obstacle) != nullptr)
            {
                pursuitDirector.reportUnitImpact(hit.obstacle, hit.otherMetresPerSecond);
            }
        }

        for (const auto& body : car.contacts().bodies)
        {
            if (body.obstacle == playerObstacle)
            {
                auto share = 0.0;
                for (const auto& hit : car.hits())
                {
                    share += hit.obstacle == playerObstacle ? hit.otherMetresPerSecond : 0.0;
                }

                nudges.push_back(PlayerNudge{.deltaLinear = body.deltaLinear,
                                             .deltaAngular = body.deltaAngular,
                                             .impactMetresPerSecond = share});
            }
            else if (auto* other = fullModel(body.obstacle); other != nullptr)
            {
                other->nudge(body.deltaLinear, body.deltaAngular);
            }
        }

        // The population's own bodies take the nudge from this solve and are charged the closing
        // speed it met them at, once — never the solver's push-out, which wrote a cheap patrol car
        // off for a few ticks of overlap with a Charger's box (docs/police-pursuit-brief.md §10).
        population->applyContacts(car.contacts(), raceengine::ImpactLedger::FromHits);

        for (const auto& hit : car.hits())
        {
            if (hit.obstacle != playerObstacle && hit.obstacle != raceengine::noObstacle && fullModel(hit.obstacle) == nullptr)
            {
                population->chargeImpact(hit.obstacle, hit.otherMetresPerSecond);
            }
        }

        // The street furniture this solve touched, exactly as `Simulation::step` reads the player's
        // manifold: the anchors that gave way during the solve, and what the two-body solve gave each
        // prop. The simulation hands both to the world after every car has been stepped, release
        // first (docs/police-driving-brief.md §2.2).
        for (const auto& body : car.contacts().bodies)
        {
            if (body.prop == raceengine::noProp)
            {
                continue;
            }

            if (body.released)
            {
                propReleaseScratch.push_back(body.prop);
            }

            propVelocityScratch.push_back(
                raceengine::PropVelocity{.prop = body.prop, .linear = body.deltaLinear, .angular = body.deltaAngular});
        }
    }
}

std::span<const std::uint32_t> TrafficDirector::propReleases() const
{
    return propReleaseScratch;
}

std::span<const raceengine::PropVelocity> TrafficDirector::propVelocities() const
{
    return propVelocityScratch;
}

// The chase, on the log, at its transitions.
void TrafficDirector::reportChase()
{
    const auto& status = pursuitDirector.status();

    if (status.active && !reportedActive)
    {
        engine.log().info("Police: pursuit -- {} (felony level {} of 5, {} unit{})", raceengine::offenceName(status.offence),
                          status.level, status.units, status.units == 1 ? "" : "s");
    }
    else if (!status.active && reportedActive)
    {
        if (status.busted)
        {
            engine.log().info("Police: BUSTED. Press R to restart.");
        }
        else
        {
            engine.log().info("Police: they lost you (felony {:.2f}, {} pursuit{} so far, {} patrol car{} wrecked)",
                              status.felony, status.pursuitsStarted, status.pursuitsStarted == 1 ? "" : "s",
                              status.policeWrecked, status.policeWrecked == 1 ? "" : "s");
        }
    }

    if (status.active && status.level != reportedLevel)
    {
        engine.log().info("Police: felony level {} of 5{}", status.level,
                          status.swarm && !reportedSwarm ? " -- every patrol car on the map is coming" : "");
    }

    if (status.active && status.units != reportedUnits && reportedActive)
    {
        engine.log().info("Police: {} unit{} in the chase, {} as the full vehicle model, {} routing ({} on you, {} on "
                          "the radio's last position, {} searching)",
                          status.units, status.units == 1 ? "" : "s", status.fullModels, status.routingUnits,
                          status.nearUnits, status.farUnits, status.searchingUnits);
    }

    // The search (docs/pursuit-radio-brief.md, §1.3): said when it starts and when a sighting ends it.
    if (status.active && status.searching && !reportedSearching)
    {
        engine.log().info("Police: they lost sight of you -- {} unit{} searching from ({:.0f}, {:.0f}), the way you were "
                          "going first",
                          status.searchingUnits, status.searchingUnits == 1 ? "" : "s", status.searchCentreMetres.x,
                          status.searchCentreMetres.z);
    }
    else if (status.active && !status.searching && reportedSearching)
    {
        engine.log().info("Police: they have you in sight again after {:.0f} s of searching", status.searchSeconds);
    }

    // The routing's cost, said when it first happens and each time the worst route doubles. On the
    // worker it is the worker's time and no longer the tick's; on the simulation thread it is both.
    if (status.routesBuilt > 0 && status.routeWorstSeconds > 2.0 * reportedRouteWorstSeconds)
    {
        reportedRouteWorstSeconds = status.routeWorstSeconds;
        engine.log().info("Police routing: the longest route so far took {:.2f} ms {} ({} routes built, {} waiting)",
                          1000.0 * status.routeWorstSeconds,
                          settings.routeOnWorker ? "on the worker" : "on the simulation thread", status.routesBuilt,
                          status.routesPending);
    }

    if (status.wrecked && !reportedWrecked)
    {
        engine.log().info("Police: your car is a wreck (damage {:.0f} %)", 100.0 * status.playerDamage);
    }

    reportedActive = status.active;
    reportedLevel = status.level;
    reportedSwarm = status.swarm;
    reportedWrecked = status.wrecked;
    reportedSearching = status.active && status.searching;
    reportedUnits = status.units;
}

std::span<const PlayerNudge> TrafficDirector::playerNudges() const
{
    return nudges;
}

const raceengine::PursuitStatus& TrafficDirector::pursuit() const
{
    return pursuitDirector.status();
}

const raceengine::NavMesh* TrafficDirector::navMesh() const
{
    return navmesh ? &*navmesh : nullptr;
}

bool TrafficDirector::isPolice(const std::uint32_t id) const
{
    if (!population)
    {
        return false;
    }

    const auto agents = population->agents();

    return id < agents.size() && agents[id].police;
}

void TrafficDirector::setWantedLevel(const int level)
{
    if (!population || population->policeIds().empty())
    {
        return;
    }

    pursuitDirector.setLevel(level);

    const auto& status = pursuitDirector.status();
    engine.log().info("Police: wanted level set to {} of 5 (felony {:.2f}{})", status.level, status.felony,
                      status.swarm ? ", every patrol car on the map is coming" : "");
}

void TrafficDirector::restart()
{
    if (!population)
    {
        return;
    }

    for (auto& car : policeCars)
    {
        car.release();
    }

    pursuitDirector.reset();
    population->seed();
    nudges.clear();
    logRestart = true;

    reportedActive = false;
    reportedLevel = 0;
    reportedSwarm = false;
    reportedWrecked = false;
    reportedSearching = false;
    reportedUnits = 0;
    reportedRouteWorstSeconds = 0.0;

    publish();
}

void TrafficDirector::obstaclesNear(const glm::dvec3& pointMetres, std::vector<raceengine::DynamicObstacle>& into) const
{
    if (!population)
    {
        return;
    }

    population->obstaclesNear(pointMetres, settings.obstacleRadiusMetres, into);

    // The full models, which the population leaves out because their bodies are this class's, with
    // their real mass: a ram moves both cars.
    const auto radius = settings.obstacleRadiusMetres;
    for (const auto& car : policeCars)
    {
        if (car.live() && glm::length(car.pose().originMetres - pointMetres) <= radius)
        {
            into.push_back(car.obstacle());
        }
    }
}

void TrafficDirector::applyContacts(const raceengine::ContactManifold& manifold, const std::span<const BodyworkHit> hits)
{
    if (!population)
    {
        return;
    }

    // What the player's solve did to a full model is this class's to apply; and where that solve was
    // the one that met the closing speed, the patrol car's share of the hit is charged here, whether
    // or not the player meant it.
    for (const auto& body : manifold.bodies)
    {
        if (auto* car = fullModel(body.obstacle); car != nullptr)
        {
            car->nudge(body.deltaLinear, body.deltaAngular);
        }
    }

    for (const auto& hit : hits)
    {
        if (fullModel(hit.obstacle) != nullptr)
        {
            pursuitDirector.reportUnitImpact(hit.obstacle, hit.otherMetresPerSecond);
        }
    }

    // The population's bodies — traffic and the cheap patrol cars alike — take the nudge and are
    // charged the closing speed, once, the way the full models are.
    population->applyContacts(manifold, raceengine::ImpactLedger::FromHits);

    for (const auto& hit : hits)
    {
        if (hit.obstacle != playerObstacle && hit.obstacle != raceengine::noObstacle && fullModel(hit.obstacle) == nullptr)
        {
            population->chargeImpact(hit.obstacle, hit.otherMetresPerSecond);
        }
    }
}

void TrafficDirector::publish()
{
    if (!population)
    {
        return;
    }

    const auto agents = population->agents();

    scratch.clear();
    scratch.reserve(agents.size());

    for (const auto& agent : agents)
    {
        scratch.push_back(TrafficSnapshot{.positionMetres = agent.positionMetres,
                                          .orientation = agent.orientation,
                                          .rolledMetres = agent.rolledMetres,
                                          .id = agent.id,
                                          .body = agent.body,
                                          .colour = agent.colour,
                                          .disturbed = agent.mode != raceengine::AgentMode::Cruising &&
                                                       agent.mode != raceengine::AgentMode::Embodied,
                                          .velocityMetresPerSecond = agent.velocityMetresPerSecond,
                                          .accelerationMetresPerSecondSquared =
                                              agent.accelerationMetresPerSecondSquared,
                                          .police = agent.police,
                                          .siren = agent.siren});
    }

    if (auto held = std::unique_lock<std::mutex>(publication, std::try_to_lock); held.owns_lock())
    {
        published = scratch;
    }

    if (trafficLog)
    {
        writeLog();
    }
}

// The position log, at the end of the tick: after the population, the police and every car's solve,
// which is the state the next tick starts from and the one the frame draws. A row per car on every
// twelfth tick, and on any tick its mode, lane or body slot changed, it moved more than a metre, or
// it is past 45 m/s; the player and the report on the periodic ticks; a marker on a restart.
void TrafficDirector::writeLog()
{
    const auto agents = population->agents();
    const auto periodic = tickIndex % 12 == 0;

    if (logMemory.size() != agents.size())
    {
        logMemory.assign(agents.size(), LogMemory{});
    }

    if (logRestart)
    {
        logRestart = false;
        trafficLog->row(TrafficLogRow{.tick = tickIndex, .kind = 'x', .mode = "Restart"});

        for (auto& memory : logMemory)
        {
            memory.seen = false;
        }
    }

    const auto asIndex = [](const std::size_t value)
    {
        return value == raceengine::noSlot ? std::int32_t{-1} : static_cast<std::int32_t>(value);
    };

    for (auto index = std::size_t{0}; index < agents.size(); index++)
    {
        const auto& agent = agents[index];
        auto& memory = logMemory[index];

        const auto jump = memory.seen ? glm::distance(agent.positionMetres, memory.positionMetres) : 0.0;
        const auto speed = glm::length(agent.velocityMetresPerSecond);
        const auto changed = memory.seen && (agent.mode != memory.mode || agent.lane != memory.lane ||
                                             agent.slot != memory.slot || jump > 1.0 || speed > 45.0);

        if (periodic || changed || !memory.seen)
        {
            trafficLog->row(TrafficLogRow{.tick = tickIndex,
                                          .kind = changed ? 'e' : 't',
                                          .id = static_cast<std::int32_t>(agent.id),
                                          .mode = modeName(agent.mode),
                                          .lane = asIndex(agent.lane),
                                          .distanceMetres = agent.distanceMetres,
                                          .target = asIndex(agent.changeTarget),
                                          .progress = agent.changeProgress,
                                          .slot = asIndex(agent.slot),
                                          .lagMetres = agent.bodyLagMetres,
                                          .stillSeconds = agent.stillSeconds,
                                          .disturbedSeconds = agent.disturbedSeconds,
                                          .x = agent.positionMetres.x,
                                          .y = agent.positionMetres.y,
                                          .z = agent.positionMetres.z,
                                          .vx = agent.velocityMetresPerSecond.x,
                                          .vy = agent.velocityMetresPerSecond.y,
                                          .vz = agent.velocityMetresPerSecond.z,
                                          .fx = agent.heading.x,
                                          .fy = agent.heading.y,
                                          .fz = agent.heading.z,
                                          .jumpMetres = jump,
                                          .police = agent.police,
                                          .siren = agent.siren});
        }

        memory = LogMemory{.seen = true,
                           .mode = agent.mode,
                           .lane = agent.lane,
                           .slot = agent.slot,
                           .positionMetres = agent.positionMetres};
    }

    if (periodic)
    {
        trafficLog->row(TrafficLogRow{.tick = tickIndex,
                                      .kind = 'p',
                                      .mode = "Player",
                                      .x = logFocusMetres.x,
                                      .y = logFocusMetres.y,
                                      .z = logFocusMetres.z,
                                      .vx = logPlayerVelocity.x,
                                      .vy = logPlayerVelocity.y,
                                      .vz = logPlayerVelocity.z,
                                      .fx = logPlayerForward.x,
                                      .fy = logPlayerForward.y,
                                      .fz = logPlayerForward.z});

        const auto& report = population->report();
        trafficLog->row(TrafficLogRow{.tick = tickIndex,
                                      .kind = 'r',
                                      .mode = "Report",
                                      .lane = static_cast<std::int32_t>(report.cruising),
                                      .distanceMetres = static_cast<double>(report.embodied),
                                      .target = static_cast<std::int32_t>(report.disturbed),
                                      .progress = static_cast<double>(report.stopped),
                                      .slot = static_cast<std::int32_t>(report.pursuing),
                                      .lagMetres = static_cast<double>(report.external),
                                      .stillSeconds = static_cast<double>(report.recycled),
                                      .disturbedSeconds = static_cast<double>(report.refused),
                                      .x = static_cast<double>(report.heldAsPoints),
                                      .y = static_cast<double>(report.dropped),
                                      .z = static_cast<double>(report.promoted)});
    }

    trafficLog->flush();
}

void TrafficDirector::collect(std::vector<TrafficSnapshot>& into) const
{
    const auto guard = std::lock_guard<std::mutex>(publication);

    into = published;
}

std::size_t TrafficDirector::count() const
{
    return population ? population->agents().size() : 0;
}

} // namespace osr
