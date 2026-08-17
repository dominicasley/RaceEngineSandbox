#include <filesystem>

import osr.game;

int main(int, char** argv)
{
    std::filesystem::current_path(std::filesystem::path(argv[0]).parent_path());

    osr::Game game;
    game.run();

    return 0;
}
