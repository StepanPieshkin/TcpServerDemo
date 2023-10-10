# CMakeList.txt : CMake project for Server, include source and define
# project specific logic here.
#
cmake_minimum_required (VERSION 3.8)

set(ASIO_NO_DEPRECATED TRUE)
#add_compile_options(/permissive- /Zc:externConstexpr)

#find_package(asio CONFIG REQUIRED)
find_package(boost REQUIRED COMPONENTS asio)
include_directories(${Boost_INCLUDE_DIRS})
#find_package(unifex CONFIG REQUIRED)

# Add source to this project's executable.
add_executable (Server "Server.cxx")
target_link_libraries(Server ${Boost_LIBRARIES})#asio) #unofficial::unifex)

# TODO: Add tests and install targets if needed.