cmake_minimum_required(VERSION 3.16)
project(mpvqml LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Baseline dependencies
find_package(Qt6 REQUIRED COMPONENTS Core Quick)
qt_policy(SET QTP0001 NEW)

find_package(MpvQt REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(MPV REQUIRED mpv)

include(GNUInstallDirs)

# ==================== PRODUCTION QML MODULE ====================
qt_add_qml_module(mpvqml
    URI MpvQml
    VERSION 1.0
    OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/MpvQml
    INSTALL_DIRECTORY ${CMAKE_INSTALL_LIBDIR}/qt6/qml/MpvQml
    SOURCES
        mpvplayer.cpp
        mpvplayer.h
        mpvcontroller_adapter.cpp
        mpvcontroller_adapter.h
        mpvcontroller_iface.h
        mpvlogclient.cpp
        mpvlogclient.h
        mpvhookmanager.cpp
        mpvhookmanager.h
)

target_include_directories(mpvqml PUBLIC ${MPV_INCLUDE_DIRS})
target_link_libraries(mpvqml PRIVATE
    Qt6::Core
    Qt6::Quick
    MpvQt::MpvQt
    ${MPV_LIBRARIES}
)

# ==================== INSTALL TARGETS ====================
install(FILES
    mpvplayer.h
    mpvcontroller_adapter.h
    mpvcontroller_iface.h
    mpvlogclient.h
    mpvhookmanager.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/mpvqml
)

# ==================== AUTOMATED TESTING TARGETS ====================
option(BUILD_TEST_MODE "Build the internal C++ test executables" ON)

if(BUILD_TEST_MODE)
  find_package(Qt6 REQUIRED COMPONENTS Test)

  add_executable(mpvqml_cpp_tests
        tests/test_mpvplayer.cpp
        tests/mock_mpvcontroller.cpp
    )

  target_include_directories(mpvqml_cpp_tests PUBLIC ${MPV_INCLUDE_DIRS} .)
  target_link_libraries(mpvqml_cpp_tests PRIVATE
        mpvqml
        Qt6::Core
        Qt6::Quick
        Qt6::Test
        MpvQt::MpvQt
        ${MPV_LIBRARIES}
    )

  enable_testing()
  add_test(NAME mpvqml_cpp_tests COMMAND mpvqml_cpp_tests)
endif()

# ==================== THE INSTRUMENTATION MATRIX ====================
option(ENABLE_COVERAGE "Instrument targets for code coverage collection" OFF)

if(ENABLE_COVERAGE)
  message(STATUS "Enabling code coverage flags")

  # 1. Profile the QML module shared library
  target_compile_options(mpvqml PRIVATE --coverage -fprofile-abs-path)
  target_link_options(mpvqml PRIVATE --coverage -fprofile-abs-path)

  if(TARGET mpvqml_cpp_tests)
    # 2. Profile the C++ test binary runner
    target_compile_options(mpvqml_cpp_tests PRIVATE --coverage -fprofile-abs-path)
    target_link_options(mpvqml_cpp_tests PRIVATE --coverage -fprofile-abs-path)
  endif()
endif()


