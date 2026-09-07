module;

#include <cstddef>
#include <expected>
#include <fstream>
#include <ios>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <glm/glm.hpp>

export module osr.game:ColliderManifest;

import :Json;

namespace osr
{

// The physics half of a track's collider export, as `~/dev/ac-car-data` writes it beside the two
// collider glTF files.
//
// **The geometry is not here and never will be.** A hull is thousands of coordinates and belongs in
// a binary file the model loader already reads; what is here is everything a hull cannot state — how
// heavy the body is, how its mass is distributed, and how hard it has to be hit before it stops
// being scenery. The two are joined by name: a manifest entry lists the hull meshes that make it up,
// and the glTF names those meshes.
//
// Nothing in this file imports `raceengine`, which is what keeps `<fstream>` out of a global module
// fragment that would then be merged against every imported BMI. It is the `TrafficNetwork.cppm`
// pattern and the reason for it is in docs/build-times.md.

// One body of the prop export.
export struct PropEntry
{
    // The body node's name, which is also the prefix of every hull mesh belonging to it.
    std::string name;
    // The exporter's own class — `lamp_column`, `street_furniture`, `heavy_structure`. Carried
    // because it is what a retune is keyed on and what a log line should say, and read by nothing
    // today.
    std::string className;

    // **`false` is the whole of what makes a body scenery.** The exporter forces static anything over
    // 2.5 m3 of hull or 4 m across whatever its class, so a concrete stair block is static and a
    // bench of the same class is not.
    bool dynamic = false;

    // Kilograms. An *effective* density times the hull's volume, clamped per class — no mass in this
    // export came from the content, because Assetto Corsa states none.
    double massKg = 0.0;
    // About the centre of mass, axes parallel to the world. The body's origin *is* its centre of
    // mass, so it needs no parallel-axis shift on the way into a physics engine.
    glm::dmat3 inertia{1.0};

    // What the base gives way at. Either one alone releases the body.
    double breakForceN = 0.0;
    double breakTorqueNm = 0.0;

    // The hull meshes this body is made of, in child order. Named rather than indexed because an
    // index into a glTF is a property of that file's node ordering and a name survives a re-export.
    std::vector<std::string> hulls;

    // Where the body stands, metres. The same value as the glTF node's translation, and carried here
    // as the cross-check rather than as the source: the geometry path reads the placement out of the
    // model it read the points from, so the two cannot drift.
    glm::dvec3 origin{0.0};
};

export struct ColliderManifest
{
    std::vector<PropEntry> props;
};

// Read one. Fails rather than degrades: a manifest that half-parses is a city with some of its
// street furniture weightless, which is not a thing anybody would notice until it was driven into.
export [[nodiscard]] std::expected<ColliderManifest, std::string> loadColliderManifest(const std::string& filePath);

} // namespace osr

