
#include "FPSCameraController.h"


FPSCameraController::FPSCameraController(Engine& engine) : engine(engine)
{

}

void FPSCameraController::update(Camera& camera)
{
    auto state = engine.window().state();
    auto [cmx, cmy] = engine.window().mousePosition();
    engine.window().setMousePosition(state.windowWidth / 2, state.windowHeight / 2);

    auto rx = rotateX - 0.001f * (cmx - mx);
    auto ry = rotateY - 0.001f * (cmy - my);

    auto[pmx, pmy] = engine.window().mousePosition();
    mx = pmx;
    my = pmy;

    auto direction = glm::vec3(cos(ry) * sin(rx), sin(ry), cos(ry) * cos(rx));
    engine.camera().setDirection(camera, direction.x, direction.y, direction.z);
    rotateX = rx;
    rotateY = ry;

    auto right = glm::vec3(
        sin(rotateX - 3.14f / 2.0f),
        0,
        cos(rotateX - 3.14f / 2.0f)
    );

    const auto delta = engine.window().delta();
    if (delta <= 0.0f)
    {
        return;
    }

    velocity *= 1 / (1 + (delta * 15.0f));
    acceleration = glm::vec3(300.0f * delta);

    if (engine.window().keyPressed(GLFW_KEY_D))
    {
        velocity += right * acceleration;
    }

    if (engine.window().keyPressed(GLFW_KEY_A))
    {
        velocity -= right * acceleration;
    }

    if (engine.window().keyPressed(GLFW_KEY_W))
    {
        velocity += direction * acceleration;
    }

    if (engine.window().keyPressed(GLFW_KEY_S))
    {
        velocity -= direction * acceleration;
    }

    engine.camera().translate(camera, velocity.x, velocity.y, velocity.z);
}