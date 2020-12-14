
#include "Game.h"

Game::Game() : waterLevel(engine)
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
