module;

#include <stdexcept>
#include <utility>

export module osr.game:CarEntity;

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
            throw std::runtime_error(loaded.error());
        }

        const auto model = std::move(loaded).value();

        auto& renderable = engine.scene().createEntity(
            scene, CreateRenderableModelDTO{
                       .node = node, .shader = engine.shader().getShaderByName("pbr").value(), .model = model});

        // Traffic, not scenery: a light probe captures its environment once and is shaded from for
        // many frames afterwards, so a car baked into one would go on lighting the street from
        // wherever it was parked when the capture ran — including after it has driven away.
        renderable.staticGeometry = false;

        const auto drawableComponent = engine.entity().addComponent<Drawable>(entity, renderable);

        // Out in the sun, past the side of the building.
        //
        // The sun is at 45 degrees along +Z, so the building throws its shadow straight back along
        // -Z and the shadow is as wide as the building is: measured off the frame, x from about
        // -180 to +150, reaching to roughly z = -330. This sits clear of that on the sunlit side and
        // still well inside the ground plane, which narrows towards the camera.
        //
        // It is worth having one lit thing standing next to the shadow: the car takes direct sun
        // and a sky-lit indirect term while the ground a few metres away takes only the second,
        // which is the whole of what the probes changed, in one frame.
        engine.sceneManager().setPosition(node, 230.0f, 0.0f, -190.0f);

        // A world unit is a tenth of a metre here — the bollard is a metre tall at its scale of 10
        // and the camera stands at 32 — and this glTF is authored in *metres*: 2.05 x 1.48 x 4.31,
        // which is a Mk7 Golf to the centimetre. The Lotus it replaced was authored in centimetres
        // and so took 0.1 to reach the same place. Carrying that number over left the car a hundred
        // times too small, which reads as a missing asset rather than as a wrong scale: at 4 cm
        // long it is one white pixel beside a building.
        engine.sceneManager().setScale(node, 10.0f, 10.0f, 10.0f);
    }

private:
    Entity& entity;
    SceneNode& node;
};

} // namespace osr
