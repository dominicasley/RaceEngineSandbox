module;

#include <stdexcept>
#include <utility>

export module osr.game:DinosaurEntity;

import raceengine;

namespace osr
{

export class DinosaurEntity
{
protected:
    constexpr const static auto load = [](raceengine::Engine& engine) {
        return engine.resource().loadModelAsync("assets/Models/test.glb").get();
    };

public:
    DinosaurEntity(raceengine::Engine& engine, Scene& scene) : entity(engine.entity().createEntity()),
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
                scene,
                CreateRenderableModelDTO {
                    .node = node,
                    .shader = engine.shader().getShaderByName("pbr").value(),
                    .model = model
                }
            )
        );

        engine.sceneManager().setScale(node, 10.0f, 10.0f, 10.0f);
    }

private:
    Entity& entity;
    SceneNode& node;
};

} // namespace osr