namespace osr
{

namespace
{

[[nodiscard]] std::expected<glm::dvec3, std::string> vectorFrom(const JsonValue& parent, const std::string_view key,
                                                                const std::string& name)
{
    const auto* found = member(parent, key);
    if (found == nullptr || found->kind != JsonKind::Array || found->items.size() != 3)
    {
        return std::unexpected("'" + name + "' states no three-number '" + std::string(key) + "'");
    }

    for (const auto& element : found->items)
    {
        if (element.kind != JsonKind::Number)
        {
            return std::unexpected("'" + name + "' has a non-number in '" + std::string(key) + "'");
        }
    }

    return glm::dvec3(found->items[0].number, found->items[1].number, found->items[2].number);
}

// Row major in the document, which is how a tensor is written down; glm indexes by column first, so
// this is where the transpose happens rather than at the point of use.
[[nodiscard]] std::expected<glm::dmat3, std::string> tensorFrom(const JsonValue& parent, const std::string& name)
{
    const auto* found = member(parent, "inertia_kgm2");
    if (found == nullptr || found->kind != JsonKind::Array || found->items.size() != 3)
    {
        return std::unexpected("dynamic body '" + name + "' states no 3x3 'inertia_kgm2'");
    }

    auto tensor = glm::dmat3(0.0);
    for (auto row = 0; row < 3; row++)
    {
        const auto& line = found->items[static_cast<std::size_t>(row)];
        if (line.kind != JsonKind::Array || line.items.size() != 3)
        {
            return std::unexpected("dynamic body '" + name + "' has a row of 'inertia_kgm2' that is not three numbers");
        }

        for (auto column = 0; column < 3; column++)
        {
            const auto& element = line.items[static_cast<std::size_t>(column)];
            if (element.kind != JsonKind::Number)
            {
                return std::unexpected("dynamic body '" + name + "' has a non-number in 'inertia_kgm2'");
            }

            tensor[column][row] = element.number;
        }
    }

    return tensor;
}

} // namespace

std::expected<ColliderManifest, std::string> loadColliderManifest(const std::string& filePath)
{
    auto file = std::ifstream(filePath, std::ios::binary);
    if (!file)
    {
        return std::unexpected("the collider manifest '" + filePath + "' could not be opened");
    }

    auto buffer = std::ostringstream();
    buffer << file.rdbuf();

    auto document = buffer.str();
    auto reader = JsonReader(document);
    auto parsed = reader.read();
    if (!parsed)
    {
        return std::unexpected("the collider manifest '" + filePath + "' is not valid JSON: " +
                               std::move(parsed).error());
    }

    const auto& root = parsed.value();

    const auto* props = member(root, "props");
    if (props == nullptr || props->kind != JsonKind::Object)
    {
        return std::unexpected("the collider manifest states no 'props' object");
    }

    const auto* items = member(*props, "items");
    if (items == nullptr || items->kind != JsonKind::Array)
    {
        return std::unexpected("the collider manifest's 'props' states no 'items' array");
    }

    auto manifest = ColliderManifest{};
    manifest.props.reserve(items->items.size());

    for (const auto& item : items->items)
    {
        if (item.kind != JsonKind::Object)
        {
            return std::unexpected("a prop entry is not an object");
        }

        auto entry = PropEntry{};
        entry.name = textFrom(item, "name");
        entry.className = textFrom(item, "class");

        if (entry.name.empty())
        {
            return std::unexpected("a prop entry states no name, and a name is what joins it to its hulls");
        }

        entry.dynamic = textFrom(item, "body") == "dynamic";

        const auto origin = vectorFrom(item, "origin", entry.name);
        if (!origin)
        {
            return std::unexpected(origin.error());
        }
        entry.origin = origin.value();

        const auto* hulls = member(item, "hulls");
        if (hulls == nullptr || hulls->kind != JsonKind::Array || hulls->items.empty())
        {
            return std::unexpected("'" + entry.name + "' lists no hulls");
        }

        entry.hulls.reserve(hulls->items.size());
        for (const auto& hull : hulls->items)
        {
            if (hull.kind != JsonKind::String)
            {
                return std::unexpected("'" + entry.name + "' has a hull entry that is not a name");
            }

            entry.hulls.push_back(hull.text);
        }

        if (entry.dynamic)
        {
            entry.massKg = numberFrom(item, "mass_kg", 0.0);
            if (!(entry.massKg > 0.0))
            {
                return std::unexpected("dynamic body '" + entry.name + "' states no mass");
            }

            const auto inertia = tensorFrom(item, entry.name);
            if (!inertia)
            {
                return std::unexpected(inertia.error());
            }
            entry.inertia = inertia.value();

            // `anchor` is null on a static body and set on every dynamic one, which the exporter
            // states outright — so an absent one here is a malformed entry rather than a body that
            // happens not to break.
            const auto* anchor = member(item, "anchor");
            if (anchor == nullptr || anchor->kind != JsonKind::Object)
            {
                return std::unexpected("dynamic body '" + entry.name + "' states no anchor");
            }

            entry.breakForceN = numberFrom(*anchor, "break_force_n", 0.0);
            entry.breakTorqueNm = numberFrom(*anchor, "break_torque_nm", 0.0);

            if (!(entry.breakForceN > 0.0) || !(entry.breakTorqueNm > 0.0))
            {
                return std::unexpected("dynamic body '" + entry.name + "' states an anchor that never holds");
            }
        }

        manifest.props.push_back(std::move(entry));
    }

    return manifest;
}

} // namespace osr
