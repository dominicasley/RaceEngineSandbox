#include <cstdio>
#include <exception>
#include <expected>
#include <filesystem>
#include <string>

import osr.game;

// The process's one exception boundary. Everything below it — the engine's bring-up, the level's
// orThrow, the Vulkan backend's ~120 `ensure()` sites, every `.at()` and every `.value()` — has a
// way to fail that unwinds, and without a handler here that unwind reaches the end of main and
// becomes std::terminate: no message, no exit status anyone can read, and a core file if the
// system is configured for one. Reporting goes to stderr rather than to the engine's logger,
// because by the time an exception arrives here the Engine that owned the logger has already been
// destroyed, or was never constructed.
//
// std::bad_expected_access is caught on its own: its what() says only that an expected held an
// error, while the error itself is the sentence describing what happened.
int main(int, char** argv)
{
    try
    {
        std::filesystem::current_path(std::filesystem::path(argv[0]).parent_path());

        osr::Game game;

        return game.run();
    }
    catch (const std::bad_expected_access<std::string>& unhandled)
    {
        std::fprintf(stderr, "OpenStreetRace stopped: %s\n", unhandled.error().c_str());
    }
    catch (const std::exception& unhandled)
    {
        std::fprintf(stderr, "OpenStreetRace stopped: %s\n", unhandled.what());
    }
    catch (...)
    {
        std::fprintf(stderr, "OpenStreetRace stopped: an exception of unknown type reached main\n");
    }

    return 1;
}
