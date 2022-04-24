#pragma once

#include <Engine.h>
#include <Game/Components/Drawable.h>

class DinosaurEntity
{
protected:
    constexpr const static auto load = [](Engine& engine) {
        return ForkJoin::join(
            engine.resource.loadModelAsync("assets/Models/trex/source/trex.glb"),
            engine.resource.loadModelAsync("assets/Models/Box/Box.glb"),
            engine.resource.loadSkeletonAsync("assets/Models/trex/source/skeleton.ozz"),
            engine.resource.loadAnimationAsync("assets/Models/trex/source/Bip001_Take 001_BaseLayer.ozz")
        );
    };

public:
    DinosaurEntity(Engine& engine, Scene* scene) : entity(engine.entity.createEntity())
    {
        const auto [_, model, skeleton, animation] = load(engine);
        drawableComponent = engine.entity.addComponent<Drawable>(entity);

        drawableComponent->renderableEntity = engine.scene.createEntity(
            scene,
            CreateRenderableModelDTO{
                .node = engine.sceneManager.createNode(scene),
                .shader = engine.shader.getShaderByName("pbr").value(),
                .model = model
            });

        engine.sceneManager.setPosition(drawableComponent->renderableEntity->node, 40.0f, 0.0f, 0.0f);
        engine.sceneManager.setScale(drawableComponent->renderableEntity->node, 20.0f, 20.0f, 20.0f);
    }

private:
    Entity& entity;
    std::shared_ptr<Drawable> drawableComponent;
};


