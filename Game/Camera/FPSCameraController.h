#pragma once

#include <Engine.h>

class FPSCameraController
{
    Engine& engine;
    double mx = 0;
    double my = 0;
    double rotateX = 0;
    double rotateY = 0;
    glm::vec3 velocity = glm::vec3(0.0f);
    glm::vec3 acceleration = glm::vec3(0.0f);

public:
    explicit FPSCameraController(Engine& engine);
    void update(Camera* camera);
};


