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
  src/tone_stack_block.cpp
  src/delay_block.cpp
  src/wav_file.cpp
  src/resample.cpp
  src/convolution.cpp
  src/nam_model.cpp
  src/preset_switcher.cpp
  src/resource_manager.cpp
  src/engine_state.cpp
  src/control_socket.cpp
)
target_include_directories(audio_engine_core PUBLIC include)
target_link_libraries(audio_engine_core PUBLIC nlohmann_json::nlohmann_json Threads::Threads)

# --- Real-time audio I/O (optional, dev-machine testing) -----------------
# Off by default: a production/Pi build need not link PortAudio at all if
# it ends up using a different backend once real hardware exists. This is
# NOT the JUCE dependency described in README.md "Deviations" -- PortAudio
# is a small, separately-packaged C library (e.g. `brew install portaudio`
# on macOS, `apt install portaudio19-dev` on Linux), reachable through a
# normal package manager rather than a GitHub clone/archive, so it isn't
# subject to the same network-sandbox constraint.
option(AUDIO_ENGINE_WITH_PORTAUDIO
  "Build the PortAudio-based real-time audio I/O backend (audio_engine --audio)" OFF)
if(AUDIO_ENGINE_WITH_PORTAUDIO)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(PORTAUDIO REQUIRED IMPORTED_TARGET portaudio-2.0)

  target_sources(audio_engine_core PRIVATE src/portaudio_backend.cpp)
  target_link_libraries(audio_engine_core PUBLIC PkgConfig::PORTAUDIO)
  target_compile_definitions(audio_engine_core PUBLIC AUDIO_ENGINE_WITH_PORTAUDIO)
endif()

# --- Real NAM inference (optional, requires network access to vendor) ---
# Off by default: this is the dependency README.md's "Deviations" section
# always said would be a drop-in once GitHub access was available (this
# machine, unlike the original sandbox this project was mostly built in,
# has it) -- see README.md "Real NAM inference". Vendors
# NeuralAmpModelerCore (MIT-licensed, github.com/sdatkinson/
# NeuralAmpModelerCore) and Eigen (MPL2-licensed, needed for its matrix
# math) via CMake FetchContent, pinned to specific commits/tags for
# reproducibility. A production/Pi build (or this sandbox's original CI)
# need not fetch either if it's fine with StubNamModel's identity/gain
# pass-through -- same "narrow interface + swappable backend" pattern as
# every other hardware/external dependency in this project.
option(AUDIO_ENGINE_WITH_REAL_NAM
  "Vendor NeuralAmpModelerCore for real WaveNet/LSTM NAM inference (requires network access)" OFF)
if(AUDIO_ENGINE_WITH_REAL_NAM)
  include(FetchContent)

  # SOURCE_SUBDIR points at a directory that doesn't exist in either repo,
  # so FetchContent_MakeAvailable populates *_SOURCE_DIR (a plain git
  # clone) without running that repo's own CMakeLists.txt -- we only want
  # NeuralAmpModelerCore's NAM/ sources and Eigen's headers, not their own
  # build systems (NeuralAmpModelerCore's top-level CMakeLists.txt builds
  # a tools/ subdirectory that needs a second submodule, AudioDSPTools,
  # we have no use for here).
  FetchContent_Declare(
    neural_amp_modeler_core
    GIT_REPOSITORY https://github.com/sdatkinson/NeuralAmpModelerCore.git
    GIT_TAG 2563c0fd4cb1f9ce457d89a761738ea15097e1f3
    SOURCE_SUBDIR not-used
  )
  FetchContent_Declare(
    eigen_for_nam
    GIT_REPOSITORY https://gitlab.com/libeigen/eigen.git
    GIT_TAG 3.4.0
    SOURCE_SUBDIR not-used
  )
  FetchContent_MakeAvailable(neural_amp_modeler_core eigen_for_nam)

  file(GLOB NAM_CORE_SOURCES
    "${neural_amp_modeler_core_SOURCE_DIR}/NAM/*.cpp"
    "${neural_amp_modeler_core_SOURCE_DIR}/NAM/*/*.cpp"
  )
  if(NOT NAM_CORE_SOURCES)
    message(FATAL_ERROR "No NAM/*.cpp sources found under ${neural_amp_modeler_core_SOURCE_DIR} -- fetch failed?")
  endif()

  # NeuralAmpModelerCore's sources #include "json.hpp" unqualified,
  # expecting their own vendored nlohmann/json copy on the include path.
  # Point that at our own already-required nlohmann_json instead of
  # fetching a second copy.
  set(NAM_JSON_SHIM_DIR "${CMAKE_BINARY_DIR}/nam_json_shim")
  file(WRITE "${NAM_JSON_SHIM_DIR}/json.hpp" "#include <nlohmann/json.hpp>\n")

  add_library(nam_core STATIC ${NAM_CORE_SOURCES})
  target_include_directories(nam_core SYSTEM PUBLIC
    "${neural_amp_modeler_core_SOURCE_DIR}"
    "${eigen_for_nam_SOURCE_DIR}"
    "${NAM_JSON_SHIM_DIR}"
  )
  target_link_libraries(nam_core PUBLIC nlohmann_json::nlohmann_json)
  target_compile_features(nam_core PUBLIC cxx_std_20)
  # NeuralAmpModelerCore's own CMakeLists.txt forces libc++ on macOS; we
  # don't run that file (see SOURCE_SUBDIR above), so match it explicitly
  # -- AppleClang's default standard library already is libc++, but this
  # keeps the flag visible/intentional rather than relying on the default.
  if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    target_compile_options(nam_core PUBLIC -stdlib=libc++)
  endif()

  target_sources(audio_engine_core PRIVATE src/real_nam_model.cpp)
  # WHOLE_ARCHIVE, not a plain link: nam::get_dsp() dispatches to a
  # per-architecture parser (WaveNet, LSTM, container, ...) that each
  # self-registers via a static initializer in its own .cpp file. Nothing
  # in our code calls a symbol from e.g. wavenet/model.cpp directly, so a
  # normal static-library link only pulls in .o members that resolve an
  # outstanding undefined symbol -- the linker silently drops those
  # registration-only translation units, and get_dsp() then throws "No
  # config parser registered for architecture: WaveNet" at runtime for
  # every real file. WHOLE_ARCHIVE forces every object file in nam_core to
  # link regardless, which is exactly what's needed for a registry pattern
  # like this one.
  target_link_libraries(audio_engine_core PUBLIC $<LINK_LIBRARY:WHOLE_ARCHIVE,nam_core>)
  target_compile_definitions(audio_engine_core PUBLIC AUDIO_ENGINE_WITH_REAL_NAM)

  # Dev diagnostic: renders a WAV through a real .nam model (dry + wet WAV
  # output for direct comparison) using the exact same code path production
  # audio does -- see tools/nam_render.cpp.
  add_executable(nam_render tools/nam_render.cpp)
  target_link_libraries(nam_render PRIVATE audio_engine_core)
