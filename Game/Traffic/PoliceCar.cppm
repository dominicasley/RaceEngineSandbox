module;

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <Profiling/RaceEngineProfile.hpp>

export module osr.game:PoliceCar;

import raceengine;

namespace osr
{

// A patrol car in a chase, driven as the full vehicle model (docs/police-pursuit-brief.md).
//
// **What this is and is not.** The pursuit director decides where every patrol car should be and how
// fast; the traffic population steps the cheap ones as rigid bodies on four spring rays. This is the
// other tier: the Dodge Charger's own `VehicleSetup`, driveline and electronics, stepped exactly the
// way the player's Golf is — `stepDriveline`, then `stepVehicle` against the road, the buildings,
// the traffic and the player's own car as an obstacle — so a patrol car that boxes the player in
// does it on tyres, springs and a limited-slip differential rather than on a box with a friction
// coefficient. The population is told the pose back every tick (`setExternalPose`) so the draw pool,
// the occupant list and the recycle rules go on seeing one car where there is one car.
//
// The driver is `PursuitDirector::drive`: pure pursuit at the director's aim, at the director's
// speed, through this car's own lock. Gears are chosen here, on engine speed, because the director
// does not know what a gearbox is.
//
// One of these costs about what the player's tick costs, so how many exist is the traffic director's
// budget (`TrafficSettings::maximumFullModels`) and the rest of a swarm stays in the cheap tier.

// The obstacle id the player's car is offered to a patrol car's solver under. Above every agent id
// the population can hand out, so a manifold body carrying it is never mistaken for traffic.
export inline constexpr std::uint32_t playerObstacle = 0xfffffffeu;

// What a car's bodywork took in one of its own solves, per far body, for the damage ledger: the
// velocity change a hit at the speed the two bodies met costs each of them, from the pair's masses
// — the world's is infinite, so the car takes the whole of it — and never from the impulse the
// solver applied. The impulse carries the position correction on every tick of a push-out and on
// both cars' solves of the same overlap, and the second seat (2026-09-09) read a patrol car that had
// climbed a kerb as a collision that went on for a dozen ticks with nobody moving. The solve that
// meets the closing speed is the one that charges both sides; the other car's solve on the same
// tick meets a pair already separating and charges nothing, which is what keeps one hit one hit.
export struct BodyworkHit
{
    // `raceengine::noObstacle` for the immovable world.
    std::uint32_t obstacle = raceengine::noObstacle;
    // The far body's index in the manifold this was read from.
    std::size_t body = 0;
    double approachMetresPerSecond = 0.0;
    // The velocity change on this car and on the far body, m/s.
    double selfMetresPerSecond = 0.0;
    double otherMetresPerSecond = 0.0;
};

// One entry per far body the manifold touched at a closing speed, appended; the caller clears.
export void bodyworkHits(const raceengine::ContactManifold& manifold, double selfMassKilograms,
                         std::vector<BodyworkHit>& into);

// A pose in the population's own terms: the body's origin on the road between the axles, the
// orientation, the velocity and how far the car has rolled.
export struct PoliceCarPose
{
    glm::dvec3 originMetres{0.0};
    glm::dquat orientation{1.0, 0.0, 0.0, 0.0};
    glm::dvec3 velocityMetresPerSecond{0.0};
    double rolledMetres = 0.0;
};

export class PoliceCar
{
public:
    PoliceCar(const raceengine::VehicleSetup& charger, const raceengine::DrivelineSetup& chargerDriveline,
              const raceengine::AssistSetup& chargerAssists);

    [[nodiscard]] bool live() const
    {
        return agentId != raceengine::noAgent;
    }

    [[nodiscard]] std::uint32_t agent() const
    {
        return agentId;
    }

    // Stand the model up on a population car's pose, rolling at its speed with the engine running and
    // a gear in. The mass ledger is asked of the model itself with one inert tick, the way the
    // player's car asks it, so the centre of mass is the model's and not a number typed here.
    void place(std::uint32_t id, const PoliceCarPose& pose, const raceengine::PhysicsWorld& world);
    void release();

