#pragma once

#include <Engine.h>
#include "../Camera/FPSCameraController.h"
#include "../Entities/DinosaurEntity.h"

class WaterLevel
{
private:
    Engine& engine;
    Scene* scene;
    Camera* camera;
    FPSCameraController cameraController;
    RenderableEntity* player;
    RenderableEntity* cpu;
    RenderableEntity* ball;
    RenderableEntity* sky;
    std::unique_ptr<DinosaurEntity> dinosaurEntity;

    glm::vec3 velocity = glm::vec3(0.0f);
    glm::vec3 cpuVelocity = glm::vec3(0.0f);
    glm::vec3 acceleration = glm::vec3(0.0f);
    glm::vec3 ballVelocity = glm::vec3(0.0f, 0.0, 300.0f);

public:
    explicit WaterLevel(Engine& engine);
    void step();
};


