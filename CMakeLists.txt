# CMakeList.txt : CMake project for OpenStreetRace, include source and define
# project specific logic here.
#
cmake_minimum_required (VERSION 3.28)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_FLAGS_RELEASE "-O3")

add_definitions(-D_SILENCE_ALL_MS_EXT_DEPRECATION_WARNINGS)

# Add source to this project's executable.
set(EXECUTABLE_OUTPUT_PATH "bin")
add_executable (
        OpenStreetRace
        OpenStreetRace.cpp
        Game/Game.cpp
        Game/Game.h
        Game/Scenes/WaterLevel.cpp
        Game/Scenes/WaterLevel.h
        Game/Camera/FPSCameraController.cpp
        Game/Camera/FPSCameraController.h
        Game/Entities/DinosaurEntity.cpp
        Game/Entities/DinosaurEntity.h
        Game/Entities/CarEntity.h
)

find_package(spdlog CONFIG REQUIRED)

target_link_libraries(OpenStreetRace Engine)
target_link_libraries(OpenStreetRace spdlog::spdlog)
target_link_libraries(OpenStreetRace raceengine_warnings)
target_include_directories(OpenStreetRace PUBLIC ../Engine)

set (source "${CMAKE_CURRENT_SOURCE_DIR}/assets")
set (destination "${CMAKE_CURRENT_BINARY_DIR}/bin/assets")
add_custom_command(
        TARGET OpenStreetRace POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E create_symlink ${source} ${destination}
        DEPENDS ${destination}
        COMMENT "symbolic link assets folder from ${source} => ${destination}"
)
