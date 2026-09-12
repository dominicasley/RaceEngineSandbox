module;

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

export module osr.game:TrafficCars;

import :PoliceLights;
import :RaceTrack;
import :RenderRig;
import :TrackFrame;
import :TrafficDirector;

import raceengine;

namespace osr
{

// The drawn half of traffic: a fixed pool of car renderables, reassigned every frame to whichever
// cars are nearest the camera and drawn at whichever level of detail the distance asks for.
//
// **The simulated count and the drawn count are different numbers on purpose.** The population is a
// city — four hundred cars, all of them moving, all of them collidable — and the pool is however
// many the frame can afford. Nothing about the model changes when the pool is made bigger or
// smaller, and nothing about the pool is visible to the model: what joins them is a list of poses.
//
// **A slot owns one node and one renderable per level, and shows exactly one of them.** A
// renderable's model is fixed when it is created, so a slot cannot swap between a 32k-triangle car
// and a 400-triangle one — it has to own both and hide the other. Hiding is `layers = 0`, which no
// camera's mask matches, so a parked level costs one integer test in the draw walk and no engine
// change.
//
// That layout is also what keeps instanced rendering open: every car is a shared model with a
// per-instance colour on the renderable, and no car owns a node beyond the frame it is assigned one.
//
// **The driver's mirror may be told to see a coarser car than the frame does** (`mirrorLevelFloor`,
// docs/driver-mirrors-brief.md). A slot then shows two of its levels at once: the one the distance
// asks for on the world layer, for every view the frame is made of, and the floor level on the
// mirror layer, which the mirror view alone draws. Where the two are the same level it stands on
// both layers, so a car past the floor's distance is one renderable in both pictures. Every other
// view — the prepass, the near cascades, the frame's blended pass — masks the mirror layer out,
// because a second copy of a car where the frame already has one is a fight in the shared depth.
//
// **The police light bars are drawn here too** (docs/police-lights-brief.md), in two halves that
// read one pattern (PoliceLights.cppm). The lens: a patrol car with its bar on is handed what each
// side of the bar emits this frame through the renderable's own `signal`, and the fleet's `beacon`
// material — the exporter's bucket for the bar's diodes, drawn by the pbr shader's lens variant —
// adds it over the shaded glass. The lamp: the frame's light budget less the sun is a pool of point
// lights, dealt each frame to the nearest patrol cars with their bars on and stood on the lit side
// of each bar, switched off between flashes so an empty street uploads the sun alone.

// What the fleet is painted, for the cars that take a colour: **a pool of real factory colours**,
// named off the ranges of the four marques that are actually on this street — Holden Commodore, Kia
// Carnival, Subaru Legacy, Toyota Hilux (2026-09-13, Dominic's ask; the ten before it were plausible
// road-car colours nobody sold). The comment carries the paint chip as sRGB and the value is that
// chip linearised and pulled to 0.8 of it, for the reason the player's own paint is
// (CarEntity.cppm): a base coat at the sRGB value's face reads brighter than any real pigment
// reflects.
//
// Three things about it are decisions rather than data.
//
// **The pool is shared by every body shape.** An agent's colour is one index drawn against one count
// (`TrafficPopulationOptions::colourCount`) and its shape is a separate index, so a Hilux can wear a
// Kia colour. Giving each shape its own range means interpreting the index per shape, which is an
// engine-side change for a thing nobody reads at ten metres.
//
// **The draw is uniform, so the street's mix is this pool's composition**: twelve of the eighteen are
// a white, a black, a grey or a silver, which is about the neutral share of a real registration year.
// Adding a colour here adds its share to the street.
//
// **No two entries differ only by flake**, because traffic paint states none (`flakeDensity` is zero
// below): four marques' blacks would be four copies of one colour. Where a family appears more than
// once the entries differ in hue or in value — a pure white, a warm pearl, a cool pearl and a solid;
// a neutral black and a blue-black.
//
// The factory paint codes are deliberately not here. Only some of them are verifiable from where
// this was written, and a code this project cannot check is worse than a name.
export inline constexpr auto trafficPalette = std::array<glm::vec3, 18>{
    glm::vec3(0.717f, 0.724f, 0.710f),  // #F3F4F2  Toyota Super White
    glm::vec3(0.710f, 0.691f, 0.652f),  // #F2EFE9  Kia Snow White Pearl
    glm::vec3(0.684f, 0.697f, 0.704f),  // #EEF0F1  Subaru Crystal White Pearl
    glm::vec3(0.677f, 0.677f, 0.646f),  // #EDEDE8  Holden Heron White
    glm::vec3(0.019f, 0.020f, 0.020f),  // #2B2C2C  Toyota Attitude Black Mica
    glm::vec3(0.016f, 0.019f, 0.025f),  // #262A31  Subaru Crystal Black Silica
    glm::vec3(0.467f, 0.488f, 0.494f),  // #C9CDCE  Toyota Silver Metallic
    glm::vec3(0.343f, 0.365f, 0.379f),  // #AFB4B7  Kia Silky Silver
    glm::vec3(0.216f, 0.230f, 0.240f),  // #8E9295  Toyota Grey Metallic
    glm::vec3(0.127f, 0.137f, 0.148f),  // #6F7377  Kia Panthera Metal
    glm::vec3(0.096f, 0.120f, 0.150f),  // #616C78  Holden Prussian Steel
    glm::vec3(0.074f, 0.082f, 0.092f),  // #565A5F  Subaru Magnetite Grey Metallic
    glm::vec3(0.023f, 0.069f, 0.227f),  // #2F5391  Holden Perfect Blue
    glm::vec3(0.014f, 0.048f, 0.125f),  // #24456E  Kia Deep Chroma Blue
    glm::vec3(0.009f, 0.019f, 0.048f),  // #1B2B45  Subaru Abyss Blue Pearl
    glm::vec3(0.427f, 0.007f, 0.009f),  // #C1171C  Holden Red Hot
    glm::vec3(0.248f, 0.006f, 0.008f),  // #97141A  Toyota Emotional Red
    glm::vec3(0.078f, 0.050f, 0.041f)}; // #584740  Toyota Phantom Brown

export class TrafficCars
{
public:
    // `fleet` is what the track states, and **empty is legal**: the simulation runs, the cars
    // collide, and nothing is drawn. It says so once, so an empty street is diagnosed from a log
    // line rather than from the seat.
    // `mirrorLevelFloor` is the lowest level of detail the mirror layer shows a car at; zero is the
    // level the frame shows, which is the pool as it was before there was a mirror.
    // `policeLights` off is the fleet before there were any: every bar a dark lens, no lamp created.
    TrafficCars(raceengine::Engine& engine, Scene& scene, std::span<const TrafficCarModel> fleet,
                std::span<const double> levelMetres, std::size_t maximumDrawn, std::size_t mirrorLevelFloor = 0,
                bool policeLights = true);

