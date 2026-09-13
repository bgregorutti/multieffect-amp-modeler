cmake_minimum_required(VERSION 3.16)
project(audio_engine CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

add_compile_options(-Wall -Wextra)

# --- Dependencies ------------------------------------------------------
# Deliberately NOT fetched via FetchContent/git/curl: this sandbox's
# network proxy blocks codeload.github.com (git clone / GitHub archive
# downloads), so every dependency here comes from apt (Ubuntu package
# repos, reachable) instead. See README.md "Deviations" for the full
# rationale (this is also why there is no JUCE and no
# NeuralAmpModelerCore in this tree).
find_package(nlohmann_json 3.10 REQUIRED)
find_package(Threads REQUIRED)

# --- Core library (everything except main.cpp) -------------------------
add_library(audio_engine_core
  src/preset_model.cpp
  src/gain_block.cpp
  src/eq_block.cpp
  src/delay_block.cpp
  src/wav_file.cpp
  src/convolution.cpp
  src/nam_model.cpp
  src/preset_switcher.cpp
  src/resource_manager.cpp
  src/engine_state.cpp
  src/control_socket.cpp
)
target_include_directories(audio_engine_core PUBLIC include)
target_link_libraries(audio_engine_core PUBLIC nlohmann_json::nlohmann_json Threads::Threads)

# --- Executable ----------------------------------------------------------
add_executable(audio_engine src/main.cpp)
target_link_libraries(audio_engine PRIVATE audio_engine_core)

# --- Tests ---------------------------------------------------------------
enable_testing()
option(AUDIO_ENGINE_BUILD_TESTS "Build audio-engine's gtest test suite" ON)
if(AUDIO_ENGINE_BUILD_TESTS)
  find_package(GTest REQUIRED)

  add_executable(audio_engine_tests
    tests/test_preset_model.cpp
    tests/test_gain_block.cpp
    tests/test_eq_block.cpp
    tests/test_delay_block.cpp
    tests/test_wav_file.cpp
    tests/test_convolution.cpp
    tests/test_nam_model.cpp
    tests/test_preset_switcher.cpp
    tests/test_resource_manager.cpp
    tests/test_control_socket_integration.cpp
  )
  target_link_libraries(audio_engine_tests PRIVATE audio_engine_core GTest::gtest GTest::gtest_main)
  target_compile_definitions(audio_engine_tests PRIVATE
    AUDIO_ENGINE_EXECUTABLE_PATH="$<TARGET_FILE:audio_engine>")
  add_dependencies(audio_engine_tests audio_engine)

  include(GoogleTest)
  gtest_discover_tests(audio_engine_tests)
endif()
