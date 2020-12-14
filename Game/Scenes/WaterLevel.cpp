
#include "WaterLevel.h"

WaterLevel::WaterLevel(Engine& engine) : engine(engine), cameraController(engine)
{
    scene = engine.sceneManager.createScene("Water Level");
    camera = engine.scene.createCamera(scene);

    engine.camera.setPosition(camera, 0, 600, -450);
    engine.camera.setRoll(camera, 0, 1, 0);
    engine.camera.lookAtPoint(camera, 0, 0, 0);

    const auto[vert, frag, pbrFragmentShader, paddleModel, ballModel, water, playerModel, monsterSkeleton, monsterAnimation] = ForkJoin::join(
        engine.resource.loadTextFileAsync("assets/Shaders/PassThroughVertexShader.glsl"),
        engine.resource.loadTextFileAsync("assets/Shaders/PassThroughFragmentShader.glsl"),
        engine.resource.loadTextFileAsync("assets/Shaders/PbrFragmentShader.glsl"),
        engine.resource.loadModelAsync("assets/Models/Paddle/Paddle.glb"),
        engine.resource.loadModelAsync("assets/Models/Box/Box.glb"),
        engine.resource.loadModelAsync("assets/Models/water/water.glb"),
        engine.resource.loadModelAsync("assets/Models/trex/source/bawanglong.glb"),
        engine.resource.loadSkeletonAsync("assets/Models/trex/source/skeleton.ozz"),
        engine.resource.loadAnimationAsync("assets/Models/trex/source/Bip001_Take 001_BaseLayer.ozz")
    );

    auto shader = engine.shader.createShader(
        "basic",
        ShaderDescriptor {
            .vertexShaderSource = vert,
            .fragmentShaderSource = frag
        });

    auto pbrShader = engine.shader.createShader(
        "pbr",
        ShaderDescriptor {
            .vertexShaderSource = vert,
            .fragmentShaderSource = pbrFragmentShader
        });

    if (paddleModel)
    {
        engine.renderer.upload(paddleModel);

        player = engine.scene.createEntity(
            scene,
            RenderableEntityDesc {
                .model = paddleModel,
                .node = engine.sceneManager.createNode(scene),
                .shader = shader.value()
            });

        engine.sceneManager.setPosition(player->node, 0.0f, 0.0f, -300.0f);
        engine.sceneManager.setScale(player->node, 10.0f, 10.0f, 10.0f);

        cpu = engine.scene.createEntity(
            scene,
            RenderableEntityDesc {
                .model = paddleModel,
                .node = engine.sceneManager.createNode(scene),
                .shader = shader.value()
            });

        engine.sceneManager.setPosition(cpu->node, 0.0f, 0.0f, 300.0f);
        engine.sceneManager.setScale(cpu->node, 10.0f, 10.0f, 10.0f);
    }

    if (ballModel)
    {
        engine.renderer.upload(ballModel);

        ball = engine.scene.createEntity(
            scene,
            RenderableEntityDesc {
                .model = ballModel,
                .node = engine.sceneManager.createNode(scene),
                .shader = pbrShader.value()
            });

        engine.sceneManager.setPosition(ball->node, 0.0f, 0.0f, 0.0f);
        engine.sceneManager.setScale(ball->node, 10.0f, 10.0f, 10.0f);

        auto light = engine.scene.createEntity(
            scene,
            RenderableEntityDesc {
                .model = ballModel,
                .node = engine.sceneManager.createNode(scene),
                .shader = pbrShader.value()
            });

        engine.sceneManager.setPosition(light->node, 0.0f, 350.0f, 350.0f);
        engine.sceneManager.setScale(light->node, 2.0f, 2.0f, 2.0f);
    }

    if (playerModel)
    {
        engine.renderer.upload(playerModel);

        auto p = engine.scene.createEntity(
            scene,
            RenderableEntityDesc {
                .model = playerModel,
                .node = engine.sceneManager.createNode(scene),
                .shader = pbrShader.value()
            });

        engine.sceneManager.setPosition(p->node, 50.0f, 20.0f, 0.0f);
        engine.sceneManager.setScale(p->node, 0.25f, 0.25f, 0.25f);
        engine.entity.setSkeleton(p->meshes.front(), monsterSkeleton);
        engine.entity.addAnimation(p->meshes.front(), monsterAnimation);
        engine.entity.setAnimation(p->meshes.front(), 0);
    }

    if (water)
    {
        engine.renderer.upload(water);

        auto ground = engine.scene.createEntity(
            scene,
            RenderableEntityDesc {
                .model = water,
                .node = engine.sceneManager.createNode(scene),
                .shader = pbrShader.value()
            });

        ground->meshes.front().material->repeat = glm::vec2(10.0f, 10.0f);
        engine.sceneManager.setPosition(ground->node, 0.0f, -25.0f, 0.0f);
        engine.sceneManager.setScale(ground->node, 200.0f, 200.0, 200.0f);
    }
}

void WaterLevel::step()
{
    cameraController.update(camera);
}
