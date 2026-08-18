module;

#include <stdexcept>
#include <utility>

export module osr.game:GroundPlane;

import raceengine;

namespace osr
{

export class GroundPlane
{
protected:
    constexpr const static auto load = [](raceengine::Engine& engine)
    {
        return engine.resource().loadModelAsync("assets/Models/plane.glb").get();
    };

public:
    GroundPlane(raceengine::Engine& engine, Scene& scene) :
        entity(engine.entity().createEntity()),
        node(engine.sceneManager().createNode(scene))
    {
        auto loaded = load(engine);
        if (!loaded)
        {
            throw std::runtime_error(loaded.error());
        }

        const auto model = std::move(loaded).value();

        const auto drawableComponent = engine.entity().addComponent<Drawable>(
            entity,
            engine.scene().createEntity(
                scene, CreateRenderableModelDTO{
                           .node = node, .shader = engine.shader().getShaderByName("pbr").value(), .model = model}));

        // The mesh is 200 across and the scene it has to carry is wider than that: the building
        // spans x -188..150 and z -72..201, and the car stands at z -130. Three covers all of it
        // with margin. The asphalt stretches by the same factor — the UVs are baked, so tiling it
        // means a Mapping node on export, not a change here.
        engine.sceneManager().setScale(node, 3.0f, 3.0f, 3.0f);
    }

private:
    Entity& entity;
    SceneNode& node;
};

} // namespace osr
