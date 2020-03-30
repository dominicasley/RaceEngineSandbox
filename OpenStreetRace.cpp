#include "OpenStreetRace.h"

int main() {
    Engine engine;

    auto scene = engine.sceneManager.createScene("game");

    auto camera = scene->createCamera();
    camera->setPosition(0, 100, 100);
    camera->setDirection(0, 0, 0);
    camera->setRoll(0, 1, 0);

    const auto[vert, frag, duckModel, speechBubbleModel, waterModel] = ForkJoin::join(
            engine.resourceService.loadTextFileAsync("Assets/Shaders/PassThroughVertexShader.glsl"),
            engine.resourceService.loadTextFileAsync("Assets/Shaders/PassThroughFragmentShader.glsl"),
            engine.resourceService.loadModelAsync("Assets/Models/Duck/glTF-Binary/Duck.glb"),
            engine.resourceService.loadModelAsync("Assets/Models/speech_bubble/speech_bubble.glb"),
            engine.resourceService.loadModelAsync("Assets/Models/water/water.glb")
    );

    Shader shader;
    shader.vertexShaderSource = vert;
    shader.fragmentShaderSource = frag;
    engine.renderer.createShaderObject(shader);

    engine.renderer.upload(duckModel);
    engine.renderer.upload(speechBubbleModel);
    engine.renderer.upload(waterModel);

    auto duck = scene->createEntity(RenderableEntityDesc {
        duckModel
    });

    auto speechBubble = scene->createEntity(RenderableEntityDesc {
        speechBubbleModel
    });

    auto water = scene->createEntity(RenderableEntityDesc {
        waterModel
    });

    duck->setPosition(0.2f, 0.2f, 0.2f);
    duck->setScale(0.2f, 0.2f, 0.2f);
    speechBubble->setPosition(186, 145, -60);
    speechBubble->setScale(50.0f, 50.0f, 50.0f);

    speechBubble->setParent(duck);

    water->setPosition(0, 0, 0);
    water->setScale(300, 300, 300);

    auto i = 0;
    while (engine.running())
    {
        i++;
        duck->transform(0, sin(i / 30.0f) / 10.0f, 0);
        duck->setDirection(sin(i / 30.0f) / 10.0f + glm::radians(10.0f), 0, 0, 1);
        //duck->rotate(glm::radians(1.0f), 0, 1, 0);

        if (engine.window.getKeyPressed(GLFW_KEY_D)) {
            duck->transform(1, 0, 0);
            duck->rotate(glm::radians(0.0f), 0, 1, 0);
        }

        if (engine.window.getKeyPressed(GLFW_KEY_A)) {
            duck->transform(-1, 0, 0);
            duck->rotate(glm::radians(180.0f), 0, 1, 0);
        }

        if (engine.window.getKeyPressed(GLFW_KEY_W)) {
            duck->transform(0, 0, -1);
            duck->rotate(glm::radians(90.0f), 0, 1, 0);
        }

        if (engine.window.getKeyPressed(GLFW_KEY_S)) {
            duck->transform(0, 0, 1);
            duck->rotate(glm::radians(270.0f), 0, 1, 0);
        }

        engine.step();
    }

    return 0;
}
