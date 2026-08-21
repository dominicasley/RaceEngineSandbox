module;

#include <optional>

export module osr.game;

export import :ApronScene;
export import :Bollard;
export import :CarEntity;
export import :ChaseCameraController;
export import :CockpitCameraController;
export import :CircuitScene;
export import :DinosaurEntity;
export import :FPSCameraController;
export import :GroundPlane;
export import :Options;
export import :PlayerCar;
export import :RenderRig;
export import :SimulatedCar;
export import :Simulation;
export import :SteeringController;
export import :TrackFrame;

import raceengine;

namespace osr
{

export class Game
{
private:
    raceengine::Engine engine;
    // Exactly one of these is built, chosen off `OSR_SCENE`. Two scenes rather than two cameras
    // because a gate is only insensitive to something it does not contain: the apron has no physics
    // world, no vehicle and no driveline, so nothing about the vehicle model can reach its frame.
    // They are declared after the engine and destroyed before it, which is the same rule the
    // engine's own member list runs on.
    std::optional<CircuitScene> circuit;
    std::optional<ApronScene> apron;

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

// The whole of the run's configuration is read and checked here, ahead of the world it configures:
// a contradiction between two of the variables is refused before anything has been loaded, rather
// than by whichever scene happened to read its own knob.
Game::Game()
{
    const auto options = runOptions();

    if (options.scene == SceneChoice::Apron)
    {
        apron.emplace(engine);
    }
    else
    {
        circuit.emplace(engine, options);
    }
}

// The scene registered its update with the engine when it was built, so the loop no longer
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
