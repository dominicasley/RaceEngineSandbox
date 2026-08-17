export module osr.game;

export import :Bollard;
export import :CarEntity;
export import :DinosaurEntity;
export import :FPSCameraController;
export import :WaterLevel;

import raceengine;

namespace osr
{

export class Game
{
private:
    raceengine::Engine engine;
    WaterLevel waterLevel;

public:
    explicit Game();
    void run();
};

} // namespace osr

module :private;

namespace osr
{

Game::Game() :
    waterLevel(engine)
{
}

void Game::run()
{
    while (engine.running())
    {
        waterLevel.step();
        engine.step();
    }
}

} // namespace osr
