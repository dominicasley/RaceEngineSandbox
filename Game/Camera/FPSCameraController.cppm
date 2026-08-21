module;

#include <algorithm>
#include <cmath>

#include <glm/glm.hpp>

export module osr.game:FPSCameraController;

import raceengine;

namespace osr
{

export class FPSCameraController
{
    static constexpr double mouseSensitivity = 0.001;
    // Just under a right angle: at exactly +/-pi/2 the direction vector is parallel to the
    // world up axis and the view matrix has no defined roll.
    static constexpr double pitchLimit = 1.5533430342749532;

    raceengine::Engine& engine;
    // Where the camera is looking, and the only answer to that question: `update` writes the
    // direction from these on every tick, so a level that spawned its camera with `lookAtPoint`
    // would be overwritten by the first one. The level states the view here instead.
    double rotateX;
    double rotateY;
    glm::vec3 velocity = glm::vec3(0.0f);
    glm::vec3 acceleration = glm::vec3(0.0f);

public:
    // Yaw about the world up axis and pitch above the horizon, in radians. Zero yaw looks along
    // positive z, which is the direction the view matrix is built from.
    explicit FPSCameraController(raceengine::Engine& engine, double yaw = 0.0, double pitch = -0.75);
    // delta is the simulation tick, not the frame: movement integrates at a fixed rate
    // however fast the renderer happens to be running.
    void update(Camera& camera, float delta);
};

} // namespace osr

namespace osr
{

FPSCameraController::FPSCameraController(raceengine::Engine& engine, const double yaw, const double pitch) :
    engine(engine),
    rotateX(yaw),
    rotateY(std::clamp(pitch, -pitchLimit, pitchLimit))
{
    // Mouse-look needs motion the desktop cannot clamp at a screen edge.
    engine.window().setCursorMode(raceengine::CursorMode::Captured);
}

void FPSCameraController::update(Camera& camera, float delta)
{
    // The window reports motion, not position: it owns the captured cursor, so there is no
    // warp to centre here and no read-back to misinterpret.
    const auto [deltaX, deltaY] = engine.window().mouseDelta();

    auto rx = rotateX - mouseSensitivity * deltaX;
    auto ry = rotateY - mouseSensitivity * deltaY;

    // Looking straight up or down would make the direction vector degenerate against the
    // world up axis and the view matrix flip.
    ry = std::clamp(ry, -pitchLimit, pitchLimit);

    auto direction = glm::vec3(cos(ry) * sin(rx), sin(ry), cos(ry) * cos(rx));
    engine.camera().setDirection(camera, direction.x, direction.y, direction.z);
    rotateX = rx;
    rotateY = ry;

    auto right = glm::vec3(sin(rotateX - static_cast<double>(3.14f / 2.0f)), 0,
                           cos(rotateX - static_cast<double>(3.14f / 2.0f)));

    velocity *= 1 / (1 + (delta * 15.0f));
    acceleration = glm::vec3(10.0f * delta);

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
