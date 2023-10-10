# CMakeList.txt : CMake project for Server, include source and define
# project specific logic here.
#
cmake_minimum_required (VERSION 3.8)

set(ASIO_NO_DEPRECATED TRUE)
find_package(asio CONFIG REQUIRED)

# Add source to this project's executable.
add_executable (Client "Client.cxx")
target_link_libraries(Client asio)

# TODO: Add tests and install targets if needed.
