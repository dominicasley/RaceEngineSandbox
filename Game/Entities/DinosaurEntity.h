#pragma once

#include <Engine.h>
#include <Game/Components/Drawable.h>

class DinosaurEntity
{
protected:
    constexpr const static auto load = [](Engine& engine) {
        return ForkJoin::join(
            engine.resource.loadModelAsync("assets/Models/test.glb")
        );
    };

public:
    DinosaurEntity(Engine& engine, Scene& scene) : entity(engine.entity.createEntity()),
                                                   node(engine.sceneManager.createNode(scene))
    {
        const auto [model] = load(engine);

        const auto drawableComponent = engine.entity.addComponent<Drawable>(
            entity,
            engine.scene.createEntity(
                scene,
                CreateRenderableModelDTO {
                    .node = node,
                    .shader = engine.shader.getShaderByName("pbr").value(),
                    .model = model
                }
            )
        );

        engine.sceneManager.setScale(node, 10.0f, 10.0f, 10.0f);
    }

private:
    Entity& entity;
    SceneNode& node;
};


