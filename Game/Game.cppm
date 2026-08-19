export module osr.game;

export import :Bollard;
export import :CarEntity;
export import :ChaseCameraController;
export import :DinosaurEntity;
// Nothing in this game constructs one any more — the scene is watched from the chase camera — and
// it stays built and exported because a free-fly view is the tool a scene is inspected with, not
// something this level happens to have finished with.
export import :FPSCameraController;
export import :PlayerCar;
export import :SteeringController;
export import :TrackFrame;
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
    // The process's exit status. The engine can end the run itself — the frame capture does —
    // and what it has to say about how that went reaches the operating system through here
    // rather than through a call to std::exit somewhere below.
    [[nodiscard]] int run();
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
int Game::run()
{
    while (engine.running())
    {
        engine.step();
    }

    return engine.status();
}

} // namespace osr