    TrafficCars(const TrafficCars&) = delete;
    TrafficCars(TrafficCars&&) = delete;
    TrafficCars& operator=(const TrafficCars&) = delete;
    TrafficCars& operator=(TrafficCars&&) = delete;

    // Main thread, once a frame, with whatever the simulation last published.
    void update(const std::vector<TrafficSnapshot>& snapshots, const glm::dvec3& cameraMetres);

    [[nodiscard]] std::size_t drawn() const
    {
        return live;
    }

private:
    static constexpr auto noLevel = static_cast<std::size_t>(-1);

    // One mesh of one level that turns with the road: which of the renderable's meshes it is, and
    // the tyre radius its angle is read against.
    struct Wheel
    {
        std::size_t mesh = 0;
        float radiusMetres = 0.0f;
    };

    struct Slot
    {
        SceneNode* node = nullptr;
        // One per level of detail, nearest first, all hanging on the node above.
        std::vector<RenderableModel*> levels{};
        // Parallel to `levels`: the meshes of each that spin. Empty for a level whose export merged
        // its wheels into the body, which is levels 2 and 3 of the whole fleet.
        std::vector<std::vector<Wheel>> wheels{};
        std::uint8_t body = 0;
        bool paintable = true;
        // Which level is showing on the world layer, or `noLevel`, and which on the mirror's —
        // the same level when the floor is not in play.
        std::size_t shown = noLevel;
        std::size_t mirrorShown = noLevel;
        // What colour this slot is currently painted, so a frame that reassigns it to a car of the
        // same colour writes nothing. 0xff is "never painted".
        std::uint8_t colour = 0xff;
    };