    // One tick. `obstacles` is everything this car may hit that is not in the world: the traffic near
    // it, the other full models, and the player's car.
    void tick(double deltaTime, const raceengine::PursuitDrive& drive,
              std::span<const raceengine::DynamicObstacle> obstacles, const raceengine::PhysicsWorld& world);

    [[nodiscard]] PoliceCarPose pose() const;
    [[nodiscard]] raceengine::PursuitCarPose pursuitPose() const;

    // This car as the player's solver sees it, with its real mass, so a ram moves both cars.
    [[nodiscard]] raceengine::DynamicObstacle obstacle() const;

    // What this car's own solve touched this tick, with the impulses it settled on: the far side of
    // each body is the player's car, another full model, or traffic, by id.
    [[nodiscard]] const raceengine::ContactManifold& contacts() const
    {
        return lastStep.contacts;
    }

    // The velocity change this car's bodywork took in its own solve this tick, m/s, for the damage
    // model: the sum of `hits()`' own side. A hit somebody else's solve put on it arrives through
    // `nudge` and was charged by that solve.
    [[nodiscard]] double impactMetresPerSecond() const
    {
        return impact;
    }

    // What this car's solve hit this tick, each with the far side's share of the hit for whoever
    // owns that body to charge.
    [[nodiscard]] std::span<const BodyworkHit> hits() const
    {
        return bodyworkHitsThisTick;
    }

    // The body box's length, for the lane occupant this car is entered as.
    [[nodiscard]] double lengthMetres() const
    {
        return 2.0 * setup.body.halfExtents.z;
    }

    // What another solve did to this body: the player's car hit it.
    void nudge(const glm::dvec3& deltaLinear, const glm::dvec3& deltaAngular);

private:
    [[nodiscard]] std::int32_t gearForSpeed(double alongMetresPerSecond) const;

    raceengine::VehicleSetup setup;
    raceengine::DrivelineSetup driveline;
    raceengine::AssistSetup assists;
    raceengine::AssistState assistState{};
    raceengine::VehicleState state{};
    raceengine::DrivelineState drivelineState{};
    raceengine::VehicleStep lastStep{};
    raceengine::TelemetryFrame lastTelemetry{};
    raceengine::DrivelineTorques lastDrivelineTorques{};
    std::array<double, raceengine::cornerCount> lastRoadTorques{};

    // The mass ledger the model reported when it was placed, kept apart from the state so the
    // obstacle this car offers is right on the tick before its first step too.
    double mass = 1.0;
    glm::dvec3 centreOfMass{0.0};
    glm::dmat3 inverseInertiaBody{1.0};

    std::uint32_t agentId = raceengine::noAgent;
    std::int32_t gear = 1;
    double rolled = 0.0;
    double impact = 0.0;
    std::vector<BodyworkHit> bodyworkHitsThisTick;
    double wheelRadius = 0.35;
    double wheelbase = 3.0;
};

} // namespace osr

