module;

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <glm/glm.hpp>

export module osr.game:RaceTrack;

import :ColliderManifest;
import :Options;

import raceengine;

namespace osr
{

// A circuit, as physics reads it: one glTF per consumer, and a manifest beside them.
//
// **Two tracks are carried and `OSR_TRACK` chooses between them**, because a track is data rather
// than code. Everything that differs between two of them is a field of `TrackDefinition` below, and
// `CircuitScene` reads one of those without knowing which — the day a third arrives it is one entry
// here and one word in `Options.cppm`, and nothing in the scene moves.
//
// Each track is two exports, converted out of Assetto Corsa content by
// /home/dominic-asley/dev/ac-car-data, already in world space and already in **metres**. The
// renderer loads the visual export as an ordinary model, and the collision mesh below is read out of
// that *same loaded model* for the physics export rather than out of a second parse of the same
// bytes — which is the whole reason the two cannot drift: a mismatch between what is drawn and what
// is driven on would read as the car floating or sinking, and there is no second source for it to
// disagree with.
//
// The manifest beside each pair — `rt_bathurst_track.json` and `gcp/physics_manifest.json`, one
// schema — is the provenance for every number in this file: it is what the surface tables and the
// grid slots were read off, and both are carried into the repository so those numbers can be
// audited rather than believed.

// One `surfaces.ini` entry, keyed the way Assetto Corsa keys them.
//
// The friction figures are AC's own and are used *as* the grip multiplier, because that is what
// they already are in AC: a per-surface scale on the tyre's own grip, with a racing surface at
// very nearly one. Nothing here is quantised onto `SurfaceKind` — a road at 0.98 and a pit lane at
// 0.95 are different numbers and stay different numbers; the kind is a label for whatever one day
// wants to know that a gravel trap is not a road.
export struct TrackSurface
{
    std::string_view key;
    double friction = 1.0;
    double damping = 0.0;
    // AC's SIN_HEIGHT: the amplitude of the fine ripple it adds under a car on that surface, which
    // is exactly what `SurfaceMaterial::bumpiness` means.
    double bumpiness = 0.0;
    raceengine::SurfaceKind kind = raceengine::SurfaceKind::Tarmac;
};

// An authored start box: where a car is put and which way it points. Read from the `AC_START_n`
// dummies through the manifest, and already dropped onto the collision mesh — AC's own boxes float
// one to three metres above the road because the game drops cars onto them, so what is carried here
// is the manifest's `position_on_surface` and not its raw `position`.
//
// This is the spawn and the AI line is not, which is a trap worth stating where it can be read: on
// Bathurst the first point of `fast_lane.ai` is a *racing* line hugging the exit of the first
// corner, 1.04 m from the right edge of an 11 m road, and its height is the recording car's
// reference height rather than the tarmac — a median 0.79 m of thin air.
export struct GridSlot
{
    glm::dvec3 position{0.0};
    // Degrees, right-handed about +y, taking +z onto the heading — the same convention the vehicle's
    // body frame uses, so it becomes an orientation with no axis remap.
    double yaw = 0.0;
};

// Everything that is a property of *which* circuit this is, so that the scene reading it has no
// opinion about which one it got. Positions are metres, the units both manifests and all of physics
// are stated in; the scene converts at its own seam.
export struct TrackDefinition
{
    // What `OSR_TRACK` names it and what a log line calls it.
    std::string_view id;
    std::string_view name;

    // Two exports of one circuit, and each is read by exactly one consumer. The visual model is the
    // scenery — buildings, trees, the mount — and only the renderer touches it; the physics model is
    // the drivable surfaces under AC's `<digit><SURFACE>` naming, and only the collision reader
    // touches it. Splitting them is what lets the picture carry hundreds of primitives of scenery
    // without the BVH having to wade through a single triangle of it, and lets the surfaces stay
    // exact while the scenery is re-exported at will.
    std::string_view visualAsset;
    std::string_view physicsAsset;

    // The collider exports beside those two, or empty where a circuit has none — and Mount Panorama
    // has none, which is the ordinary case rather than a gap: a circuit's barriers are authored
    // collision and are already in its physics export.
    //
    // **Everything in these two files is derived rather than authored.** Assetto Corsa ships no
    // building collision for this map at all — the facades are driven through in AC itself — so
    // `~/dev/ac-car-data` hulls the *drawn* geometry and says so on every node it writes. That is
    // why they are a separate pair of files from `physicsAsset` and not merged into it: the drivable
    // surfaces are the author's and these are ours, and the day the derivation is re-run only these
    // two move.
    //
    // The prop file carries both the 176 static bodies and the 3536 break-away ones, told apart by
    // their material — `collider_static` against `collider_dynamic`. Only the static half is loaded
    // today; the other half is `docs/world-colliders-brief.md` stage 2, and until it lands a bin is
    // still driven through rather than being an immovable steel bin.
    std::string_view buildingColliderAsset;
    std::string_view propColliderAsset;
    // The drawn half of the props, one node per body, its vertices local to the same origin its
    // hulls are. Empty where there are no props.
    //
    // **It is a separate file from `visualAsset` and that is load-bearing rather than tidy.** These
    // objects move once they are hit, and geometry that moves cannot live inside a scenery batch of
    // one mesh per material — so the exporter takes them out of the city and writes them here, and
    // the city is re-exported without them. Draw both and every prop is drawn twice, as two
    // identical co-planar shells that shimmer.
    std::string_view propVisualAsset;

    // The manifest beside them, which is where every number a hull cannot state lives: a prop's
    // mass, its inertia tensor and the two loads its base gives way at. Empty where the pair above
    // is empty; a track that states collider geometry and no manifest gets its static half and no
    // breakable props, which is stage 1 of `docs/world-colliders-brief.md` and is a legitimate world.
    std::string_view colliderManifestAsset;

    // The traffic network export beside those two, or empty where a circuit has none. It is CSP's
    // lane graph — a point list per lane, authored at road level — and this game reads it for two
    // things: where the city's local light probes stand today, and where traffic drives tomorrow.
    // Empty is not a gap to fill in later: Mount Panorama is a circuit and has no traffic lanes,
    // and a scene that finds nothing here simply places no street probes.
    std::string_view trafficAsset;

