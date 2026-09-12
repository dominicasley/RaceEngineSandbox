module;

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

export module osr.game:PoliceLog;

import :DiagnosticLog;

namespace osr
{

// The police position log, `OSR_POLICE_LOG=<file>` — a diagnostic (2026-09-12) for a chase seen going
// wrong from the seat: every unit in the chase, its pose and every decision the pursuit director made
// for it — the aim it steers at, the station it holds, the goal its route goes to, the speed it was
// asked for and what capped it, and every state flag (routing, reversing, the ram, the
// block, the three-point turn, the lane shift, the corridor) — written from the simulation thread on
// every twelfth tick (30 Hz) and on every tick a flag, the role or the aim changed by more than a
// metre, so a unit's whole history stands in the file at 30 Hz with every transition on the tick it
// happened. The player and the chase's status ride the same file on the periodic ticks; a unit that
// leaves the chase writes one last row; a restart writes a marker. The file is CSV with one header
// line; the comment line under it says what the special row kinds carry in the unit columns.
//
// Written by a thread of its own (`DiagnosticLog`): the simulation thread only formats into a string
// and hands the block over once a tick. About 400 bytes a row: a six-unit chase is ~4 MB a minute, a
// swarm of 26 about 19.

export struct PoliceLogRow
{
    std::uint64_t tick = 0;
    // 'u' a unit on the periodic tick, 'e' a unit whose state changed on this tick, 'l' a unit that
    // left the chase, 'p' the player, 's' the chase's status, 'x' a restart.
    char kind = 'u';
    std::int32_t id = -1;
    const char* role = "";
    // The population's mode for the car: Pursuing (the cheap body) or External (the full model).
    const char* mode = "";

    // The flags, as integers so the status row can carry its counts in the same columns.
    std::int32_t full = 0;
    std::int32_t sighted = 0;
    std::int32_t nearPlayer = 0;
    std::int32_t searching = 0;
    std::int32_t routing = 0;
    // 0 no route, 1 a route through the graph, 2 the straight line, 3 blocked (the last resort).
    std::int32_t routeKind = 0;
    std::int32_t routePending = 0;
    std::int32_t reversing = 0;
    std::int32_t toStation = 0;
    std::int32_t ramming = 0;
    std::int32_t blocking = 0;
    std::int32_t turnPhase = 0;
    std::int32_t turnSide = 0;
    std::int32_t shifting = 0;
    std::int32_t shiftSide = 0;
    // The shift's offset as the aim carries it, metres left of the heading (§13's blend).
    double shiftAppliedMetres = 0.0;
    std::int32_t lineClear = 0;
    std::int32_t corridorHit = 0;
    std::int32_t corridorBlocked = 0;
    std::int32_t trafficAhead = 0;

    // The pose: position, velocity, heading (the player's forward on its row).
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    double vx = 0.0;
    double vy = 0.0;
    double vz = 0.0;
    double fx = 0.0;
    double fy = 0.0;
    double fz = 0.0;
    // The targets: the aim the wheel steers at, the station held in the player's frame, the goal the
    // route goes to, and the block point.
    double ax = 0.0;
    double ay = 0.0;
    double az = 0.0;
    double sx = 0.0;
    double sy = 0.0;
    double sz = 0.0;
    double gx = 0.0;
    double gy = 0.0;
    double gz = 0.0;
    double bx = 0.0;
    double by = 0.0;
    double bz = 0.0;

    double wantedMetresPerSecond = 0.0;
    double capMetresPerSecond = 0.0;
    double speedMetresPerSecond = 0.0;
    double distanceMetres = 0.0;
    double damage = 0.0;
    double stuckSeconds = 0.0;
    double routeLengthMetres = 0.0;
    double routeAtMetres = 0.0;
    double routeAgeSeconds = 0.0;
    double curvatureAhead = 0.0;
    double trafficGapMetres = 0.0;
    double trafficSpeedMetresPerSecond = 0.0;
    double corridorReachMetres = 0.0;
    double corridorHitMetres = 0.0;
    double roomLeftMetres = 0.0;
    double roomRightMetres = 0.0;
    // The full model's pedals and wheel this tick; zero on a cheap body.
    double steering = 0.0;
    double throttle = 0.0;
    double brake = 0.0;
    std::int32_t reverseGear = 0;
    // How far the unit moved since the tick before, metres.
    double jumpMetres = 0.0;
};

export class PoliceLog
{
public:
    explicit PoliceLog(const std::string& path);

    [[nodiscard]] bool open() const;

