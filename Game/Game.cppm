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

// The level registered its update with the engine when it was built, so the loop no longer
// carries an ordering contract of its own: the engine runs game logic on its fixed tick, at a
// point of its choosing, and a frame renders the state the tick before it produced.
void Game::run()
{
    while (engine.running())
    {
        engine.step();
    }
}

} // namespace osr
