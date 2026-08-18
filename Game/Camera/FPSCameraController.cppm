module;

#include <cmath>

#include <glm/glm.hpp>

export module osr.game:FPSCameraController;

import raceengine;

namespace osr
{

export class FPSCameraController
{
    raceengine::Engine& engine;
    bool firstUpdate = true;
    double mx = 0;
    double my = 0;
    double rotateX = 0;
    double rotateY = -0.75;
    glm::vec3 velocity = glm::vec3(0.0f);
    glm::vec3 acceleration = glm::vec3(0.0f);

public:
    explicit FPSCameraController(raceengine::Engine& engine);
    // delta is the simulation tick, not the frame: movement integrates at a fixed rate
    // however fast the renderer happens to be running.
    void update(Camera& camera, float delta);
};

} // namespace osr

namespace osr
{

FPSCameraController::FPSCameraController(raceengine::Engine& engine) :
    engine(engine)
{
}

void FPSCameraController::update(Camera& camera, float delta)
{
    auto state = engine.window().state();
    auto [cmx, cmy] = engine.window().mousePosition();
    engine.window().setMousePosition(state.windowWidth / 2, state.windowHeight / 2);

    // The first update has no previous cursor sample; without this guard the cursor's absolute
    // position becomes a huge one-off rotation (0.001 rad/px from an arbitrary origin).
    if (firstUpdate)
    {
        firstUpdate = false;
        mx = cmx;
        my = cmy;
    }

    auto rx = rotateX - static_cast<double>(0.001f) * (cmx - mx);
    auto ry = rotateY - static_cast<double>(0.001f) * (cmy - my);

    auto [pmx, pmy] = engine.window().mousePosition();
    mx = pmx;
    my = pmy;

    auto direction = glm::vec3(cos(ry) * sin(rx), sin(ry), cos(ry) * cos(rx));
    engine.camera().setDirection(camera, direction.x, direction.y, direction.z);
    rotateX = rx;
    rotateY = ry;

    auto right = glm::vec3(sin(rotateX - static_cast<double>(3.14f / 2.0f)), 0,
                           cos(rotateX - static_cast<double>(3.14f / 2.0f)));

    velocity *= 1 / (1 + (delta * 15.0f));
    acceleration = glm::vec3(300.0f * delta);

    if (engine.window().keyPressed(Key::D))
    {
        velocity += right * acceleration;
    }

    if (engine.window().keyPressed(Key::A))
    {
        velocity -= right * acceleration;
    }

    if (engine.window().keyPressed(Key::W))
    {
        velocity += direction * acceleration;
    }

    if (engine.window().keyPressed(Key::S))
    {
        velocity -= direction * acceleration;
    }

    engine.camera().translate(camera, velocity.x, velocity.y, velocity.z);
}

} // namespace osr
