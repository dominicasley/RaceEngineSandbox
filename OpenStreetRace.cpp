#include <filesystem>
#include "Game/Game.h"

int main(int argc, char **argv)
{
    std::filesystem::current_path(std::filesystem::path(argv[0]).parent_path());

    Game game;
    game.run();

    return 0;
}
