module;

#include <utility>

#include <glm/glm.hpp>

export module osr.game:CarEntity;

import :RenderRig;

import raceengine;

namespace osr
{

export class CarEntity
{
protected:
    constexpr const static auto load = [](raceengine::Engine& engine)
    {
        return engine.resource().loadModelAsync("assets/Models/golf_gti_2018.glb").get();
    };

public:
    CarEntity(raceengine::Engine& engine, Scene& scene) :
        entity(engine.entity().createEntity()),
        node(engine.sceneManager().createNode(scene))
    {
        auto loaded = load(engine);
        if (!loaded)
        {
            raceengine::fail(loaded.error());
        }

        const auto model = std::move(loaded).value();

        auto& created = engine.scene().createEntity(
            scene, CreateRenderableModelDTO{
                       .node = node, .shader = engine.shader().getShaderByName("pbr").value(), .model = model});
        renderable = &created;

        // Traffic, not scenery: a light probe captures its environment once and is shaded from for
        // many frames afterwards, so a car baked into one would go on lighting the street from
        // wherever it was parked when the capture ran — including after it has driven away.
        created.staticGeometry = false;

        // The car layer, which is the one layer statement in the whole game: everything else is
        // born on worldLayer, and the layered frame draws this renderable through the car camera
        // instead — its own buffer, its own meter, composited over the finished world.
        created.layers = carLayer;

        // **The paint, which is this car's and not the model's.** It rides the renderable, so this is
        // the whole of recolouring a car: no material is looked up, nothing inside the asset is
        // touched, and a second car built from the same model would state its own.
        //
        // VW **LD1B Ginster Yellow**, the paint chip linearised and pulled a little under the raw
        // conversion — a base coat at the sRGB value's face reads as brighter than any real pigment
        // reflects. It is a **solid** colour, which is why there is no flake in it: Ginster is not a
        // metallic, and dialling flake into it would make it a colour Volkswagen never sold.
        //
        // The lacquer over the top is what makes it a car rather than a yellow object — a sharp
        // second highlight on a Fresnel of 0.04 — and a little orange peel, because a sprayed and
        // baked panel never comes out dead flat and the absence of it is one of the things that reads
        // as "rendered".
        created.paint = raceengine::Paint{.enabled = true,
                                          .colour = glm::vec3(0.75f, 0.47f, 0.02f),
                                          .flakeDensity = 0.0f,
                                          .clearcoat = 1.0f,
                                          .clearcoatRoughness = 0.04f,
                                          .orangePeel = 0.35f,
                                          .orangePeelScale = 30.0f};

        const auto drawableComponent = engine.entity().addComponent<Drawable>(entity, created);

        // Where it stands is not this entity's: on the circuit the vehicle model writes the node
        // from the first tick, and on the apron the scene places it. Stating a position here would
        // be a third answer that one of them overwrites.

        // A world unit is a tenth of a metre here — the bollard is a metre tall at its scale of 10
        // and the camera stands at 32 — and this glTF is authored in *metres*: 2.05 x 1.48 x 4.31,
        // which is a Mk7 Golf to the centimetre. The Lotus it replaced was authored in centimetres
        // and so took 0.1 to reach the same place. Carrying that number over left the car a hundred
        // times too small, which reads as a missing asset rather than as a wrong scale: at 4 cm
        // long it is one white pixel beside a building.
        engine.sceneManager().setScale(node, 10.0f, 10.0f, 10.0f);
    }

    // The node this car is drawn through, so that something else can place or drive it. The scale
    // and the entity's own flags stay this entity's.
    [[nodiscard]] SceneNode& sceneNode() const
    {
        return node;
    }

    // The renderable itself, so the game can drive a named part of the model — the steering wheel
    // is the first — through its per-instance mesh transforms.
    [[nodiscard]] RenderableModel& renderableModel() const
    {
        return *renderable;
    }

private:
    Entity& entity;
    SceneNode& node;
    RenderableModel* renderable = nullptr;
};

} // namespace osr
