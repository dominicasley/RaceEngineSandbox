#pragma once

#include <stdexcept>
#include <Engine.h>
#include <Game/Components/Drawable.h>

class Bollard
{
protected:
    constexpr const static auto load = [](Engine& engine) {
        return engine.resource().loadModelAsync("assets/Models/bollard.glb").get();
    };

public:
    Bollard(Engine& engine, Scene& scene) :
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
                scene,
                CreateRenderableModelDTO {
                    .node = node,
                    .shader = engine.shader().getShaderByName("pbr").value(),
                    .model = model
                }
            )
        );

        engine.sceneManager().setPosition(node, 10.0f, 0.0f, 10.0f);
        engine.sceneManager().setScale(node, 10.0f, 10.0f, 10.0f);
    }

private:
    Entity& entity;
    SceneNode& node;
};


