#pragma once

#include <Engine.h>
#include "../Camera/FPSCameraController.h"
#include "../Entities/DinosaurEntity.h"
#include "../Entities/CarEntity.h"
#include "../Entities/Bollard.h"

class WaterLevel
{
private:
    Engine& engine;
    Scene& scene;
    Camera& camera;
    FPSCameraController cameraController;

    RenderableModel* sky;

    glm::vec3 velocity = glm::vec3(0.0f);
    glm::vec3 cpuVelocity = glm::vec3(0.0f);
    glm::vec3 acceleration = glm::vec3(0.0f);
    glm::vec3 ballVelocity = glm::vec3(0.0f, 0.0, 300.0f);

public:
    explicit WaterLevel(Engine& engine);
    void step();
};


