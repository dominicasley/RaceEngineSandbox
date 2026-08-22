module;

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
            raceengine::fail(loaded.error());
        }

        const auto model = std::move(loaded).value();

        const auto drawableComponent = engine.entity().addComponent<Drawable>(
            entity,
            engine.scene().createEntity(
                scene, CreateRenderableModelDTO{
                           .node = node, .shader = engine.shader().getShaderByName("pbr").value(), .model = model}));

        // The mesh is 200 across and the scene it has to carry is wider than that: the building
        // spans x -188..150 and z -72..201, and the car stands out on the apron beyond it. Three
        // covers all of it with margin. The asphalt stretches by the same factor — the UVs are
        // baked, so tiling it means a Mapping node on export, not a change here.
        //
        // Its textures are the 2048 ones history carries. The 4K re-export that was on disk before
        // 2026-08-19 is gone and exists nowhere on this machine, so this asset is what the golden
        // frame is blessed against; see *The asset that was lost* in CLAUDE.md.
        engine.sceneManager().setScale(node, 3.0f, 3.0f, 3.0f);
    }

private:
    Entity& entity;
    SceneNode& node;
};

} // namespace osr
