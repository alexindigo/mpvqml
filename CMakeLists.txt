cmake_minimum_required(VERSION 3.16)
project(mpvqml VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Baseline dependencies
find_package(Qt6 REQUIRED COMPONENTS Core Gui Quick OpenGL)
qt_policy(SET QTP0001 NEW)

find_package(PkgConfig REQUIRED)
pkg_check_modules(MPV REQUIRED IMPORTED_TARGET mpv)

include(GNUInstallDirs)

# ==================== PRODUCTION QML MODULE ====================
qt_add_qml_module(mpvqml
    URI MpvQml
    VERSION 1.0
    OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/MpvQml
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
        mpvcontroller.cpp
        mpvcontroller_p.h
        mpvrenderbridge.cpp
        mpvrenderbridge_p.h
        mpvrenderer.cpp
        mpvrenderer_p.h
)

set_target_properties(mpvqml PROPERTIES VERSION 0.2.0 SOVERSION 0.2)

target_include_directories(mpvqml PUBLIC ${MPV_INCLUDE_DIRS})
target_link_libraries(mpvqml PUBLIC
    Qt6::Core
    Qt6::Quick
    Qt6::Gui
    PRIVATE
    Qt6::OpenGL
    PkgConfig::MPV
    ${CMAKE_DL_LIBS}
)

# ==================== INSTALL TARGETS ====================
install(TARGETS mpvqml
    EXPORT mpvqmlTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # The module plugin and backing library share the chosen prefix. Only the
    # installed plugin needs this relative path; build-tree search stays local.
    set_target_properties(mpvqmlplugin PROPERTIES INSTALL_RPATH "$ORIGIN/../../..")
endif()
install(TARGETS mpvqmlplugin
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/qt6/qml/MpvQml
    RUNTIME DESTINATION ${CMAKE_INSTALL_LIBDIR}/qt6/qml/MpvQml
)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/MpvQml/qmldir
    ${CMAKE_CURRENT_BINARY_DIR}/MpvQml/mpvqml.qmltypes
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/qt6/qml/MpvQml
)

install(FILES
    mpvplayer.h
    mpvcontroller_iface.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/mpvqml
)

install(FILES UPSTREAM.md LICENSE LICENSES/LGPL-3.0-only.txt
    DESTINATION ${CMAKE_INSTALL_DATADIR}/mpvqml/licenses
)
install(FILES CMakeLists.txt README.md API.md Architecture.md UPSTREAM.md LICENSE
    mpvplayer.cpp mpvplayer.h mpvcontroller_iface.h
    mpvcontroller_adapter.cpp mpvcontroller_adapter.h
    mpvlogclient.cpp mpvlogclient.h mpvhookmanager.cpp mpvhookmanager.h
    mpvcontroller.cpp mpvcontroller_p.h mpvrenderbridge.cpp mpvrenderbridge_p.h
    mpvrenderer.cpp mpvrenderer_p.h
    DESTINATION ${CMAKE_INSTALL_DATADIR}/mpvqml/source
)
install(FILES LICENSES/LGPL-3.0-only.txt
    DESTINATION ${CMAKE_INSTALL_DATADIR}/mpvqml/source/LICENSES
)

# ==================== AUTOMATED TESTING TARGETS ====================
option(BUILD_TEST_MODE "Build the internal C++ test executables" ON)

if(BUILD_TEST_MODE)
  find_package(Qt6 REQUIRED COMPONENTS Test Network)
  enable_language(C)

  # Filesystem ownership for the QML suite stays in a test-only module.
  qt_add_qml_module(mpvqml_test_support
      URI MpvQml.TestSupport
      VERSION 1.0
      OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/MpvQml/TestSupport
      SOURCES tests/temporary_directory.h tests/test_window.h
  )
  target_include_directories(mpvqml_test_support PRIVATE tests)
  target_link_libraries(mpvqml_test_support PRIVATE Qt6::Core Qt6::Qml Qt6::Quick)

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
        PkgConfig::MPV
    )

  enable_testing()
  add_test(NAME mpvqml_cpp_tests COMMAND mpvqml_cpp_tests)

  foreach(test_name IN ITEMS controller retirement renderer facade helpers)
    add_executable(mpvqml_${test_name}_tests tests/test_${test_name}.cpp)
    target_include_directories(mpvqml_${test_name}_tests PRIVATE . ${MPV_INCLUDE_DIRS})
    target_link_libraries(mpvqml_${test_name}_tests PRIVATE mpvqml Qt6::Core Qt6::Test ${MPV_LIBRARIES})
    add_test(NAME mpvqml_${test_name}_tests COMMAND mpvqml_${test_name}_tests)
  endforeach()
  target_link_libraries(mpvqml_renderer_tests PRIVATE Qt6::Gui Qt6::Quick Qt6::OpenGL)
  target_link_libraries(mpvqml_facade_tests PRIVATE Qt6::Gui Qt6::Quick Qt6::Network)
  target_sources(mpvqml_facade_tests PRIVATE tests/mock_mpvcontroller.cpp tests/mock_mpvcontroller.h)
  target_compile_definitions(mpvqml_facade_tests PRIVATE MPVQML_EXAMPLES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/examples")
  target_link_libraries(mpvqml_helpers_tests PRIVATE Qt6::Gui Qt6::Quick)
  target_compile_definitions(mpvqml_helpers_tests PRIVATE
      MPVQML_HELPER_VIDEO="${CMAKE_CURRENT_SOURCE_DIR}/tests/assets/test_one_video.mp4")
  target_compile_definitions(mpvqml_renderer_tests PRIVATE
      MPVQML_TEST_VIDEO="${CMAKE_CURRENT_SOURCE_DIR}/tests/assets/test_one_video.mp4"
      MPVQML_TEST_RED_VIDEO="${CMAKE_CURRENT_SOURCE_DIR}/tests/assets/test_two_video.mp4")

  add_library(mpvqml_test_faults SHARED tests/faults.c)
  set_target_properties(mpvqml_test_faults PROPERTIES AUTOMOC OFF C_STANDARD 11 C_STANDARD_REQUIRED ON)
  target_include_directories(mpvqml_test_faults PRIVATE ${MPV_INCLUDE_DIRS})
  target_link_libraries(mpvqml_test_faults PRIVATE ${CMAKE_DL_LIBS} pthread)
  add_executable(mpvqml_crash_tests tests/test_crash_lifecycle.cpp)
  target_include_directories(mpvqml_crash_tests PRIVATE . ${MPV_INCLUDE_DIRS})
  target_link_libraries(mpvqml_crash_tests PRIVATE mpvqml Qt6::Core Qt6::Test ${MPV_LIBRARIES} ${CMAKE_DL_LIBS})
  target_compile_definitions(mpvqml_crash_tests PRIVATE MPVQML_FAULT_LIBRARY="$<TARGET_FILE:mpvqml_test_faults>")
  target_compile_definitions(mpvqml_crash_tests PRIVATE
      MPVQML_CRASH_VIDEO="${CMAKE_CURRENT_SOURCE_DIR}/tests/assets/test_one_video.mp4")
  add_library(mpvqml_test_unload_consumer SHARED tests/unload-consumer.cpp)
  set_target_properties(mpvqml_test_unload_consumer PROPERTIES AUTOMOC OFF)
  target_compile_options(mpvqml_test_unload_consumer PRIVATE -fno-gnu-unique)
  # This disposable consumer must not supply inline C++ helpers to the pinned
  # product DSO, or a reverse relocation dependency would keep it mapped.
  target_link_options(mpvqml_test_unload_consumer PRIVATE
      "LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/tests/unload-consumer.map")
  set_property(TARGET mpvqml_test_unload_consumer APPEND PROPERTY LINK_DEPENDS
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/unload-consumer.map")
  target_include_directories(mpvqml_test_unload_consumer PRIVATE . ${MPV_INCLUDE_DIRS})
  target_link_libraries(mpvqml_test_unload_consumer PRIVATE mpvqml Qt6::Core ${MPV_LIBRARIES})
  add_executable(mpvqml_unload_tests tests/test_unload.cpp)
  set_target_properties(mpvqml_unload_tests PROPERTIES AUTOMOC OFF)
  target_link_libraries(mpvqml_unload_tests PRIVATE Qt6::Core Qt6::Test ${CMAKE_DL_LIBS})
  target_compile_definitions(mpvqml_unload_tests PRIVATE MPVQML_UNLOAD_CONSUMER="$<TARGET_FILE:mpvqml_test_unload_consumer>")
  target_compile_definitions(mpvqml_crash_tests PRIVATE MPVQML_UNLOAD_EXECUTABLE="$<TARGET_FILE:mpvqml_unload_tests>")
  add_dependencies(mpvqml_unload_tests mpvqml_test_unload_consumer)
  add_dependencies(mpvqml_crash_tests mpvqml_test_faults mpvqml_unload_tests)
  add_test(NAME mpvqml_crash_tests COMMAND mpvqml_crash_tests)

  # libmpv scaler-LUT upload guard: GL unit checks plus an explicitly preloaded
  # child whose LUT padding is deterministically NaN (see tests/lutpoison.c).
  add_library(mpvqml_test_lutpoison SHARED tests/lutpoison.c)
  set_target_properties(mpvqml_test_lutpoison PROPERTIES AUTOMOC OFF C_STANDARD 11 C_STANDARD_REQUIRED ON)
  add_executable(mpvqml_lut_guard_tests tests/test_lut_guard.cpp)
  target_include_directories(mpvqml_lut_guard_tests PRIVATE . ${MPV_INCLUDE_DIRS})
  target_link_libraries(mpvqml_lut_guard_tests PRIVATE mpvqml Qt6::Core Qt6::Gui Qt6::Quick Qt6::OpenGL
      Qt6::Test ${MPV_LIBRARIES} ${CMAKE_DL_LIBS})
  target_compile_definitions(mpvqml_lut_guard_tests PRIVATE
      MPVQML_TEST_VIDEO="${CMAKE_CURRENT_SOURCE_DIR}/tests/assets/test_one_video.mp4"
      MPVQML_LUT_POISON_LIBRARY="$<TARGET_FILE:mpvqml_test_lutpoison>")
  add_dependencies(mpvqml_lut_guard_tests mpvqml_test_lutpoison)
  add_test(NAME mpvqml_lut_guard_tests COMMAND mpvqml_lut_guard_tests)
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
