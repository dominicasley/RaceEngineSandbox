# CMakeList.txt : CMake project for OpenStreetRace, include source and define
# project specific logic here.
#
cmake_minimum_required (VERSION 3.8)

# Add source to this project's executable.
add_executable (OpenStreetRace "OpenStreetRace.cpp" "OpenStreetRace.h")

target_link_libraries(OpenStreetRace Engine glfw3 vulkan-1)

target_include_directories(OpenStreetRace PUBLIC "../Middleware/glfw/include/")
target_link_directories(OpenStreetRace PUBLIC "../Middleware/glfw/lib-vc2019/")

# TODO: Add tests and install targets if needed.