    // Where a body shape's light bar is: the centre of the exporter's `LIGHTBAR_beacon` node in the
    // car's own frame, metres, and half the bar's width. A shape whose model carries no such node
    // has no bar: its lens never lights and it is dealt no lamp.
    struct LightBar
    {
        bool present = false;
        glm::vec3 centreMetres{0.0f};
        float halfWidthMetres = 0.0f;
    };

    [[nodiscard]] std::size_t levelFor(double metres) const;
    [[nodiscard]] std::vector<Wheel> findWheels(const RenderableModel& renderable) const;
    [[nodiscard]] LightBar findLightBar(const RenderableModel& renderable) const;
    void placeLamps(const std::vector<TrafficSnapshot>& snapshots);
    void darkenGlass(const raceengine::Resource<raceengine::Model>& model) const;
    void show(Slot& slot, const TrafficSnapshot& snapshot, std::size_t level);
    void spinWheels(Slot& slot, std::size_t level, const TrafficSnapshot& snapshot) const;
    void hide(Slot& slot);

    raceengine::Engine& engine;
    std::vector<double> levelMetres;
    // How many cars are drawn at most, whatever the pool holds.
    std::size_t drawnCap = 0;
    // The mirror's floor on a car's level of detail; see the class comment.
    std::size_t mirrorFloor = 0;
    // The police light bars, on or off as a whole (`OSR_POLICE_LIGHTS`).
    bool policeLights = true;
    // Per body shape, in the fleet's order — `Slot::body` indexes it.
    std::vector<LightBar> lightBars;
    // The lamps, into the scene's own add-only deque, created once and switched per frame.
    std::vector<raceengine::Light*> lamps;
    // The bars' clock this frame: the engine's simulated seconds, read once per update so every
    // lens and every lamp shows one instant, and a capture's flashes are a function of the frame.
    double clockSeconds = 0.0;
    std::vector<Slot> slots;
    std::size_t live = 0;
    // Reused every frame so the draw walk allocates nothing after the first one.
    std::vector<std::size_t> order;
    std::vector<bool> taken;
};

} // namespace osr