    // Which of the render rig's shaders draws the visual export, and it is a property of *that file*
    // rather than of the scene that shows it — which is why it is named here beside the asset.
    //
    // **It follows how the asset was exported and there is no third answer.** `track.py visual
    // --ac-materials` writes each material's authored ambient, diffuse, specular and Blinn-Phong
    // exponent into `extras.blinn_phong`, and `"blinn-phong"` shades from exactly that; exported
    // without the flag, the same tool converts to metalness-roughness by *inference* and writes no
    // such block, and the file then describes itself in the model `"pbr"` reads. Draw one through
    // the other's shader and it is wrong in a way that looks like a lighting bug: a blinn-phong
    // asset under "pbr" loses its diffuse colour wherever the metalness was inferred high, and a
    // metalness-roughness asset under "blinn-phong" is lit uniformly by its own ambient term.
    std::string_view visualShader;

    // The surface table in the manifest's own index order, which is the order a triangle's surface
    // index means, and the authored start boxes in theirs.
    std::span<const TrackSurface> surfaces;
    std::span<const GridSlot> grid;

    // Where the one global light probe stands, in metres. A probe records what the indirect light in
    // this world *is*, so it belongs where the car drives rather than at a fixed height above an
    // origin: above open ground on a circuit, and down among the buildings in a city, because a
    // street canyon's bounce is the thing being photographed.
    glm::dvec3 lightProbeMetres{0.0};

    // Where the quoted fog density is quoted at, in metres — the ground this circuit stands on.
    // Bathurst's pit straight is 35 m above its own origin; a city laid out about sea level is at
    // nothing. A layer quoted at the wrong height is either buried or floating.
    double fogBaseHeightMetres = 0.0;

