# CMakeList.txt : CMake project for OpenStreetRace, include source and define
# project specific logic here.
#
cmake_minimum_required (VERSION 3.8)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_FLAGS_RELEASE "-O3")

link_directories(../Engine/vendor/FreeImage/bin)
link_libraries(FreeImage)

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
        Game/DotNetHost.cpp
        Game/DotNetHost.h
        Game/Entities/DinosaurEntity.cpp
        Game/Entities/DinosaurEntity.h
        Game/Entities/CarEntity.h
)

add_subdirectory(vendor/tweeny vendor/tweeny)

if (NOT TARGET spdlog)
    add_subdirectory(vendor/spdlog)
endif()

target_link_libraries(OpenStreetRace tweeny)
target_link_libraries(OpenStreetRace Engine)
target_link_libraries(OpenStreetRace spdlog)
target_include_directories(OpenStreetRace PUBLIC ../Engine)
include_directories(OpenStreetRace PUBLIC C:/Users/Dominic/Documents/dev/dotnet/runtime/src)

set (source "${CMAKE_CURRENT_SOURCE_DIR}/assets")
set (destination "${CMAKE_CURRENT_BINARY_DIR}/bin/assets")
add_custom_command(
        TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E create_symlink ${source} ${destination}
        DEPENDS ${destination}
        COMMENT "symbolic link assets folder from ${source} => ${destination}"
)