namespace osr
{

namespace
{

constexpr auto forwardAxis = glm::dvec3(0.0, 0.0, 1.0);

// car.ini STEER_LOCK 450 through STEER_RATIO 17: the road wheel's angle at a full steering demand.
// The director's driver turns a curvature into a demand against this.
constexpr auto chargerRoadWheelLock = (450.0 / 17.0) * (3.14159265358979323846 / 180.0);

// Where the driver shifts, as fractions of the limiter: up near it, down a little above idle, so a
// patrol car pulling out of a box is in first and one on the parkway is in fifth.
constexpr auto upshiftFraction = 0.92;
constexpr auto downshiftFraction = 0.35;

} // namespace

void bodyworkHits(const raceengine::ContactManifold& manifold, const double selfMassKilograms,
                  std::vector<BodyworkHit>& into)
{
    const auto selfInverseMass = selfMassKilograms > 0.0 ? 1.0 / selfMassKilograms : 0.0;
    const auto first = into.size();

    // One entry per far body, at the fastest of its points: four points on one face are one hit.
    for (const auto& point : manifold.points)
    {
        if (point.approachSpeed <= 0.0)
        {
            continue;
        }

        auto* entry = static_cast<BodyworkHit*>(nullptr);
        for (auto index = first; index < into.size(); index++)
        {
            if (into[index].body == point.body)
            {
                entry = &into[index];
                break;
            }
        }

        if (entry == nullptr)
        {
            const auto obstacle =
                point.body < manifold.bodies.size() ? manifold.bodies[point.body].obstacle : raceengine::noObstacle;
            into.push_back(BodyworkHit{.obstacle = obstacle, .body = point.body});
            entry = &into.back();
        }

        entry->approachMetresPerSecond = std::max(entry->approachMetresPerSecond, point.approachSpeed);
    }

    // Split by the pair's masses, translation only: the closing speed removed at the effective mass
    // of the pair, each side's share by its own inverse mass. Against the immovable world the car
    // takes all of it.
    for (auto index = first; index < into.size(); index++)
    {
        auto& hit = into[index];
        const auto otherInverseMass = hit.body < manifold.bodies.size() ? manifold.bodies[hit.body].inverseMass : 0.0;
        const auto pairInverseMass = selfInverseMass + otherInverseMass;
        const auto effectiveMass = pairInverseMass > 0.0 ? 1.0 / pairInverseMass : 0.0;

        hit.selfMetresPerSecond = hit.approachMetresPerSecond * effectiveMass * selfInverseMass;
        hit.otherMetresPerSecond = hit.approachMetresPerSecond * effectiveMass * otherInverseMass;
    }
}

PoliceCar::PoliceCar(const raceengine::VehicleSetup& charger, const raceengine::DrivelineSetup& chargerDriveline,
                     const raceengine::AssistSetup& chargerAssists) :
    setup(charger),
    driveline(chargerDriveline),
    assists(chargerAssists)
{
    const auto& front = setup.corners[static_cast<std::size_t>(raceengine::Corner::FrontLeft)].hardpoints;
    const auto& rear = setup.corners[static_cast<std::size_t>(raceengine::Corner::RearLeft)].hardpoints;

    wheelRadius = front.wheelRadius;
    wheelbase = std::abs(front.wheelCentre.z - rear.wheelCentre.z);
}

std::int32_t PoliceCar::gearForSpeed(const double alongMetresPerSecond) const
{
    // The gear whose ratio puts the engine nearest the middle of its band at this road speed, so a
    // car placed rolling is placed in the gear a driver would be in.
    const auto axleSpeed = std::abs(alongMetresPerSecond) / wheelRadius;
    const auto target = 0.5 * (driveline.engine.idleSpeed + driveline.engine.limiterSpeed);

    auto best = std::int32_t{1};
    auto bestError = std::numeric_limits<double>::max();

    for (auto index = std::size_t{0}; index < driveline.gearbox.ratios.size(); index++)
    {
        const auto engineSpeed = axleSpeed * driveline.gearbox.ratios[index] * driveline.gearbox.finalDrive;
        const auto error = std::abs(engineSpeed - target);

        if (error < bestError)
        {
            bestError = error;
            best = static_cast<std::int32_t>(index + 1);
        }
    }

    return best;
}

void PoliceCar::place(const std::uint32_t id, const PoliceCarPose& pose, const raceengine::PhysicsWorld& world)
{
    agentId = id;
    rolled = pose.rolledMetres;
    impact = 0.0;
    assistState = raceengine::AssistState{};
    lastStep = raceengine::VehicleStep{};
    lastTelemetry = raceengine::TelemetryFrame{};
    lastDrivelineTorques = raceengine::DrivelineTorques{};
    lastRoadTorques = {};

    // The ledger, from the model: one inert tick a metre over the road, the way `SimulatedCar` asks.
    state = raceengine::VehicleState{};
    state.chassis.orientation = pose.orientation;
    state.chassis.position = pose.originMetres + glm::dvec3(0.0, 1.0, 0.0);
    if (const auto probed = raceengine::stepVehicle(setup, state, {}, raceengine::noDriveTorque, world, 1e-6); !probed)
    {
        raceengine::fail(probed.error());
    }

    mass = state.chassis.mass;
    centreOfMass = state.chassis.centreOfMass;
    inverseInertiaBody = state.chassis.inverseInertia;

    const auto along = glm::dot(pose.velocityMetresPerSecond, pose.orientation * forwardAxis);

    state = raceengine::VehicleState{};
    state.chassis.orientation = pose.orientation;
    state.chassis.position = pose.originMetres + pose.orientation * centreOfMass;
    state.chassis.linearVelocity = pose.velocityMetresPerSecond;

    for (auto& corner : state.corners)
    {
        corner.wheelSpeed = along / wheelRadius;
    }

    // The thermal models are off on this car; the seeds keep the state a sensible one all the same.
    raceengine::seedTyreTemperatures(state, 25.0);
    raceengine::seedDiscTemperatures(state, 20.0);

    // Placed rolling: the shaft slaved to the wheels, the engine running and turning at the speed the
    // gear it is placed in asks of it, so the first tick is a car driving and not a car being
    // push-started.
    gear = gearForSpeed(along);

    drivelineState = raceengine::DrivelineState{};
    raceengine::placeDriveline(driveline, drivelineState, along / wheelRadius);
    raceengine::startEngine(driveline, drivelineState);

    drivelineState.gear = gear;
    drivelineState.targetGear = gear;
    drivelineState.shiftFrom = gear;
    drivelineState.engineSpeed =
        std::max(driveline.engine.idleSpeed, std::abs(along) / wheelRadius *
                                                 driveline.gearbox.ratios[static_cast<std::size_t>(gear - 1)] *
                                                 driveline.gearbox.finalDrive);
}

void PoliceCar::release()
{
    agentId = raceengine::noAgent;
    impact = 0.0;
    lastStep = raceengine::VehicleStep{};
}

void PoliceCar::tick(const double deltaTime, const raceengine::PursuitDrive& drive,
                     const std::span<const raceengine::DynamicObstacle> obstacles, const raceengine::PhysicsWorld& world)
{
    RACEENGINE_ZONE_N("police car tick");

    if (!live())
    {
        return;
    }

    // The gears, on engine speed, and only between shifts: a demand changed mid-shift retargets the
    // shift machine, which is not what a driver does. Reverse on the director's word, first the
    // moment it is withdrawn.
    if (drive.reverse)
    {
        gear = -1;
    }
    else if (gear < 1)
    {
        gear = 1;
    }
    else if (drivelineState.shiftPhase == raceengine::ShiftPhase::Engaged && drivelineState.gear == gear)
    {
        const auto top = static_cast<std::int32_t>(driveline.gearbox.ratios.size());

        if (drivelineState.engineSpeed > upshiftFraction * driveline.engine.limiterSpeed && gear < top)
        {
            gear++;
        }
        else if (drivelineState.engineSpeed < downshiftFraction * driveline.engine.limiterSpeed && gear > 1)
        {
            gear--;
        }
    }

    auto input = raceengine::VehicleInput{};
    input.steering = std::clamp(drive.steering, -1.0, 1.0);
    input.throttle = std::clamp(drive.throttle, 0.0, 1.0);
    input.brake = std::clamp(drive.brake, 0.0, 1.0);
    input.gear = gear;

    const auto inertias = raceengine::wheelInertias(setup);

    auto speeds = std::array<double, raceengine::cornerCount>{};
    for (auto index = std::size_t{0}; index < raceengine::cornerCount; index++)
    {
        speeds[index] = state.corners[index].wheelSpeed;
    }

    // The electronics, on the player's car's own terms and in its order: the sensors a car has on
    // its bus, last tick's driveline torque, the pedal as the hydraulics made it.
    auto sensors = raceengine::AssistSensors{};
    sensors.wheelSpeeds = speeds;
    sensors.yawRate = lastTelemetry.yawRate;
    sensors.lateralAcceleration = lastTelemetry.acceleration.x;
    sensors.steeringWheelAngle = lastTelemetry.steeringWheelAngle;
    sensors.driveTorque = lastDrivelineTorques.wheel;
    sensors.driveTorqueKnown = true;

    // No electronics while backing up: the anti-lock and traction units read wheel speeds that were
    // never calibrated for a car going backwards, and a patrol car reversing off a kerb wants its
    // pedals as they are.
    const auto assistCommand =
        drive.reverse ? raceengine::AssistOutput{}
                      : raceengine::updateAssists(assists, assistState, sensors,
                                                  {.brake = input.brake, .throttle = input.throttle},
                                                  raceengine::brakeCircuitPressures(setup, input.brake), deltaTime);

    input.throttle *= assistCommand.throttleScale;

    const auto driven =
        raceengine::stepDriveline(driveline, drivelineState, speeds, inertias, lastRoadTorques, input, deltaTime);
    if (!driven)
    {
        raceengine::fail(driven.error());
    }

    auto stepped = raceengine::stepVehicle(setup, state, input, driven->wheel, world, deltaTime, assistCommand.brakes,
                                           raceengine::AmbientConditions{}, obstacles);
    if (!stepped)
    {
        raceengine::fail(stepped.error());
    }

    lastRoadTorques = raceengine::roadTorques(stepped.value());
    lastTelemetry = stepped->telemetry;
    lastDrivelineTorques = driven.value();
    lastStep = std::move(stepped).value();

    const auto along = glm::dot(state.chassis.linearVelocity, state.chassis.orientation * forwardAxis);
    rolled += std::abs(along) * deltaTime;

    // What the bodywork took in this solve, per body it met at a closing speed, as this car's share
    // of the velocity change. The road holds the car up through the tyres and not through this
    // manifold, so a car driving along reads zero here and a car into a wall reads the hit — once,
    // on the tick it arrives, and not again on every tick the solver spends pushing it back out.
    bodyworkHitsThisTick.clear();
    bodyworkHits(lastStep.contacts, mass, bodyworkHitsThisTick);

    impact = 0.0;
    for (const auto& hit : bodyworkHitsThisTick)
    {
        impact += hit.selfMetresPerSecond;
    }
}

PoliceCarPose PoliceCar::pose() const
{
    const auto& chassis = state.chassis;

    return PoliceCarPose{.originMetres = chassis.position - chassis.orientation * centreOfMass,
                         .orientation = chassis.orientation,
                         .velocityMetresPerSecond = chassis.linearVelocity,
                         .rolledMetres = rolled};
}

raceengine::PursuitCarPose PoliceCar::pursuitPose() const
{
    const auto& chassis = state.chassis;

    return raceengine::PursuitCarPose{.positionMetres = chassis.position - chassis.orientation * centreOfMass,
                                      .orientation = chassis.orientation,
                                      .velocityMetresPerSecond = chassis.linearVelocity,
                                      .wheelbaseMetres = wheelbase,
                                      .lockRadians = chargerRoadWheelLock,
                                      .corneringLimitG = 0.85};
}

raceengine::DynamicObstacle PoliceCar::obstacle() const
{
    const auto& chassis = state.chassis;
    const auto rotation = glm::mat3_cast(chassis.orientation);
    // The box is stated about the body's origin; the chassis position is the centre of mass.
    const auto origin = chassis.position - chassis.orientation * centreOfMass;

    return raceengine::DynamicObstacle{.id = agentId,
                                       .centre = origin + chassis.orientation * setup.body.centre,
                                       .orientation = chassis.orientation,
                                       .halfExtents = setup.body.halfExtents,
                                       .centreOfMass = chassis.position,
                                       .linearVelocity = chassis.linearVelocity,
                                       .angularVelocity = raceengine::angularVelocity(chassis),
                                       .inverseMass = mass > 0.0 ? 1.0 / mass : 0.0,
                                       .inverseInertia = rotation * inverseInertiaBody * glm::transpose(rotation)};
}

void PoliceCar::nudge(const glm::dvec3& deltaLinear, const glm::dvec3& deltaAngular)
{
    if (!live())
    {
        return;
    }

    state.chassis.linearVelocity += deltaLinear;
    raceengine::setAngularVelocity(state.chassis, raceengine::angularVelocity(state.chassis) + deltaAngular);
}

} // namespace osr