    // Where `OSR_CAMERA=fixed` stands when no pose is stated, and which way it looks. Metres and
    // radians — radians because `FPSCameraController` takes them, and stating them converted would
    // make "leave it unset" mean a different view from the one this shipped with. Zero yaw looks
    // along positive z.
    glm::dvec3 viewpointMetres{0.0};
    double viewpointYawRadians = 0.0;
    double viewpointPitchRadians = 0.0;
};

// Mount Panorama's surfaces, from `rt_bathurst_track.json`. LINES has no triangles in this track and
// is kept for that reason: dropping an empty surface would shift every index after it away from the
// document that explains them.
//
// WALL is the one entry AC does not state, because a wall is collision geometry rather than a
// surface a car drives on — it has no `surfaces.ini` line at all. It takes PITLANE's 0.95, that
// being AC's own figure for the concrete on this circuit, rather than an invented one.
constexpr auto bathurstSurfaces = std::array{
    TrackSurface{.key = "ROAD", .friction = 0.98, .kind = raceengine::SurfaceKind::Tarmac},
    TrackSurface{.key = "EDGE", .friction = 0.96, .kind = raceengine::SurfaceKind::Tarmac},
    TrackSurface{.key = "KERB", .friction = 0.96, .damping = 0.001, .kind = raceengine::SurfaceKind::Kerb},
    TrackSurface{.key = "PITLANE", .friction = 0.95, .kind = raceengine::SurfaceKind::Tarmac},
    TrackSurface{.key = "LINES", .friction = 0.95, .damping = 0.001, .kind = raceengine::SurfaceKind::Tarmac},
    TrackSurface{
        .key = "GRASS", .friction = 0.72, .damping = 0.005, .bumpiness = 0.05, .kind = raceengine::SurfaceKind::Grass},
    TrackSurface{
        .key = "SAND", .friction = 0.90, .damping = 0.05, .bumpiness = 0.01, .kind = raceengine::SurfaceKind::Gravel},
    TrackSurface{.key = "CUT", .friction = 0.95, .kind = raceengine::SurfaceKind::Tarmac},
    TrackSurface{.key = "WALL", .friction = 0.95, .kind = raceengine::SurfaceKind::Wall}};

constexpr auto bathurstGrid =
    std::array{GridSlot{.position = glm::dvec3(121.23581, 31.21522, -583.34558), .yaw = -100.000030},
               GridSlot{.position = glm::dvec3(122.28194, 31.11862, -588.51135), .yaw = -100.000030},
               GridSlot{.position = glm::dvec3(132.84029, 31.19675, -581.31903), .yaw = -100.000030},
               GridSlot{.position = glm::dvec3(134.07028, 31.08885, -586.26605), .yaw = -100.000030},
               GridSlot{.position = glm::dvec3(144.82327, 31.14151, -579.11279), .yaw = -100.000030},
               GridSlot{.position = glm::dvec3(145.84677, 31.01288, -584.14429), .yaw = -100.000030},
               GridSlot{.position = glm::dvec3(156.63475, 31.09563, -576.78357), .yaw = -100.000030},
               GridSlot{.position = glm::dvec3(157.66176, 30.96518, -581.82501), .yaw = -100.000030},
               GridSlot{.position = glm::dvec3(168.35210, 31.10075, -574.46002), .yaw = -100.000030},
               GridSlot{.position = glm::dvec3(169.34669, 30.96121, -579.58765), .yaw = -100.000030},
               GridSlot{.position = glm::dvec3(180.36108, 31.14211, -572.26917), .yaw = -100.000030},
               GridSlot{.position = glm::dvec3(181.35718, 30.99500, -577.33490), .yaw = -100.000030},
               GridSlot{.position = glm::dvec3(192.61592, 31.15940, -569.94250), .yaw = -100.000030},
               GridSlot{.position = glm::dvec3(193.54576, 30.99666, -575.03955), .yaw = -100.000030},
               GridSlot{.position = glm::dvec3(204.69899, 31.01649, -567.48608), .yaw = -100.000030},
               GridSlot{.position = glm::dvec3(205.67245, 30.90416, -572.51752), .yaw = -100.000030},
               GridSlot{.position = glm::dvec3(216.36038, 30.74099, -565.20685), .yaw = -100.000030},
               GridSlot{.position = glm::dvec3(217.23723, 30.63959, -570.29822), .yaw = -100.000030},
               GridSlot{.position = glm::dvec3(228.32785, 30.39649, -562.78326), .yaw = -100.000030},
               GridSlot{.position = glm::dvec3(229.37250, 30.28957, -567.91089), .yaw = -100.000030},
               GridSlot{.position = glm::dvec3(240.12854, 29.98587, -560.65692), .yaw = -100.000030},
               GridSlot{.position = glm::dvec3(241.17467, 29.89829, -565.77271), .yaw = -100.000030},
               GridSlot{.position = glm::dvec3(252.03319, 29.53999, -558.33032), .yaw = -100.000030},
               GridSlot{.position = glm::dvec3(253.06306, 29.49186, -563.47736), .yaw = -100.000030},
               GridSlot{.position = glm::dvec3(263.56592, 29.09024, -556.07397), .yaw = -100.000030},
               GridSlot{.position = glm::dvec3(264.58942, 29.05436, -561.10547), .yaw = -100.000030},
               GridSlot{.position = glm::dvec3(275.62750, 28.60046, -553.69470), .yaw = -100.000030},
               GridSlot{.position = glm::dvec3(276.65454, 28.55360, -558.78619), .yaw = -100.000030},
               GridSlot{.position = glm::dvec3(287.04465, 28.10235, -551.42114), .yaw = -100.000030},
               GridSlot{.position = glm::dvec3(287.93918, 28.04940, -556.54883), .yaw = -100.000030},
               GridSlot{.position = glm::dvec3(299.53079, 27.51073, -549.15765), .yaw = -100.000030},
               GridSlot{.position = glm::dvec3(300.55426, 27.45635, -554.18909), .yaw = -100.000030},
               GridSlot{.position = glm::dvec3(310.31903, 27.00576, -547.00287), .yaw = -100.000030},
               GridSlot{.position = glm::dvec3(311.34607, 26.98109, -552.09442), .yaw = -100.000030},
               GridSlot{.position = glm::dvec3(322.27423, 26.43453, -544.63446), .yaw = -100.000030},
               GridSlot{.position = glm::dvec3(323.16879, 26.44857, -549.76215), .yaw = -100.000030}};

// Grand City Parkway's surfaces, from `gcp/physics_manifest.json`, in that file's index order.
//
// **The first six are AC's own `surfaces.ini` lines verbatim, and the last three are not stated
// anywhere**, which is the same situation Bathurst's WALL is in and is resolved the same way:
// against what AC itself would do, and said out loud rather than invented.
//
// TILE, SAND and KSTREE carry no triangles in this export and are kept for the reason LINES is kept
// above — an index in this table is what a triangle's surface index means, and dropping an empty row
// would shift every row after it away from the manifest that explains them.
//
// WALL is 503,559 triangles of lamp posts, fences and street furniture, and the audit puts 54% of
// its faces off the horizontal: it is a barrier and takes KERB's 0.94, that being this track's own
// figure for concrete, rather than an invented one.
//
// RROD is the one judgement call in the table and it is the other way about. It has no
// `surfaces.ini` line either, so the exporter files it under barriers — but its 2,895 triangles are
// **100% upward-facing** and its meshes are named `1rrod0.yrroding` and `1rrod0.ymid` beside the
// road's own `1ROAD0.yROADing`. It is carriageway the author left out of `surfaces.ini`, so it is
// carried as tarmac at ROAD's 0.98, which is also the figure AC itself would land on: an unmatched
// key falls back to `SURFACE_0`, and `SURFACE_0` on this track is ROAD.
constexpr auto grandCityParkwaySurfaces =
    std::array{TrackSurface{.key = "ROAD", .friction = 0.98, .kind = raceengine::SurfaceKind::Tarmac},
               TrackSurface{.key = "GRASS", .friction = 0.85, .kind = raceengine::SurfaceKind::Grass},
               TrackSurface{.key = "KERB", .friction = 0.94, .kind = raceengine::SurfaceKind::Kerb},
               TrackSurface{.key = "MARK", .friction = 0.93, .kind = raceengine::SurfaceKind::Tarmac},
               TrackSurface{.key = "TILE", .friction = 0.92, .kind = raceengine::SurfaceKind::Tarmac},
               TrackSurface{.key = "SAND", .friction = 0.82, .kind = raceengine::SurfaceKind::Gravel},
               // AC states this one and states it as not a track: FRICTION 0, IS_VALID_TRACK 0. It is what a
               // tree is made of, and a car that reaches it gets no grip from it by the author's own decision.
               TrackSurface{.key = "KSTREE", .friction = 0.0, .kind = raceengine::SurfaceKind::Wall},
               TrackSurface{.key = "WALL", .friction = 0.94, .kind = raceengine::SurfaceKind::Wall},
               TrackSurface{.key = "RROD", .friction = 0.98, .kind = raceengine::SurfaceKind::Tarmac}};

// The forty-six authored boxes, on a concrete apron beside the parkway rather than on a start/finish
// line — this track's own layout is `freeroam` and it has no grid in the racing sense. Every slot
// probes down onto `1KERB0.conc_lod450m.001` at y = 0.15 m, which is that apron.
constexpr auto grandCityParkwayGrid =
    std::array{GridSlot{.position = glm::dvec3(-588.69733, 0.15000, -18.80842), .yaw = 58.820900},
               GridSlot{.position = glm::dvec3(-593.66058, 0.15000, -18.02717), .yaw = 58.820900},
               GridSlot{.position = glm::dvec3(-595.73779, 0.15000, -15.33873), .yaw = 58.820900},
               GridSlot{.position = glm::dvec3(-597.59900, 0.15000, -12.84790), .yaw = 58.820900},
               GridSlot{.position = glm::dvec3(-598.59406, 0.15000, -10.67996), .yaw = 78.160720},
               GridSlot{.position = glm::dvec3(-598.77209, 0.15000, -7.66393), .yaw = 89.813460},
               GridSlot{.position = glm::dvec3(-598.74524, 0.15000, -4.25698), .yaw = 96.198540},
               GridSlot{.position = glm::dvec3(-598.35394, 0.15000, -0.69045), .yaw = 100.076050},
               GridSlot{.position = glm::dvec3(-597.28351, 0.15000, 2.06085), .yaw = 117.033250},
               GridSlot{.position = glm::dvec3(-595.35339, 0.15000, 4.66076), .yaw = 125.439830},
               GridSlot{.position = glm::dvec3(-593.23938, 0.15000, 7.42273), .yaw = 125.439830},
               GridSlot{.position = glm::dvec3(-591.03815, 0.15000, 8.67708), .yaw = 154.122420},
               GridSlot{.position = glm::dvec3(-587.07214, 0.15000, 7.86825), .yaw = 154.122420},
               GridSlot{.position = glm::dvec3(-583.40485, 0.15000, 5.34066), .yaw = 154.122420},
               GridSlot{.position = glm::dvec3(-588.66016, 0.15000, -15.18598), .yaw = 87.524410},
               GridSlot{.position = glm::dvec3(-590.80170, 0.15000, -12.04717), .yaw = 87.524410},
               GridSlot{.position = glm::dvec3(-591.68866, 0.15000, -8.82104), .yaw = 87.524410},
               GridSlot{.position = glm::dvec3(-591.79895, 0.15000, -5.38351), .yaw = 87.524410},
               GridSlot{.position = glm::dvec3(-591.16016, 0.15000, -1.93679), .yaw = 87.524410},
               GridSlot{.position = glm::dvec3(-590.20886, 0.15000, 1.09172), .yaw = 87.524410},
               GridSlot{.position = glm::dvec3(-586.63647, 0.15000, 3.21571), .yaw = 115.209900},
               GridSlot{.position = glm::dvec3(-583.44275, 0.15000, -12.97360), .yaw = 88.972850},
               GridSlot{.position = glm::dvec3(-583.68542, 0.15000, -9.76401), .yaw = 88.972850},
               GridSlot{.position = glm::dvec3(-583.93909, 0.15000, -6.78053), .yaw = 88.972850},
               GridSlot{.position = glm::dvec3(-583.97333, 0.15000, -4.03657), .yaw = 98.729690},
               GridSlot{.position = glm::dvec3(-582.80969, 0.15000, 0.33663), .yaw = 98.729690},
               GridSlot{.position = glm::dvec3(-550.57501, 0.15000, 7.93917), .yaw = -164.322430},
               GridSlot{.position = glm::dvec3(-553.97760, 0.15000, 8.41896), .yaw = -164.322430},
               GridSlot{.position = glm::dvec3(-557.47949, 0.15000, 9.04212), .yaw = -164.322430},
               GridSlot{.position = glm::dvec3(-560.97034, 0.15000, 9.54948), .yaw = -164.322430},
               GridSlot{.position = glm::dvec3(-564.07025, 0.15000, 14.54140), .yaw = -127.058820},
               GridSlot{.position = glm::dvec3(-545.20587, 0.15000, 6.87491), .yaw = -140.864610},
               GridSlot{.position = glm::dvec3(-541.12494, 0.15000, 6.70947), .yaw = -140.864610},
               GridSlot{.position = glm::dvec3(-537.17639, 0.15000, 6.54403), .yaw = -140.864610},
               GridSlot{.position = glm::dvec3(-528.82153, 0.15000, 6.76462), .yaw = -140.864610},
               GridSlot{.position = glm::dvec3(-524.67993, 0.15000, 6.64881), .yaw = -140.864610},
               GridSlot{.position = glm::dvec3(-516.97876, 0.15000, -17.66742), .yaw = -41.919540},
               GridSlot{.position = glm::dvec3(-521.13690, 0.15000, -17.88801), .yaw = -41.919540},
               GridSlot{.position = glm::dvec3(-525.23438, 0.15000, -18.11412), .yaw = -41.919540},
               GridSlot{.position = glm::dvec3(-532.85022, 0.15000, -18.00382), .yaw = -41.919540},
               GridSlot{.position = glm::dvec3(-536.28302, 0.15000, -18.20100), .yaw = -37.434680},
               GridSlot{.position = glm::dvec3(-540.72241, 0.15000, -18.11276), .yaw = -37.434680},
               GridSlot{.position = glm::dvec3(-544.27979, 0.15000, -18.21231), .yaw = -33.588140},
               GridSlot{.position = glm::dvec3(-550.42871, 0.15000, -18.02481), .yaw = -33.588140},
               GridSlot{.position = glm::dvec3(-553.96368, 0.15000, -18.21782), .yaw = -33.588140},
               GridSlot{.position = glm::dvec3(-557.33075, 0.15000, -17.82818), .yaw = -28.678540}};

// The two circuits this game carries.
//
// Grand City Parkway is the default and Mount Panorama is the one both frame gates were blessed
// against, which is why `scripts/verify-parity.sh` and `scripts/smoke.sh` name a track rather than
// inheriting this default: a golden frame is a photograph of one world, and a gate that followed the
// game's default would go red on the day the default moved and report it as a rendering change.
constexpr auto trackTable = std::array{
    TrackDefinition{.id = "gcp",
                    .name = "Grand City Parkway",
                    .visualAsset = "assets/Tracks/gcp/visual.glb",
                    .physicsAsset = "assets/Tracks/gcp/physics.glb",
                    .buildingColliderAsset = "assets/Tracks/gcp/grand_city_parkway_building_colliders.glb",
                    .propColliderAsset = "assets/Tracks/gcp/grand_city_parkway_prop_colliders.glb",
                    .propVisualAsset = "assets/Tracks/gcp/grand_city_parkway_props_visual.glb",
                    .colliderManifestAsset = "assets/Tracks/gcp/grand_city_parkway_colliders.json",
                    .trafficAsset = "assets/Tracks/gcp/grand_city_parkway_traffic.json",
                    // Exported without `--ac-materials`: `gcp/visual.log` records "converted 51 materials to
                    // metalness-roughness (infer)", and not one of the 52 carries an `extras.blinn_phong` block.
                    // So this file states itself in the model "pbr" reads, and re-exporting it with the flag is
                    // what would move it to "blinn-phong" — the two go together and neither half is a preference.
                    .visualShader = "blinn-phong",
                    .surfaces = grandCityParkwaySurfaces,
                    .grid = grandCityParkwayGrid,
                    // Thirty metres over the apron the car spawns on, which on this track is a street rather than
                    // open ground: the buildings within sixty metres of it reach 150 m, so the probe stands in the
                    // canyon and photographs the canyon. That is the indirect light this world actually has.
                    .lightProbeMetres = glm::dvec3(-588.7, 30.0, -18.8),
                    // A city laid out about sea level: the carriageway is at 0.15 m and the physics export spans
                    // −24 to +8.6 m.
                    .fogBaseHeightMetres = 0.0,
                    // A hundred and twenty metres back down the spawn's own heading and sixty up, looking at the
                    // grid.
                    .viewpointMetres = glm::dvec3(-691.3, 60.2, -80.9),
                    .viewpointYawRadians = 1.0266,
                    .viewpointPitchRadians = -0.4636},
    TrackDefinition{.id = "bathurst",
                    .name = "Mount Panorama",
                    .visualAsset = "assets/Tracks/rt_bathurst_visual.glb",
                    .physicsAsset = "assets/Tracks/rt_bathurst_physics.glb",
                    // A circuit's walls are authored collision and are already in the physics export
                    // above, so there is nothing for a derived hull to add. Stated rather than left
                    // out, because every other field of this table is stated.
                    .buildingColliderAsset = "",
                    .propColliderAsset = "",
                    .propVisualAsset = "",
                    .colliderManifestAsset = "",
                    // A circuit, not a city: AC ships no traffic lane graph for it and there is
                    // nothing for one to be exported from. Stated rather than left out, because
                    // every other field of this table is stated.
                    .trafficAsset = "",
                    // Exported with `--ac-materials`, so its 188 materials each state an ambient, a diffuse, a
                    // specular and a Blinn-Phong exponent that a modeller set while looking at that formula.
                    .visualShader = "blinn-phong",
                    .surfaces = bathurstSurfaces,
                    .grid = bathurstGrid,
                    // Thirty metres over the pit straight and midway between the two cameras the scene offers,
                    // ninety-odd metres from each, which is what the 250 m skybox allows and the reason it is not
                    // simply over the grid.
                    .lightProbeMetres = glm::dvec3(30.0, 65.0, -565.0),
                    // The pit straight stands at about 35 m, and the Mountain climbs 174 m out of that — which at
                    // the rig's hundred-metre scale height puts the top of it in a fifth of the air the pit lane
                    // stands in.
                    .fogBaseHeightMetres = 35.0,
                    // Hell Corner from above and behind, and nothing gates it — this is the view to fly the
                    // circuit from by hand.
                    .viewpointMetres = glm::dvec3(-660.0, 195.8, 1216.0),
                    .viewpointYawRadians = 1.5708,
                    .viewpointPitchRadians = -0.08}};

// The track this run drives, chosen once in `Options.cppm` and read here.
export [[nodiscard]] const TrackDefinition& trackFor(TrackChoice chosen);

// The surface table in the order the definition states it, which is the order a triangle's surface
// index means.
export [[nodiscard]] std::vector<raceengine::SurfaceMaterial> trackSurfaceMaterials(const TrackDefinition& track);

// Which half of a collider export to read.
//
// The two live in one file and are told apart by the material the exporter gave them —
// `collider_static` and `collider_dynamic` — which is the *only* place that distinction is
// recorded in a form the model loader carries: the node extras that state it in words are dropped
// on load, and the hull meshes are unnamed. So this is a filter on `Material::name`, and a file
// whose materials are named anything else yields nothing rather than yielding everything.
export enum class ColliderBody { Static, Dynamic };

// The convex colliders of a loaded collider export, as the physics world takes them.
//
// Same ordering constraint as `trackCollisionMesh` below and for the same reason: the renderer frees
// a mesh buffer the moment it uploads it, so this must run before anything draws that model. Nothing
// draws these — they are collision and there is no picture of them — which is what makes them safe
// to read at any point in a level's construction rather than only in its constructor.
//
// **Every hull is given the track's own WALL surface**, which is what it is: barrier geometry with
// no authored friction, taking this track's own figure for concrete. It is deliberately not a new
// surface of its own — the index a hit reports is an index into the table `physics.glb`'s mesh names
// are resolved against, and a table with entries nothing in that file can name is a table with a
// hole in it waiting for somebody to renumber. A track whose surfaces state no WALL is refused
// rather than defaulted, because the fallback index is 0 and on both of these tracks that is ROAD:
// a car would grip a building like tarmac and nothing would say so.
export [[nodiscard]] std::expected<std::vector<raceengine::ConvexCollider>, std::string>
trackColliderHulls(raceengine::MemoryStorageService& storage, const raceengine::Resource<raceengine::Model>& handle,
                   const TrackDefinition& track, ColliderBody wanted);

// The breakable props of a loaded prop-collider export, joined to the manifest that weighs them.
//
// **Two files and neither is sufficient alone.** The glTF carries the hulls and their placement and
// can say nothing about mass; the manifest carries mass, inertia and the two break thresholds and
// carries no geometry. They are joined on the hull mesh's name, which is why the exporter names the
// meshes and not only the nodes — a glTF *mesh* name is the only one the model loader keeps.
//
// A manifest entry naming a hull this file does not carry is a mismatched pair of exports and is
// refused: the alternative is a lamp column with half its shape, which stands up and is wrong in a
// way nothing would report.
export [[nodiscard]] std::expected<std::vector<raceengine::BreakableProp>, std::string>
trackBreakableProps(raceengine::MemoryStorageService& storage, const raceengine::Resource<raceengine::Model>& handle,
                    const TrackDefinition& track, const ColliderManifest& manifest);

// The collision geometry of a loaded model, as the physics world takes it.
//
// It must run before anything draws that model: the renderer frees each `MeshBuffer::data` the
// moment it uploads it to the GPU, and upload happens lazily on the first draw. A level builds this
// in its constructor, which is entirely ahead of the first `Engine::step`, so the ordering is not a
// race — but it is a real dependency and the reason this is not something a game can ask for later.
export [[nodiscard]] std::expected<raceengine::SurfaceMesh, std::string>
trackCollisionMesh(raceengine::MemoryStorageService& storage, const raceengine::Resource<raceengine::Model>& handle,
                   const TrackDefinition& track);

} // namespace osr

