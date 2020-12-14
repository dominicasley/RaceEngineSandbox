#pragma once

#include <chrono>
#include <Engine.h>
#include "Scenes/WaterLevel.h"

class Game
{
private:
    Engine engine;
    WaterLevel waterLevel;

public:
    explicit Game();
    void run();
};


