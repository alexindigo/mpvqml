cmake_minimum_required(VERSION 3.16)
project(mpvqml_install_consumers LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Gui Qml Test)
find_package(mpvqml 0.3 CONFIG REQUIRED PATHS "${PROBE_PREFIX}" NO_DEFAULT_PATH)

add_executable(module-consumer install-module-consumer.cpp)
target_link_libraries(module-consumer PRIVATE Qt6::Gui Qt6::Qml Qt6::Test)

add_executable(header-consumer installed-consumer.cpp)
target_link_libraries(header-consumer PRIVATE mpvqml::mpvqml)
