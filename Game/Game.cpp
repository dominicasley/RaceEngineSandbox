
#include "Game.h"
#include "DotNetHost.h"

Game::Game() : waterLevel(engine)
{
    DotNetHost host;
}

void Game::run()
{
    while (engine.running())
    {
        waterLevel.step();
        engine.step();
    }
}