namespace osr
{

TrafficCars::TrafficCars(raceengine::Engine& engine, Scene& scene, const std::span<const TrafficCarModel> fleet,
                         const std::span<const double> levels, const std::size_t maximumDrawn,
                         const std::size_t mirrorLevelFloor, const bool lights) :
    engine(engine),
    levelMetres(levels.begin(), levels.end()),
    drawnCap(maximumDrawn),
    mirrorFloor(mirrorLevelFloor),
    policeLights(lights)
{
    if (fleet.empty() || maximumDrawn == 0)
    {
        engine.log().info("Traffic cars: no fleet stated, so traffic is simulated and not drawn");

        return;
    }

    // Every body shape gets a whole pool of its own, because a slot's models are fixed when it is
    // created and the nearest cars can all be of one shape. A share per shape — a fifth plus one —
    // meant the eighth Commodore among the nearest thirty-two was not drawn, and *which* one changed
    // as they moved: a car vanishing at forty metres and coming back. A hidden renderable costs one
    // integer test in the draw walk, so the spare slots are cheap where the pop was not; the count
    // that is actually drawn is capped by `drawnCap` in `update`.
    const auto perBody = maximumDrawn;

    slots.reserve(perBody * fleet.size());

    auto triangles = std::size_t{0};
    auto wheelMeshes = std::size_t{0};

    for (auto index = std::size_t{0}; index < fleet.size(); index++)
    {
        const auto& car = fleet[index];

        // Loaded once per level and shared by every slot of this shape: `Resource<Model>` is a
        // handle, so twenty models are twenty loads however many slots name them.
        auto models = std::vector<raceengine::Resource<raceengine::Model>>();
        models.reserve(car.levels.size());

        for (const auto& asset : car.levels)
        {
            auto loaded = engine.resource().loadModelAsync(std::string(asset)).get();
            if (!loaded)
            {
                raceengine::fail(loaded.error());
            }

            models.push_back(std::move(loaded).value());
            darkenGlass(models.back());
        }

        for (auto copy = std::size_t{0}; copy < perBody; copy++)
        {
            auto& node = engine.sceneManager().createNode(scene);

            auto slot = Slot{.node = &node,
                             .body = static_cast<std::uint8_t>(index),
                             .paintable = car.paintable};

            slot.levels.reserve(models.size());
            slot.wheels.reserve(models.size());

            for (const auto& model : models)
            {
                auto& created = engine.scene().createEntity(
                    scene, CreateRenderableModelDTO{.node = node,
                                                    .shader = engine.shader().getShaderByName("pbr").value(),
                                                    .model = model});

                // Traffic, not scenery, and for the reason the player's car states: a light probe is
                // captured once and shaded from for many frames, so a car baked into one goes on
                // lighting the street from wherever it was parked when the capture ran. It is also
                // what keeps traffic out of the two cached far shadow cascades.
                created.staticGeometry = false;
                created.layers = 0u;

                slot.levels.push_back(&created);
                slot.wheels.push_back(findWheels(created));
                wheelMeshes += slot.wheels.back().size();
            }

            // A world unit is a tenth of a metre and the fleet is authored in metres, the same as
            // `golf_gti_2018.glb`. A model authored in centimetres comes out a hundred times small,
            // which reads as a missing asset rather than as a wrong scale.
            engine.sceneManager().setScale(node, 10.0f, 10.0f, 10.0f);

            // Where this shape's light bar is, read once off its nearest level: every copy is the
            // same model.
            if (copy == 0)
            {
                lightBars.push_back(slot.levels.empty() ? LightBar{} : findLightBar(*slot.levels.front()));
            }

            slots.push_back(std::move(slot));
        }

        triangles += car.levels.size();
    }

    engine.log().info("Traffic cars: {} slots across {} body shapes, {} models, {} levels of detail, {} wheel meshes; "
                      "the mirror sees level {} or coarser",
                      slots.size(), fleet.size(), triangles, levelMetres.size() + 1, wheelMeshes, mirrorFloor);

    // The lamps (docs/police-lights-brief.md): the frame's light budget less what the scene already
    // holds — the sun — created off. A lamp is switched on only on a frame it has a lit bar to stand
    // over, so a city with no chase in it uploads the sun alone and pays nothing for these.
    const auto barred = std::ranges::count_if(lightBars, [](const LightBar& bar) { return bar.present; });

    // A patrol car whose nearest level has no bar is an export that lost its `beacon` bucket — the
    // Charger's did on 2026-09-11, re-exported for its livery without the lights — and a drive is
    // the wrong place to find that out. Said here, once, at load, for every shape that has a siren.
    for (auto index = std::size_t{0}; index < fleet.size() && index < lightBars.size(); index++)
    {
        if (!fleet[index].siren.empty() && !lightBars[index].present)
        {
            engine.log().warn("Police lights: '{}' carries the siren but its nearest level has no LIGHTBAR_beacon "
                              "node, so its bar never lights and it is dealt no lamp. Re-export it with car_lod.py, "
                              "which reads the lights off the car's own extension/ext_config.ini "
                              "(docs/police-lights-brief.md)",
                              fleet[index].name);
        }
    }

    if (policeLights && barred > 0)
    {
        for (auto index = scene.lights.size(); index < raceengine::maxLights; index++)
        {
            auto& lamp = engine.scene().createLight(scene);
            lamp = raceengine::Light{.type = raceengine::LightType::Point, .enabled = false};
            lamps.push_back(&lamp);
        }
    }

    engine.log().info("Police lights: {} body shape{} with a light bar, {} lamp{} for the street{}", barred,
                      barred == 1 ? "" : "s", lamps.size(), lamps.size() == 1 ? "" : "s",
                      policeLights ? "" : " (OSR_POLICE_LIGHTS=off: every bar dark)");
}

// The light bar of one renderable's model, by the node the fleet's exporter writes for the roof
// bar's diodes — `LIGHTBAR_beacon`, a node of its own whose origin is the bar's centre and whose
// vertices are centred on it (car_lod.py, BEACON_PARTS) — so the bar's place in the car's frame is
// the node's own translation and its half width the primitive's own bounds, and nothing here is
// typed. Metres, the model's units, like the wheels' radii. A model with no such node is a car with
// no bar, which is every shape but the patrol car.
TrafficCars::LightBar TrafficCars::findLightBar(const RenderableModel& renderable) const
{
    static constexpr auto barMesh = std::string_view{"LIGHTBAR_beacon"};

    for (const auto& instance : renderable.meshes)
    {
        const auto* mesh = engine.memoryStorage().meshes.find(instance.mesh);
        if (mesh == nullptr || mesh->name != barMesh)
        {
            continue;
        }

        auto halfWidth = 0.0f;
        for (const auto& primitive : mesh->meshPrimitives)
        {
            halfWidth = std::max(halfWidth, primitive.boundsHalfExtent.x);
        }

        return LightBar{.present = true, .centreMetres = glm::vec3(mesh->modelMatrix[3]), .halfWidthMetres = halfWidth};
    }

    return LightBar{};
}

// The fleet's glass, as a pane with nothing behind it.
//
// The exporter buckets every window into one `glass` material and writes `extras.shader =
// "windshield"` on it, the same tag the player's `GlassInt` carries. That shader draws the grime on
// the *inner* pane of a windscreen and nothing else — the reflection belongs to `GlassExt` beside
// it, which stays `pbr` — so on a car that has one shell of glass and no interior it draws a faint
// film over the road behind the car, which reads as no glass at all. These cars have nothing behind
// their windows to look at, so the pane is the car's skin: a black, polished dielectric, which is
// what a tinted window with a dark cabin behind it looks like from the pavement. The roughness is
// the Golf's own exterior glass (`GlassExt`, 0.06); the base colour is near black so the diffuse
// term vanishes and the reflection — the sky and the sun, at the dielectric's Fresnel — is the whole
// of what the pane draws; the coverage stays just under one, blended as the asset states it, because
// a blended draw tests depth without writing it and an inner shell the exporter kept (the Kia's
// bucket carries more inward-facing triangles than outward ones) cannot then fight the outer one.
//
// Done on the material rather than on the export because it is this renderer's reading of the
// bucket, not a property of the asset: the same file under a shader that had a reflection in it
// would want none of this. Materials are shared storage owned by the model, so this runs once per
// loaded level and every slot of that shape sees it; the model is loaded by nothing else.
void TrafficCars::darkenGlass(const raceengine::Resource<raceengine::Model>& model) const
{
    // Linear, and below the darkest paint the palette carries — the tint is the absence of a
    // cabin, not a colour.
    constexpr auto colour = glm::vec4(0.008f, 0.008f, 0.010f, 0.97f);
    constexpr auto roughness = 0.06f;

    const auto* stored = engine.memoryStorage().models.find(model);
    if (stored == nullptr)
    {
        return;
    }

    for (const auto& key : stored->materials)
    {
        engine.memoryStorage().materials.mutate(key,
                                                [&](raceengine::Material& material)
                                                {
                                                    if (material.declaredShader != "windshield")
                                                    {
                                                        return;
                                                    }

                                                    // Cleared, so `createModel` falls the material
                                                    // back to the renderable's own `pbr` rather than
                                                    // resolving the grime shader.
                                                    material.declaredShader.clear();
                                                    material.baseColour = colour;
                                                    material.metalness = 0.0f;
                                                    material.roughness = roughness;
                                                });
    }
}

// The meshes of one renderable that are road wheels, by the name the exporter tags them with.
//
// `~/dev/ac-car-data` keeps each wheel as its own node, `WHEEL_LF`, `WHEEL_LR`, `WHEEL_RF`, `WHEEL_RR`,
// with the material bucket appended (`WHEEL_LF_body`, and on the Hilux a `WHEEL_LF_glass` hub cap
// beside it), each node carrying a translation and no rotation, and each mesh's vertices centred on
// its own node origin. So the wheel's own frame is the car's — +x left, +z forward — its spin axis is
// its local +x, and rotating about it through the origin spins the tyre about the hub.
//
// The radius is read off the asset rather than stated: the tyre's vertical half-extent, which the
// loader already keeps for culling. Per *corner* rather than per mesh, because the hub cap is a
// three-centimetre disc on the same axle and must turn at the tyre's rate, not its own.
std::vector<TrafficCars::Wheel> TrafficCars::findWheels(const RenderableModel& renderable) const
{
    static constexpr auto prefix = std::string_view{"WHEEL_"};
    static constexpr auto corners = std::array<std::string_view, 4>{"LF", "LR", "RF", "RR"};
    // What a wheel with no declared bounds is read against, which glTF does not permit for POSITION
    // and no export has produced: the fleet's own tyres, as `SimpleVehicleSetup` states them.
    static constexpr auto fallbackRadiusMetres = 0.36f;

    auto cornerOf = [](const std::string_view name) -> std::size_t
    {
        if (!name.starts_with(prefix))
        {
            return corners.size();
        }

        const auto tag = name.substr(prefix.size(), 2);

        return static_cast<std::size_t>(std::ranges::find(corners, tag) - corners.begin());
    };

    auto radii = std::array<float, 4>{};
    auto found = std::vector<std::pair<std::size_t, std::size_t>>();

    for (auto index = std::size_t{0}; index < renderable.meshes.size(); index++)
    {
        const auto* mesh = engine.memoryStorage().meshes.find(renderable.meshes[index].mesh);
        if (mesh == nullptr)
        {
            continue;
        }

        const auto corner = cornerOf(mesh->name);
        if (corner == corners.size())
        {
            continue;
        }

        for (const auto& primitive : mesh->meshPrimitives)
        {
            radii[corner] = std::max({radii[corner], primitive.boundsHalfExtent.y, primitive.boundsHalfExtent.z});
        }

        found.emplace_back(index, corner);
    }

    auto wheels = std::vector<Wheel>();
    wheels.reserve(found.size());

    for (const auto& [index, corner] : found)
    {
        wheels.push_back(Wheel{.mesh = index,
                               .radiusMetres = radii[corner] > 0.0f ? radii[corner] : fallbackRadiusMetres});
    }

    return wheels;
}

std::size_t TrafficCars::levelFor(const double metres) const
{
    for (auto index = std::size_t{0}; index < levelMetres.size(); index++)
    {
        if (metres < levelMetres[index])
        {
            return index;
        }
    }

    return levelMetres.size();
}

void TrafficCars::hide(Slot& slot)
{
    if (slot.shown != noLevel)
    {
        slot.levels[slot.shown]->layers = 0u;
        slot.shown = noLevel;
    }

    if (slot.mirrorShown != noLevel)
    {
        slot.levels[slot.mirrorShown]->layers = 0u;
        slot.mirrorShown = noLevel;
    }
}

void TrafficCars::show(Slot& slot, const TrafficSnapshot& snapshot, const std::size_t level)
{
    const auto wanted = std::min(level, slot.levels.size() - 1);

    const auto placed = toWorldUnits(snapshot.positionMetres);

    engine.sceneManager().setPosition(*slot.node, static_cast<float>(placed.x), static_cast<float>(placed.y),
                                      static_cast<float>(placed.z));

    // The scene graph is single precision throughout, so the pose narrows here and does so component
    // by component rather than through a conversion that would do it silently.
    engine.sceneManager().setOrientation(
        *slot.node, glm::quat(static_cast<float>(snapshot.orientation.w), static_cast<float>(snapshot.orientation.x),
                              static_cast<float>(snapshot.orientation.y), static_cast<float>(snapshot.orientation.z)));

    // The mirror's level: the frame's own, or the floor where the frame's is finer than it.
    const auto mirrorWanted = std::min(std::max(wanted, mirrorFloor), slot.levels.size() - 1);

    if (slot.shown != wanted || slot.mirrorShown != mirrorWanted)
    {
        hide(slot);

        if (mirrorWanted == wanted)
        {
            slot.levels[wanted]->layers = worldLayer | mirrorLayer;
        }
        else
        {
            slot.levels[wanted]->layers = worldLayer;
            slot.levels[mirrorWanted]->layers = mirrorLayer;
        }

        slot.shown = wanted;
        slot.mirrorShown = mirrorWanted;
    }

    // The wheels, on every level that is showing — the mirror's stand-in rolls too.
    spinWheels(slot, wanted, snapshot);

    if (mirrorWanted != wanted)
    {
        spinWheels(slot, mirrorWanted, snapshot);
    }

    // The light bar's lens (docs/police-lights-brief.md): what each side emits this frame, on every
    // level showing so the mirror's stand-in flashes with the frame's car. A car with its bar off —
    // every car that is not a patrol car in a chase, and every shape with no bar — states zero,
    // which is the plain pbr lens.
    auto signal = glm::vec4(0.0f);
    if (policeLights && snapshot.siren && lightBars[slot.body].present)
    {
        const auto state = beaconPattern(clockSeconds + beaconPhaseSeconds(snapshot.id));
        signal = glm::vec4(state.red * beaconLensRadiance, state.blue * beaconLensRadiance, 0.0f, 0.0f);
    }

    slot.levels[wanted]->signal = signal;
    slot.levels[mirrorWanted]->signal = signal;

    if (!slot.paintable)
    {
        // **This car's colour is in its own texture and must not be replaced.** The exporter marks
        // its paint bucket `extras.shader = "carpaint"`, and that shader reads the material's own
        // base map whenever the renderable states no paint — which for the police Charger is the
        // livery. Painting it would put a flat colour over the markings.
        return;
    }

    if (slot.colour == snapshot.colour)
    {
        return;
    }

    slot.colour = snapshot.colour;

    // The paint rides the renderable rather than the material, which is the whole of how one shared
    // model gives a street of different-coloured cars. No material is looked up and nothing inside
    // the asset is touched. Written to every level, so a car does not change colour as it recedes.
    const auto paint = raceengine::Paint{.enabled = true,
                                         .colour = trafficPalette[snapshot.colour % trafficPalette.size()],
                                         .flakeDensity = 0.0f,
                                         .clearcoat = 1.0f,
                                         .clearcoatRoughness = 0.06f,
                                         // Off, as on the player's car (CarEntity.cppm, 2026-09-13).
                                         .orangePeel = 0.0f,
                                         .orangePeelScale = 30.0f};

    for (auto* renderable : slot.levels)
    {
        renderable->paint = paint;
    }
}

// A positive angle about the wheel's local +x takes its top (+y) toward +z, which is the car's
// forward, so a car rolling forward turns its wheels forward. Wrapped to one turn in double before
// it narrows: a car that has driven the whole session has rolled tens of kilometres, and a float of
// that many radians steps by hundredths.
void TrafficCars::spinWheels(Slot& slot, const std::size_t level, const TrafficSnapshot& snapshot) const
{
    auto& meshes = slot.levels[level]->meshes;

    for (const auto& wheel : slot.wheels[level])
    {
        const auto circumference = 2.0 * glm::pi<double>() * static_cast<double>(wheel.radiusMetres);
        const auto radians = std::fmod(snapshot.rolledMetres, circumference) / static_cast<double>(wheel.radiusMetres);

        meshes[wheel.mesh].localTransform =
            glm::rotate(glm::mat4(1.0f), static_cast<float>(radians), glm::vec3(1.0f, 0.0f, 0.0f));
    }
}

void TrafficCars::update(const std::vector<TrafficSnapshot>& snapshots, const glm::dvec3& cameraMetres)
{
    live = 0;

    if (slots.empty())
    {
        return;
    }

    clockSeconds = engine.simulatedSeconds();

    // Nearest first, and only as many as there are slots. `partial_sort` rather than a full one:
    // four hundred cars are ordered every frame and only the first few dozen of that order is ever
    // read.
    order.clear();
    order.reserve(snapshots.size());

    for (auto index = std::size_t{0}; index < snapshots.size(); index++)
    {
        order.push_back(index);
    }

    const auto kept = std::min(order.size(), drawnCap);

    const auto nearer = [&](const std::size_t left, const std::size_t right)
    {
        const auto toLeft = snapshots[left].positionMetres - cameraMetres;
        const auto toRight = snapshots[right].positionMetres - cameraMetres;

        // Squared distance: the ordering is the same and the square root is not.
        return glm::dot(toLeft, toLeft) < glm::dot(toRight, toRight);
    };

    std::partial_sort(order.begin(), order.begin() + static_cast<std::ptrdiff_t>(kept), order.end(), nearer);

    taken.assign(slots.size(), false);

    for (auto index = std::size_t{0}; index < kept; index++)
    {
        const auto& snapshot = snapshots[order[index]];
        const auto level = levelFor(glm::distance(snapshot.positionMetres, cameraMetres));

        // The first free slot carrying this car's own body shape. A linear scan over a pool of a few
        // dozen, once per drawn car; a map would be an allocation to save nothing.
        for (auto candidate = std::size_t{0}; candidate < slots.size(); candidate++)
        {
            if (taken[candidate] || slots[candidate].body != snapshot.body)
            {
                continue;
            }

            taken[candidate] = true;
            show(slots[candidate], snapshot, level);
            live++;

            break;
        }
    }

    for (auto index = std::size_t{0}; index < slots.size(); index++)
    {
        if (!taken[index])
        {
            hide(slots[index]);
        }
    }

    placeLamps(snapshots);
}

// The lamps, dealt to the nearest patrol cars with their bars on (docs/police-lights-brief.md). One
// lamp per car, stood on the side of the bar that is lit this frame — the red side is the car's
// left — and no lamp at all in the dark between flashes: a lamp switched off is not uploaded, so the
// street pays for the flashes and not for the bars. Nearest first, in the order the pool was dealt
// in, because a lamp reaches thirty metres and the pool's reach is well past that.
void TrafficCars::placeLamps(const std::vector<TrafficSnapshot>& snapshots)
{
    auto next = std::size_t{0};
    const auto kept = std::min(order.size(), drawnCap);

    for (auto index = std::size_t{0}; index < kept && next < lamps.size(); index++)
    {
        const auto& snapshot = snapshots[order[index]];
        if (!snapshot.siren || snapshot.body >= lightBars.size() || !lightBars[snapshot.body].present)
        {
            continue;
        }

        const auto state = beaconPattern(clockSeconds + beaconPhaseSeconds(snapshot.id));
        auto& lamp = *lamps[next++];

        if (state.red <= 0.0f && state.blue <= 0.0f)
        {
            lamp.enabled = false;

            continue;
        }

        const auto& bar = lightBars[snapshot.body];
        const auto red = state.red > 0.0f;

        // Half way out along the lit side of the bar, a hand's breadth over it, so the lamp stands
        // over the roof rather than inside the housing.
        const auto onCar = glm::dvec3(bar.centreMetres) +
                           glm::dvec3((red ? 0.5 : -0.5) * static_cast<double>(bar.halfWidthMetres), 0.05, 0.0);
        const auto placed = toWorldUnits(snapshot.positionMetres + snapshot.orientation * onCar);

        // The irradiance is stated a metre out and the shader's inverse square runs in world units,
        // so the lamp carries it scaled by the square of the units per metre; the range likewise.
        const auto colour = (red ? beaconRed : beaconBlue) *
                            (beaconLampIrradianceAtOneMetre *
                             static_cast<float>(worldUnitsPerMetre * worldUnitsPerMetre));

        lamp.type = raceengine::LightType::Point;
        lamp.enabled = true;
        lamp.position =
            glm::vec3(static_cast<float>(placed.x), static_cast<float>(placed.y), static_cast<float>(placed.z));
        lamp.diffuse = colour;
        lamp.specular = colour;
        lamp.ambient = glm::vec3(0.0f);
        lamp.attenuation = static_cast<float>(beaconLampRangeMetres * worldUnitsPerMetre);
    }

    for (; next < lamps.size(); next++)
    {
        lamps[next]->enabled = false;
    }
}

} // namespace osr