endif()

# --- VST3 plugin hosting (optional, requires network access to vendor) ---
# Off by default, same rationale/pattern as AUDIO_ENGINE_WITH_REAL_NAM above:
# vendors vst3_pluginterfaces (Steinberg VST3 SDK license -- see its own
# LICENSE.txt; this is the "pluginterfaces" subset only, no GUI/validator
# tooling, no JUCE, no public.sdk) via CMake FetchContent, pinned to a
# release tag. See vst3_host.hpp for what "hosting" means here: a
# hand-written COM-style host talking directly to
# IPluginFactory/IComponent/IAudioProcessor/IEditController. Off builds keep
# the "vst3" block type resolvable (vst3_plugin_host_stub.cpp) so a preset
# referencing one fails with one clear error instead of a link error.
option(AUDIO_ENGINE_WITH_VST3
  "Vendor the VST3 SDK's pluginterfaces for real VST3 plugin hosting (requires network access)" OFF)
if(AUDIO_ENGINE_WITH_VST3)
  include(FetchContent)

  FetchContent_Declare(
    vst3_pluginterfaces
    GIT_REPOSITORY https://github.com/steinbergmedia/vst3_pluginterfaces.git
    GIT_TAG v3.7.14_build_55
    SOURCE_SUBDIR not-used
  )
  FetchContent_MakeAvailable(vst3_pluginterfaces)

  # vst3_pluginterfaces' own headers #include "pluginterfaces/base/...h" /
  # "pluginterfaces/vst/...h" -- i.e. they expect to be found under a
  # directory literally named "pluginterfaces" -- but FetchContent clones
  # the repo's own root (base/, vst/, gui/, ...) straight into
  # vst3_pluginterfaces_SOURCE_DIR with no such wrapper folder. A symlink
  # shim (same idea as NAM_JSON_SHIM_DIR above, just a directory instead of
  # a generated file) gives the compiler that expected path without
  # copying the tree.
  set(VST3_INCLUDE_SHIM_DIR "${CMAKE_BINARY_DIR}/vst3_include_shim")
  file(MAKE_DIRECTORY "${VST3_INCLUDE_SHIM_DIR}")
  if(NOT EXISTS "${VST3_INCLUDE_SHIM_DIR}/pluginterfaces")
    file(CREATE_LINK "${vst3_pluginterfaces_SOURCE_DIR}" "${VST3_INCLUDE_SHIM_DIR}/pluginterfaces" SYMBOLIC)
  endif()

  # pluginterfaces is "actually a header-only library, but it has some
  # sources as well which need compilation" (its own CMakeLists.txt's
  # words) -- we don't run that CMakeLists.txt (it calls
  # smtg_target_setup_universal_binary(), from vst3sdk's top-level cmake/
  # modules we don't vendor), so list those few .cpp files ourselves.
  add_library(vst3_pluginterfaces_core STATIC
    "${vst3_pluginterfaces_SOURCE_DIR}/base/conststringtable.cpp"
    "${vst3_pluginterfaces_SOURCE_DIR}/base/coreiids.cpp"
    "${vst3_pluginterfaces_SOURCE_DIR}/base/funknown.cpp"
    "${vst3_pluginterfaces_SOURCE_DIR}/base/ustring.cpp"
  )
  target_include_directories(vst3_pluginterfaces_core SYSTEM PUBLIC "${VST3_INCLUDE_SHIM_DIR}")
  target_compile_features(vst3_pluginterfaces_core PUBLIC cxx_std_17)

  target_sources(audio_engine_core PRIVATE src/vst3_plugin_host.cpp)
  target_link_libraries(audio_engine_core PUBLIC vst3_pluginterfaces_core ${CMAKE_DL_LIBS})
  target_compile_definitions(audio_engine_core PUBLIC AUDIO_ENGINE_WITH_VST3)

  # In-repo trivial VST3 plugin (gain), built purely from the vendored
  # pluginterfaces -- lets test_vst3_host.cpp exercise Vst3PluginHost
  # end-to-end with zero third-party binary dependency, same ethos as
  # MockFootswitchBackend/NullAudioEngineClient elsewhere in this project.
  # Real third-party plugins (DISTRHO-family etc.) are for later manual/
  # hardware validation, not this test.
  add_library(vst3_test_gain_plugin MODULE tests/vst3_test_plugin/gain_plugin.cpp)
  target_link_libraries(vst3_test_gain_plugin PRIVATE vst3_pluginterfaces_core)
  set_target_properties(vst3_test_gain_plugin PROPERTIES PREFIX "" OUTPUT_NAME "vst3_test_gain_plugin")

  # Packaged into a real `.vst3` bundle directory
  # (Contents/<arch>-linux/*.so), not just a bare .so, so the test loads it
  # exactly the way it would load a real plugin -- see
  # resolveModuleSharedLibrary() in vst3_plugin_host.cpp.
  set(VST3_TEST_PLUGIN_ARCH_DIR "${CMAKE_SYSTEM_PROCESSOR}-linux")
  set(VST3_TEST_PLUGIN_BUNDLE_DIR "${CMAKE_BINARY_DIR}/vst3_test_plugins/GainTest.vst3")
  add_custom_command(TARGET vst3_test_gain_plugin POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
            "${VST3_TEST_PLUGIN_BUNDLE_DIR}/Contents/${VST3_TEST_PLUGIN_ARCH_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy
            "$<TARGET_FILE:vst3_test_gain_plugin>"
            "${VST3_TEST_PLUGIN_BUNDLE_DIR}/Contents/${VST3_TEST_PLUGIN_ARCH_DIR}/vst3_test_gain_plugin.so"
  )