namespace osr
{

const TrackDefinition& trackFor(const TrackChoice chosen)
{
    switch (chosen)
    {
    case TrackChoice::GrandCityParkway:
        return trackTable[0];
    case TrackChoice::Bathurst:
        break;
    }

    return trackTable[1];
}

std::vector<raceengine::SurfaceMaterial> trackSurfaceMaterials(const TrackDefinition& track)
{
    auto materials = std::vector<raceengine::SurfaceMaterial>();
    materials.reserve(track.surfaces.size());

    for (const auto& surface : track.surfaces)
    {
        materials.push_back(raceengine::SurfaceMaterial{.gripMultiplier = surface.friction,
                                                        .bumpiness = surface.bumpiness,
                                                        .damping = surface.damping,
                                                        .kind = surface.kind});
    }

    return materials;
}

namespace
{

// glTF's own numbering, which the loaded model carries through verbatim. Named here rather than
// spelled at the comparison because the sandbox cannot see tinygltf's headers and a bare 5123 in a
// bounds check is unreadable.
constexpr auto gltfModeTriangles = 4;
constexpr auto gltfComponentUnsignedByte = 5121;
constexpr auto gltfComponentUnsignedShort = 5123;
constexpr auto gltfComponentUnsignedInt = 5125;
constexpr auto gltfComponentFloat = 5126;
constexpr auto gltfTypeVec3 = 3;

// The surface a collision mesh's name declares.
//
// AC marks a mesh as collision geometry with a leading digit and then names the `surfaces.ini` key,
// and the two tracks here show the two shapes that takes: Bathurst writes `1ROAD_mp-asphalt014` and
// Grand City Parkway writes `1KERB0.sidew_part0_lod250m` — a key, an index, and a dot. So the key is
// whatever stands between the leading digits and the first `_` or `.`, with any trailing index taken
// off it, and the comparison ignores case because that same track writes `1rrod0.ymid_lod250m` in
// lower case.
//
// **The match is exact and stays exact.** AC itself resolves the key by longest prefix match, which
// quietly makes an unknown surface into a known one; this refuses instead, because a track whose road
// is being read as grass should stop rather than be driven on. Trimming a trailing index is not a
// prefix match — `KERB0` resolves to KERB and `KERBSTONE` still resolves to nothing.
[[nodiscard]] bool sameKey(const std::string_view left, const std::string_view right)
{
    if (left.size() != right.size())
    {
        return false;
    }

    const auto upper = [](const char character)
    {
        return character >= 'a' && character <= 'z' ? static_cast<char>(character - ('a' - 'A')) : character;
    };

    for (auto index = std::size_t{0}; index < left.size(); index++)
    {
        if (upper(left[index]) != upper(right[index]))
        {
            return false;
        }
    }

    return true;
}

[[nodiscard]] std::expected<std::uint32_t, std::string> surfaceOf(const std::string& meshName,
                                                                  const std::span<const TrackSurface> surfaces)
{
    auto key = std::string_view(meshName);
    while (!key.empty() && key.front() >= '0' && key.front() <= '9')
    {
        key.remove_prefix(1);
    }

    key = key.substr(0, key.find_first_of("_."));

    const auto lookUp = [&](const std::string_view candidate) -> std::expected<std::uint32_t, std::string>
    {
        for (auto index = std::size_t{0}; index < surfaces.size(); index++)
        {
            if (sameKey(surfaces[index].key, candidate))
            {
                return static_cast<std::uint32_t>(index);
            }
        }

        return std::unexpected(std::string(candidate));
    };

    if (const auto found = lookUp(key); found)
    {
        return found.value();
    }

    auto trimmed = key;
    while (!trimmed.empty() && trimmed.back() >= '0' && trimmed.back() <= '9')
    {
        trimmed.remove_suffix(1);
    }

    if (trimmed.size() != key.size())
    {
        if (const auto found = lookUp(trimmed); found)
        {
            return found.value();
        }
    }

    return std::unexpected("mesh '" + meshName + "' names surface '" + std::string(key) +
                           "', which this track's surface table does not carry");
}

// One element out of a mesh buffer, by value. memcpy rather than a reinterpret_cast because a
// glTF buffer view is aligned to its own component and not to whatever this reads it as.
template <typename T> [[nodiscard]] T readAt(const std::vector<unsigned char>& data, const std::size_t offset)
{
    auto value = T{};
    std::memcpy(&value, data.data() + offset, sizeof(T));

    return value;
}

} // namespace

std::expected<raceengine::SurfaceMesh, std::string>
trackCollisionMesh(raceengine::MemoryStorageService& storage, const raceengine::Resource<raceengine::Model>& handle,
                   const TrackDefinition& track)
{
    const auto* model = storage.models.find(handle);
    if (model == nullptr)
    {
        return std::unexpected("the track model handle names no live model");
    }

    auto mesh = raceengine::SurfaceMesh{};
    mesh.materials = trackSurfaceMaterials(track);

    for (const auto& meshKey : model->meshes)
    {
        const auto* source = storage.meshes.find(meshKey);
        if (source == nullptr)
        {
            return std::unexpected("the track model names a mesh that is no longer loaded");
        }

        const auto surface = surfaceOf(source->name, track.surfaces);
        if (!surface)
        {
            return std::unexpected(surface.error());
        }

        // Identity for this asset — every node states its geometry in world space — but read rather
        // than assumed, because assuming it is exactly how a model arrives as a pile at the origin.
        // Widened to double before it is applied: the positions are single precision in the file, so
        // this is where that precision stops being lost rather than where it starts.
        const auto placement = glm::dmat4(source->modelMatrix);

        for (const auto& primitive : source->meshPrimitives)
        {
            if (primitive.mode != gltfModeTriangles)
            {
                return std::unexpected("mesh '" + source->name + "' carries a primitive that is not a triangle list");
            }

            const raceengine::MeshPrimitiveAttribute* position = nullptr;
            for (const auto& attribute : primitive.attributes)
            {
                if (attribute.attributeType == raceengine::PrimitiveAttributeType::Position)
                {
                    position = &attribute;
                }
            }

            if (position == nullptr)
            {
                return std::unexpected("mesh '" + source->name + "' has a primitive with no POSITION attribute");
            }

            if (position->componentType != gltfComponentFloat || position->type != gltfTypeVec3)
            {
                return std::unexpected("mesh '" + source->name + "' states positions this loader cannot read");
            }

            const auto indexWidth = primitive.componentType == gltfComponentUnsignedByte    ? std::size_t{1}
                                    : primitive.componentType == gltfComponentUnsignedShort ? std::size_t{2}
                                    : primitive.componentType == gltfComponentUnsignedInt   ? std::size_t{4}
                                                                                            : std::size_t{0};
            if (indexWidth == 0)
            {
                return std::unexpected("mesh '" + source->name + "' states indices this loader cannot read");
            }

            if (primitive.elementCount % 3 != 0)
            {
                return std::unexpected("mesh '" + source->name + "' has an index count that is not whole triangles");
            }

            if (primitive.meshBufferIndex < 0 ||
                std::cmp_greater_equal(primitive.meshBufferIndex, model->meshBuffers.size()) ||
                position->bufferIndex < 0 || std::cmp_greater_equal(position->bufferIndex, model->meshBuffers.size()))
            {
                return std::unexpected("mesh '" + source->name + "' names a buffer the model does not carry");
            }

            const auto& indexData = model->meshBuffers[static_cast<std::size_t>(primitive.meshBufferIndex)].data;
            const auto& vertexData = model->meshBuffers[static_cast<std::size_t>(position->bufferIndex)].data;

            // The one thing this cannot recover from rather than report: the renderer clears a
            // buffer's bytes when it uploads it, so an empty one here is not a malformed file, it is
            // this having been asked for after the first frame.
            if (indexData.empty() || vertexData.empty())
            {
                return std::unexpected("the track's geometry has already been uploaded and released; the collision "
                                       "mesh has to be read before anything draws it");
            }

            if (primitive.byteOffset + primitive.elementCount * indexWidth > indexData.size())
            {
                return std::unexpected("mesh '" + source->name + "' has an index range outside its buffer");
            }

            const auto readIndex = [&](const std::size_t element)
            {
                const auto at = primitive.byteOffset + element * indexWidth;

                return indexWidth == 1   ? static_cast<std::uint32_t>(readAt<std::uint8_t>(indexData, at))
                       : indexWidth == 2 ? static_cast<std::uint32_t>(readAt<std::uint16_t>(indexData, at))
                                         : readAt<std::uint32_t>(indexData, at);
            };

            // Every index the primitive uses, so the vertices copied below are exactly the ones it
            // reaches. glTF states no vertex count on a primitive and the accessor's is not carried
            // through the loader, so this is where the extent comes from.
            auto highest = std::uint32_t{0};
            for (auto element = std::size_t{0}; element < primitive.elementCount; element++)
            {
                highest = std::max(highest, readIndex(element));
            }

            const auto stride = position->stride > 0 ? static_cast<std::size_t>(position->stride) : 3 * sizeof(float);
            if (position->offset + static_cast<std::size_t>(highest) * stride + 3 * sizeof(float) > vertexData.size())
            {
                return std::unexpected("mesh '" + source->name + "' indexes past the end of its vertex buffer");
            }

            const auto base = static_cast<std::uint32_t>(mesh.vertices.size());
            mesh.vertices.reserve(mesh.vertices.size() + highest + 1);
            for (auto vertex = std::uint32_t{0}; vertex <= highest; vertex++)
            {
                const auto at = position->offset + static_cast<std::size_t>(vertex) * stride;
                const auto local =
                    glm::dvec4(static_cast<double>(readAt<float>(vertexData, at)),
                               static_cast<double>(readAt<float>(vertexData, at + sizeof(float))),
                               static_cast<double>(readAt<float>(vertexData, at + 2 * sizeof(float))), 1.0);

                mesh.vertices.emplace_back(glm::dvec3(placement * local));
            }

            mesh.indices.reserve(mesh.indices.size() + primitive.elementCount);
            for (auto element = std::size_t{0}; element < primitive.elementCount; element++)
            {
                mesh.indices.push_back(base + readIndex(element));
            }

            mesh.surfaces.insert(mesh.surfaces.end(), primitive.elementCount / 3, surface.value());

            // A barrier is emitted twice, wound both ways, and the doubling is armour rather than
            // tidiness. The shape query ignores back faces — the proving ground's own barrier was
            // driven straight through while every ray cast said it was there — and an imported
            // track's wall winding is whatever the modder authored: audited against the racing
            // line, 8% of this circuit's merged wall loop faces away from the road, which is a car
            // that bounces off most walls and sails through particular ones, the least diagnosable
            // version of "no walls". A wall's job is to stop a car from *both* sides — being
            // between a wall and the road edge is an ordinary place for a rally-crossed car to be —
            // so both windings is what a barrier means, not a workaround. Drivable surfaces stay
            // single-sided: a tyre reaches them through rays, which have no facing to get wrong.
            if (track.surfaces[surface.value()].kind == raceengine::SurfaceKind::Wall)
            {
                for (auto element = std::size_t{0}; element + 2 < primitive.elementCount; element += 3)
                {
                    mesh.indices.push_back(base + readIndex(element));
                    mesh.indices.push_back(base + readIndex(element + 2));
                    mesh.indices.push_back(base + readIndex(element + 1));
                }

                mesh.surfaces.insert(mesh.surfaces.end(), primitive.elementCount / 3, surface.value());
            }
        }
    }

    if (mesh.indices.empty())
    {
        return std::unexpected("the track model carries no triangles");
    }

    return mesh;
}


std::expected<std::vector<raceengine::ConvexCollider>, std::string>
trackColliderHulls(raceengine::MemoryStorageService& storage, const raceengine::Resource<raceengine::Model>& handle,
                   const TrackDefinition& track, const ColliderBody wanted)
{
    const auto* model = storage.models.find(handle);
    if (model == nullptr)
    {
        return std::unexpected("the collider model handle names no live model");
    }

    auto wall = track.surfaces.size();
    for (auto index = std::size_t{0}; index < track.surfaces.size(); index++)
    {
        if (sameKey(track.surfaces[index].key, "WALL"))
        {
            wall = index;
        }
    }

    if (wall == track.surfaces.size())
    {
        return std::unexpected("this track's surface table states no WALL, which is the surface a derived "
                               "collider hull presents");
    }

    const auto wantedMaterial = wanted == ColliderBody::Static ? std::string_view("collider_static")
                                                               : std::string_view("collider_dynamic");

    auto hulls = std::vector<raceengine::ConvexCollider>();

    for (const auto& meshKey : model->meshes)
    {
        const auto* source = storage.meshes.find(meshKey);
        if (source == nullptr)
        {
            return std::unexpected("the collider model names a mesh that is no longer loaded");
        }

        // The body node's placement, which the loader has already accumulated down the chain — the
        // hull children state no transform of their own, which is the single thing the exporter's
        // own document warns hardest about and is the thing this loader makes unreachable.
        //
        // Split into a rotation and a translation rather than applied whole: the translation becomes
        // the body's position and the points stay local to it. Baking both into the points would put
        // a city's worth of hulls in world coordinates, and Jolt is single precision.
        const auto placement = glm::dmat4(source->modelMatrix);
        const auto rotation = glm::dmat3(placement);
        const auto origin = glm::dvec3(placement[3]);

        for (const auto& primitive : source->meshPrimitives)
        {
            if (!primitive.material.has_value())
            {
                continue;
            }

            const auto* material = storage.materials.find(primitive.material.value());
            if (material == nullptr || std::string_view(material->name) != wantedMaterial)
            {
                continue;
            }

            if (primitive.mode != gltfModeTriangles)
            {
                return std::unexpected("a collider hull carries a primitive that is not a triangle list");
            }

            const raceengine::MeshPrimitiveAttribute* position = nullptr;
            for (const auto& attribute : primitive.attributes)
            {
                if (attribute.attributeType == raceengine::PrimitiveAttributeType::Position)
                {
                    position = &attribute;
                }
            }

            if (position == nullptr)
            {
                return std::unexpected("a collider hull has a primitive with no POSITION attribute");
            }

            if (position->componentType != gltfComponentFloat || position->type != gltfTypeVec3)
            {
                return std::unexpected("a collider hull states positions this loader cannot read");
            }

            const auto indexWidth = primitive.componentType == gltfComponentUnsignedByte    ? std::size_t{1}
                                    : primitive.componentType == gltfComponentUnsignedShort ? std::size_t{2}
                                    : primitive.componentType == gltfComponentUnsignedInt   ? std::size_t{4}
                                                                                            : std::size_t{0};
            if (indexWidth == 0)
            {
                return std::unexpected("a collider hull states indices this loader cannot read");
            }

            if (primitive.meshBufferIndex < 0 ||
                std::cmp_greater_equal(primitive.meshBufferIndex, model->meshBuffers.size()) ||
                position->bufferIndex < 0 || std::cmp_greater_equal(position->bufferIndex, model->meshBuffers.size()))
            {
                return std::unexpected("a collider hull names a buffer the model does not carry");
            }

            const auto& indexData = model->meshBuffers[static_cast<std::size_t>(primitive.meshBufferIndex)].data;
            const auto& vertexData = model->meshBuffers[static_cast<std::size_t>(position->bufferIndex)].data;

            // The same one-way failure `trackCollisionMesh` reports: a buffer the renderer has
            // already uploaded and released is not a malformed file, it is this having been asked
            // for too late.
            if (indexData.empty() || vertexData.empty())
            {
                return std::unexpected("the collider geometry has already been uploaded and released; the hulls "
                                       "have to be read before anything draws that model");
            }

            if (primitive.byteOffset + primitive.elementCount * indexWidth > indexData.size())
            {
                return std::unexpected("a collider hull has an index range outside its buffer");
            }

            const auto readIndex = [&](const std::size_t element)
            {
                const auto at = primitive.byteOffset + element * indexWidth;

                return indexWidth == 1   ? static_cast<std::uint32_t>(readAt<std::uint8_t>(indexData, at))
                       : indexWidth == 2 ? static_cast<std::uint32_t>(readAt<std::uint16_t>(indexData, at))
                                         : readAt<std::uint32_t>(indexData, at);
            };

            // Which vertices this primitive reaches, on `trackCollisionMesh`'s own reasoning: glTF
            // states no vertex count on a primitive and the accessor's does not survive the loader.
            auto highest = std::uint32_t{0};
            for (auto element = std::size_t{0}; element < primitive.elementCount; element++)
            {
                highest = std::max(highest, readIndex(element));
            }

            const auto stride = position->stride > 0 ? static_cast<std::size_t>(position->stride) : 3 * sizeof(float);
            if (position->offset + static_cast<std::size_t>(highest) * stride + 3 * sizeof(float) > vertexData.size())
            {
                return std::unexpected("a collider hull indexes past the end of its vertex buffer");
            }

            // **The triangles are dropped and only the points are kept**, which is not a shortcut:
            // a convex hull is defined by its point set, and the physics backend rebuilds its own
            // face planes from them regardless. Interior and duplicate points cost load time and
            // change nothing — this export unrolls each hull's triangles per face, so a 36-point
            // hull arrives as 228 positions and Jolt merges them back down.
            auto hull = raceengine::ConvexCollider{
                .points = {}, .origin = origin, .surface = static_cast<std::uint32_t>(wall)};
            hull.points.reserve(static_cast<std::size_t>(highest) + 1);

            for (auto vertex = std::uint32_t{0}; vertex <= highest; vertex++)
            {
                const auto at = position->offset + static_cast<std::size_t>(vertex) * stride;
                const auto local = glm::dvec3(static_cast<double>(readAt<float>(vertexData, at)),
                                              static_cast<double>(readAt<float>(vertexData, at + sizeof(float))),
                                              static_cast<double>(readAt<float>(vertexData, at + 2 * sizeof(float))));

                hull.points.emplace_back(rotation * local);
            }

            // Four points is the least a solid can be made of, and a degenerate hull is refused by
            // the backend by name. Skipped here instead, because a collider export is thousands of
            // derived shapes and one bad one is a reason to lose that shape rather than the city.
            if (hull.points.size() >= 4)
            {
                hulls.push_back(std::move(hull));
            }
        }
    }

    return hulls;
}


std::expected<std::vector<raceengine::BreakableProp>, std::string>
trackBreakableProps(raceengine::MemoryStorageService& storage, const raceengine::Resource<raceengine::Model>& handle,
                    const TrackDefinition& track, const ColliderManifest& manifest)
{
    // The dynamic half of the same file the static half was read out of, so the filter is the one
    // `trackColliderHulls` uses and the two together take every hull exactly once.
    auto loose = trackColliderHulls(storage, handle, track, ColliderBody::Dynamic);
    if (!loose)
    {
        return std::unexpected(loose.error());
    }

    // The hulls come back in the model's own order, which is the glTF's node order, and a prop's
    // entry names its hulls rather than counting them. So the join is by name, and the names are
    // read back out of the same meshes — sorted once and bisected rather than scanned, because three
    // and a half thousand props against ten thousand hulls is a scan nobody should write.
    //
    // No `std::map`: a second global module fragment declaring one breaks the sandbox link outright
    // (CLAUDE.md, *Do not break*), and a sorted vector is what that rule leaves.
    auto byName = std::vector<std::pair<std::string_view, std::size_t>>();
    byName.reserve(loose->size());

    {
        const auto* model = storage.models.find(handle);
        if (model == nullptr)
        {
            return std::unexpected("the collider model handle names no live model");
        }

        // `trackColliderHulls` walks the model's meshes in order and keeps the dynamic ones, so
        // walking it the same way here pairs each kept hull with the mesh it came from.
        auto kept = std::size_t{0};
        for (const auto& meshKey : model->meshes)
        {
            const auto* source = storage.meshes.find(meshKey);
            if (source == nullptr)
            {
                return std::unexpected("the collider model names a mesh that is no longer loaded");
            }

            for (const auto& primitive : source->meshPrimitives)
            {
                if (!primitive.material.has_value())
                {
                    continue;
                }

                const auto* material = storage.materials.find(primitive.material.value());
                if (material == nullptr || std::string_view(material->name) != "collider_dynamic")
                {
                    continue;
                }

                if (kept >= loose->size())
                {
                    return std::unexpected("the collider model yielded fewer hulls than it names");
                }

                byName.emplace_back(std::string_view(source->name), kept);
                kept++;
            }
        }

        if (kept != loose->size())
        {
            return std::unexpected("the collider model's hull names and its hulls disagree in number");
        }
    }

    std::sort(byName.begin(), byName.end(), [](const auto& left, const auto& right) { return left.first < right.first; });

    auto props = std::vector<raceengine::BreakableProp>();
    props.reserve(manifest.props.size());

    for (const auto& entry : manifest.props)
    {
        if (!entry.dynamic)
        {
            continue;
        }

        auto prop = raceengine::BreakableProp{.hulls = {},
                                              .mass = entry.massKg,
                                              .inertia = entry.inertia,
                                              .breakForce = entry.breakForceN,
                                              .breakTorque = entry.breakTorqueNm};
        prop.hulls.reserve(entry.hulls.size());

        for (const auto& wanted : entry.hulls)
        {
            const auto found = std::lower_bound(byName.begin(), byName.end(), std::string_view(wanted),
                                                [](const auto& left, const std::string_view key)
                                                { return left.first < key; });

            if (found == byName.end() || found->first != std::string_view(wanted))
            {
                return std::unexpected("the manifest names hull '" + wanted +
                                       "', which the collider export does not carry");
            }

            prop.hulls.push_back(loose.value()[found->second]);
        }

        props.push_back(std::move(prop));
    }

    return props;
}

} // namespace osr
