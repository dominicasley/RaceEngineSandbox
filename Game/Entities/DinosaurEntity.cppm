module;

#include <stdexcept>
#include <utility>

export module osr.game:DinosaurEntity;

import raceengine;

namespace osr
{

// Constant-rate yaw on the fixed tick. The rate is per simulated second, so the entity turns
// at the same speed whatever the frame rate is — which is the property the fixed timestep
// exists to give, and the reason this is here rather than in the frame loop.
//
// State lives in the component, which the entity owns: the builders below are temporaries in
// the level's constructor, so a behaviour that captured one would outlive it.
export class Spin : public Behaviour
{
private:
    raceengine::SceneManagerService& sceneManagerService;
    SceneNode& node;
    float degreesPerSecond;

public:
    explicit Spin(raceengine::SceneManagerService& sceneManagerService, SceneNode& node, float degreesPerSecond) :
        sceneManagerService(sceneManagerService),
        node(node),
        degreesPerSecond(degreesPerSecond)
    {
    }

    void update(float delta) override
    {
        sceneManagerService.rotate(node, degreesPerSecond * delta, 0.0f, 1.0f, 0.0f);
    }
};

export class DinosaurEntity
{
protected:
    constexpr const static auto load = [](raceengine::Engine& engine)
    {
        return engine.resource().loadModelAsync("assets/Models/test.glb").get();
    };

public:
    DinosaurEntity(raceengine::Engine& engine, Scene& scene) :
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

        engine.sceneManager().setScale(node, 10.0f, 10.0f, 10.0f);

        // 20 deg/s: a turn every 18 seconds. This is the only entity the spawn view frames,
        // so it is the one that makes the update phase visible in a capture.
        engine.entity().addComponent<Spin>(entity, engine.sceneManager(), node, 20.0f);
    }

private:
    Entity& entity;
    SceneNode& node;
};

} // namespace osr
