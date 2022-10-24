cmake_minimum_required(VERSION 3.10)

project(mbw VERSION 1.5)
add_compile_options(-O3 -mavx)

add_executable(mbw mbw.cpp)
add_executable(mulmcp mulmcp.cpp)
target_link_libraries(mbw pthread)
target_link_libraries(mulmcp pthread)
target_link_libraries(mulmcp numa)

install(TARGETS mbw DESTINATION bin)
