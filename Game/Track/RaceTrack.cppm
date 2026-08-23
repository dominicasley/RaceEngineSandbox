module;

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <glm/glm.hpp>

export module osr.game:RaceTrack;

import raceengine;

namespace osr
{

// Bathurst, as physics reads it: one glTF, two consumers.
//
// The circuit is `assets/Tracks/rt_bathurst_track.glb` — Assetto Corsa's collision geometry for
// Mount Panorama, converted by /home/dominic-asley/dev/ac-car-data. 74 meshes, 615,197 triangles,
// nine surfaces, already in world space and already in **metres**. The renderer loads it as an
// ordinary model and the collision mesh below is read out of that *same* loaded model rather than
// out of a second parse of the same bytes, which is the whole reason the two cannot drift: a
// mismatch between what is drawn and what is driven on would read as the car floating or sinking,
// and there is no second source for it to disagree with.
//
// The manifest beside it, `rt_bathurst_track.json`, is the provenance for every number in this
// file: it is what the surface table and the grid slots were read off, and it is carried into the
// repository so those numbers can be audited rather than believed.

// One `surfaces.ini` entry, keyed the way Assetto Corsa keys them.
//
// The friction figures are AC's own and are used *as* the grip multiplier, because that is what
// they already are in AC: a per-surface scale on the tyre's own grip, with a racing surface at
// very nearly one. Nothing here is quantised onto `SurfaceKind` — a road at 0.98 and a pit lane at
// 0.95 are different numbers and stay different numbers; the kind is a label for whatever one day
// wants to know that a gravel trap is not a road.
struct TrackSurface
{
    std::string_view key;
    double friction = 1.0;
    double damping = 0.0;
    // AC's SIN_HEIGHT: the amplitude of the fine ripple it adds under a car on that surface, which
    // is exactly what `SurfaceMaterial::bumpiness` means.
    double bumpiness = 0.0;
    raceengine::SurfaceKind kind = raceengine::SurfaceKind::Tarmac;
};

// Every surface the export carries, in the manifest's own index order so a row here and a row there
// are the same row. LINES has no triangles in this track and is kept for that reason: dropping an
// empty surface would shift every index after it away from the document that explains them.
//
// WALL is the one entry AC does not state, because a wall is collision geometry rather than a
// surface a car drives on — it has no `surfaces.ini` line at all. It takes PITLANE's 0.95, that
// being AC's own figure for the concrete on this circuit, rather than an invented one.
constexpr auto trackSurfaces = std::array{
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

// An authored start box: where a car is put and which way it points. Read from the `AC_START_n`
// dummies through the manifest, and already dropped onto the collision mesh — AC's own boxes float
// two to three metres above the road because the game drops cars onto them.
//
// This is the spawn and the AI line is not, which is a trap worth stating where it can be read: the
// first point of `fast_lane.ai` is a *racing* line hugging the exit of the first corner, 1.04 m from
// the right edge of an 11 m road, and its height is the recording car's reference height rather than
// the tarmac — a median 0.79 m of thin air.
export struct GridSlot
{
    glm::dvec3 position{0.0};
    // Degrees, right-handed about +y, taking +z onto the heading — the same convention the vehicle's
    // body frame uses, so it becomes an orientation with no axis remap.
    double yaw = 0.0;
};

export constexpr auto gridSlots =
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

// Two exports of one circuit, and each is read by exactly one consumer. The visual model is the
// scenery — buildings, trees, the mount — and only the renderer touches it; the physics model is
// the drivable surfaces under AC's `1SURFACE_` naming, and only the collision reader touches it.
// Splitting them is what lets the picture carry 681 primitives of scenery without the BVH having to
// wade through a single triangle of it, and lets the surfaces stay exact while the scenery is
// re-exported at will.
export inline constexpr auto trackVisualAsset = std::string_view("assets/Tracks/rt_bathurst_visual.glb");
export inline constexpr auto trackPhysicsAsset = std::string_view("assets/Tracks/rt_bathurst_physics.glb");

// Which of the render rig's shaders draws the visual export, and it is a property of *that file*
// rather than of the scene that shows it — which is why it is named here beside the asset and not
// beside the entity that is built from it.
//
// The circuit's materials are Assetto Corsa's own, exported as authored rather than converted:
// 188 of them, each stating an ambient, a diffuse, a specular and a Blinn-Phong exponent that a
// modeller set while looking at that formula. Drawn through "pbr" they would need a metalness, and
// there is no metalness in the source data to read — it can only be inferred, and an inferred metal
// loses its diffuse colour outright and reads as black tarmac. "blinn-phong" shades from what the
// file actually states. Re-export the asset with `--ac-materials` (see ~/dev/ac-car-data) and this
// stays true; re-export it without and every material loses the block this shader reads, which
// shows up as a circuit uniformly lit by its own ambient term.
export inline constexpr auto trackVisualShader = std::string_view("blinn-phong");

// The surface table in the order `trackSurfaces` states it, which is the order a triangle's surface
// index means.
export [[nodiscard]] std::vector<raceengine::SurfaceMaterial> trackSurfaceMaterials()
{
    auto materials = std::vector<raceengine::SurfaceMaterial>();
    materials.reserve(trackSurfaces.size());

    for (const auto& surface : trackSurfaces)
    {
        materials.push_back(raceengine::SurfaceMaterial{.gripMultiplier = surface.friction,
                                                        .bumpiness = surface.bumpiness,
                                                        .damping = surface.damping,
                                                        .kind = surface.kind});
    }

    return materials;
}

// The collision geometry of a loaded model, as the physics world takes it.
//
// It must run before anything draws that model: the renderer frees each `MeshBuffer::data` the
// moment it uploads it to the GPU, and upload happens lazily on the first draw. A level builds this
// in its constructor, which is entirely ahead of the first `Engine::step`, so the ordering is not a
// race — but it is a real dependency and the reason this is not something a game can ask for later.
export [[nodiscard]] std::expected<raceengine::SurfaceMesh, std::string>
trackCollisionMesh(raceengine::MemoryStorageService& storage, const raceengine::Resource<raceengine::Model>& handle);

} // namespace osr

namespace osr
{

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
// AC names a physics mesh `<digit><SURFACE>_<whatever>`: the leading digit is what marks it as
// collision geometry at all, and the token after it is the `surfaces.ini` key. AC itself resolves
// that key by longest prefix match, which quietly makes an unknown surface into a known one; this
// refuses instead, because a track whose road is being read as grass should stop rather than be
// driven on.
[[nodiscard]] std::expected<std::uint32_t, std::string> surfaceOf(const std::string& meshName)
{
    auto key = std::string_view(meshName);
    while (!key.empty() && key.front() >= '0' && key.front() <= '9')
    {
        key.remove_prefix(1);
    }

    key = key.substr(0, key.find('_'));

    for (auto index = std::size_t{0}; index < trackSurfaces.size(); index++)
    {
        if (trackSurfaces[index].key == key)
        {
            return static_cast<std::uint32_t>(index);
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
trackCollisionMesh(raceengine::MemoryStorageService& storage, const raceengine::Resource<raceengine::Model>& handle)
{
    const auto* model = storage.models.find(handle);
    if (model == nullptr)
    {
        return std::unexpected("the track model handle names no live model");
    }

    auto mesh = raceengine::SurfaceMesh{};
    mesh.materials = trackSurfaceMaterials();

    for (const auto& meshKey : model->meshes)
    {
        const auto* source = storage.meshes.find(meshKey);
        if (source == nullptr)
        {
            return std::unexpected("the track model names a mesh that is no longer loaded");
        }

        const auto surface = surfaceOf(source->name);
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
            if (trackSurfaces[surface.value()].kind == raceengine::SurfaceKind::Wall)
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

} // namespace osr
