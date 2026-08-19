# CMakeList.txt : CMake project for OpenStreetRace, include source and define
# project specific logic here.
#
cmake_minimum_required (VERSION 3.28)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_FLAGS_RELEASE "-O3")

add_definitions(-D_SILENCE_ALL_MS_EXT_DEPRECATION_WARNINGS)

# Add source to this project's executable.
set(EXECUTABLE_OUTPUT_PATH "bin")
add_executable (OpenStreetRace OpenStreetRace.cpp)
target_sources(OpenStreetRace PRIVATE
        FILE_SET CXX_MODULES
        BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
        FILES
        Game/Camera/ChaseCameraController.cppm
        Game/Camera/FPSCameraController.cppm
        Game/Entities/Bollard.cppm
        Game/Entities/CarEntity.cppm
        Game/Entities/DinosaurEntity.cppm
        Game/Entities/GroundPlane.cppm
        Game/Game.cppm
        Game/Options.cppm
        Game/Scenes/ApronScene.cppm
        Game/Scenes/CircuitScene.cppm
        Game/Scenes/RenderRig.cppm
        Game/Track/RaceTrack.cppm
        Game/Vehicle/PlayerCar.cppm
        Game/Vehicle/SteeringController.cppm
        Game/Vehicle/TrackFrame.cppm)

find_package(spdlog CONFIG REQUIRED)

target_link_libraries(OpenStreetRace Engine)
target_link_libraries(OpenStreetRace spdlog::spdlog)
target_link_libraries(OpenStreetRace raceengine_warnings)

set (source "${CMAKE_CURRENT_SOURCE_DIR}/assets")
set (destination "${CMAKE_CURRENT_BINARY_DIR}/bin/assets")
add_custom_command(
        TARGET OpenStreetRace POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E create_symlink ${source} ${destination}
        DEPENDS ${destination}
        COMMENT "symbolic link assets folder from ${source} => ${destination}"
)