else()
  target_sources(audio_engine_core PRIVATE src/vst3_plugin_host_stub.cpp)
endif()

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
    tests/test_tone_stack_block.cpp
    tests/test_delay_block.cpp
    tests/test_wav_file.cpp
    tests/test_resample.cpp
    tests/test_convolution.cpp
    tests/test_nam_model.cpp
    tests/test_preset_switcher.cpp
    tests/test_resource_manager.cpp
    tests/test_engine_state.cpp
    tests/test_control_socket_integration.cpp
  )
  target_link_libraries(audio_engine_tests PRIVATE audio_engine_core GTest::gtest GTest::gtest_main)
  target_compile_definitions(audio_engine_tests PRIVATE
    AUDIO_ENGINE_EXECUTABLE_PATH="$<TARGET_FILE:audio_engine>")
  add_dependencies(audio_engine_tests audio_engine)

  if(AUDIO_ENGINE_WITH_REAL_NAM)
    target_sources(audio_engine_tests PRIVATE tests/test_real_nam_model.cpp)
    # Reuse NeuralAmpModelerCore's own example .nam fixtures (already
    # fetched above) rather than hand-rolling test files that might not
    # match real export shapes -- see test_real_nam_model.cpp.
    target_compile_definitions(audio_engine_tests PRIVATE
      NAM_EXAMPLE_MODELS_DIR="${neural_amp_modeler_core_SOURCE_DIR}/example_models")
  endif()

  if(AUDIO_ENGINE_WITH_VST3)
    target_sources(audio_engine_tests PRIVATE tests/test_vst3_host.cpp)
    target_compile_definitions(audio_engine_tests PRIVATE
      VST3_TEST_PLUGIN_BUNDLE_DIR="${VST3_TEST_PLUGIN_BUNDLE_DIR}")
    add_dependencies(audio_engine_tests vst3_test_gain_plugin)
  endif()

  include(GoogleTest)
  gtest_discover_tests(audio_engine_tests)
endif()