    // Simulation thread. `row` formats into the tick's block; `flush` hands the block to the writer.
    void row(const PoliceLogRow& entry);
    void flush();

private:
    DiagnosticLog file;
};

} // namespace osr

namespace osr
{

namespace
{

constexpr auto policeLogHeader = std::string_view(
    "tick,kind,id,role,mode,full,sighted,near,searching,routing,routeKind,routePending,reversing,toStation,"
    "ramming,blocking,turnPhase,turnSide,shifting,shiftSide,shiftApplied,lineClear,corridorHit,"
    "corridorBlocked,trafficAhead,x,y,z,vx,vy,vz,fx,fy,fz,ax,ay,az,sx,sy,sz,gx,gy,gz,bx,by,bz,wanted,cap,"
    "speed,dist,damage,stuck,routeLen,routeAt,routeAge,curv,gap,gapSpeed,corridorReach,corridorAt,roomL,"
    "roomR,steer,throttle,brake,reverseGear,jump\n"
    "# kind p: the player -- x y z its position, vx vy vz its velocity, fx fy fz its forward. kind s: the "
    "chase's status -- id=units mode=offence full=active sighted=observed near=nearUnits "
    "searching=searchingUnits routing=routingUnits routeKind=level routePending=routesPending "
    "reversing=swarm toStation=busted ramming=searching blocking=broadcasts turnPhase=fullModels "
    "shifting=wrecked shiftSide=sightedUnits x y z=broadcast ax ay az=searchCentre sx sy sz=nearestUnit wanted=felony "
    "cap=searchRadius speed=reportAge dist=nearestUnitMetres damage=playerDamage stuck=unobservedSeconds "
    "routeLen=searchSeconds routeAt=overspeed. kind l: a unit that left the chase this tick, at its last "
    "logged position. kind x: the chase was forgotten and the city reseeded on this tick.\n");

} // namespace

PoliceLog::PoliceLog(const std::string& path) : file(path, policeLogHeader)
{
}

bool PoliceLog::open() const
{
    return file.open();
}

void PoliceLog::row(const PoliceLogRow& entry)
{
    if (!file.open())
    {
        return;
    }

    char line[1024];

    const auto written = std::snprintf(
        line, sizeof line,
        "%llu,%c,%d,%s,%s,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%.3f,%d,%d,%d,%d,"
        "%.3f,%.3f,%.3f,%.2f,%.2f,%.2f,%.3f,%.3f,%.3f,"
        "%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,"
        "%.2f,%.2f,%.2f,%.2f,%.3f,%.2f,%.1f,%.1f,%.2f,%.4f,%.1f,%.2f,%.1f,%.1f,%.1f,%.1f,"
        "%.3f,%.3f,%.3f,%d,%.3f\n",
        static_cast<unsigned long long>(entry.tick), entry.kind, entry.id, entry.role, entry.mode, entry.full,
        entry.sighted, entry.nearPlayer, entry.searching, entry.routing, entry.routeKind, entry.routePending,
        entry.reversing, entry.toStation, entry.ramming, entry.blocking, entry.turnPhase, entry.turnSide,
        entry.shifting, entry.shiftSide, entry.shiftAppliedMetres, entry.lineClear, entry.corridorHit,
        entry.corridorBlocked, entry.trafficAhead, entry.x, entry.y, entry.z, entry.vx, entry.vy, entry.vz,
        entry.fx, entry.fy, entry.fz, entry.ax, entry.ay, entry.az, entry.sx, entry.sy, entry.sz, entry.gx,
        entry.gy, entry.gz, entry.bx, entry.by, entry.bz, entry.wantedMetresPerSecond, entry.capMetresPerSecond,
        entry.speedMetresPerSecond, entry.distanceMetres, entry.damage, entry.stuckSeconds,
        entry.routeLengthMetres, entry.routeAtMetres, entry.routeAgeSeconds, entry.curvatureAhead,
        entry.trafficGapMetres, entry.trafficSpeedMetresPerSecond, entry.corridorReachMetres,
        entry.corridorHitMetres, entry.roomLeftMetres, entry.roomRightMetres, entry.steering, entry.throttle,
        entry.brake, entry.reverseGear, entry.jumpMetres);

    if (written > 0)
    {
        file.line(std::string_view(line, static_cast<std::size_t>(written) < sizeof line
                                             ? static_cast<std::size_t>(written)
                                             : sizeof line - 1));
    }
}

void PoliceLog::flush()
{
    file.flush();
}

} // namespace osr
