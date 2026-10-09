cmake_minimum_required(VERSION 3.21)
project(NeuralPass VERSION 0.1.0 LANGUAGES CXX)

option(NEURALPASS_BUILD_ADDON "Build the Windows ReShade add-on" ${WIN32})
option(NEURALPASS_BUILD_TESTS "Build platform-neutral tests" ON)
set(RESHADE_SDK_DIR "" CACHE PATH "Path to a ReShade checkout (must contain include/reshade.hpp)")
set(ONNXRUNTIME_ROOT "" CACHE PATH "Optional ONNX Runtime DirectML package root")

add_library(neuralpass_core STATIC
    src/atlas_cache.cpp
    src/binding_identity.cpp
    src/inference_preview.cpp
    src/scene_cache.cpp
    src/surface_capture.cpp
    src/temporal.cpp
    src/texture_baker.cpp
    src/tile_scheduler.cpp)
target_include_directories(neuralpass_core PUBLIC include)
target_compile_features(neuralpass_core PUBLIC cxx_std_20)
if(MSVC)
    target_compile_options(neuralpass_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(neuralpass_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(NEURALPASS_BUILD_TESTS)
    enable_testing()
    add_executable(neuralpass_tests tests/core_tests.cpp)
    target_link_libraries(neuralpass_tests PRIVATE neuralpass_core)
    add_test(NAME neuralpass_core COMMAND neuralpass_tests)
    if(WIN32)
        add_executable(neuralpass_d3d11_capture_tests
            addon/d3d11_surface_capture.cpp
            tests/d3d11_capture_tests.cpp)
        target_include_directories(neuralpass_d3d11_capture_tests PRIVATE include addon)
        target_compile_features(neuralpass_d3d11_capture_tests PRIVATE cxx_std_20)
        target_link_libraries(neuralpass_d3d11_capture_tests PRIVATE
            neuralpass_core d3d11 d3dcompiler)
        add_test(NAME neuralpass_d3d11_capture COMMAND neuralpass_d3d11_capture_tests)
    endif()
endif()

if(NEURALPASS_BUILD_ADDON)
    if(NOT WIN32)
        message(FATAL_ERROR "The ReShade add-on is Windows-only")
    endif()
    if(NOT EXISTS "${RESHADE_SDK_DIR}/include/reshade.hpp")
        message(FATAL_ERROR "Set RESHADE_SDK_DIR to a ReShade source checkout")
    endif()
    add_library(NeuralPassAddon SHARED
        addon/d3d11_surface_capture.cpp
        addon/neuralpass_addon.cpp
        "${RESHADE_SDK_DIR}/examples/utils/descriptor_tracking.cpp")
    target_compile_features(NeuralPassAddon PRIVATE cxx_std_20)
    target_include_directories(NeuralPassAddon PRIVATE include
        "${RESHADE_SDK_DIR}/include" "${RESHADE_SDK_DIR}/deps/imgui"
        "${RESHADE_SDK_DIR}/examples/utils")
    target_link_libraries(NeuralPassAddon PRIVATE neuralpass_core d3d11 d3dcompiler)
    target_compile_definitions(NeuralPassAddon PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(NeuralPassAddon PRIVATE /W4 /permissive-)
    if(EXISTS "${ONNXRUNTIME_ROOT}/include/onnxruntime_cxx_api.h")
        target_sources(NeuralPassAddon PRIVATE src/inference_onnx.cpp)
        target_include_directories(NeuralPassAddon PRIVATE "${ONNXRUNTIME_ROOT}/include")
        target_link_directories(NeuralPassAddon PRIVATE "${ONNXRUNTIME_ROOT}/lib")
        target_link_libraries(NeuralPassAddon PRIVATE onnxruntime)
        target_compile_definitions(NeuralPassAddon PRIVATE
            NEURALPASS_HAS_ONNXRUNTIME NEURALPASS_USE_DIRECTML)
    else()
        message(WARNING "ONNXRUNTIME_ROOT not set: add-on will use the clearly-labelled preview backend")
    endif()
    set_target_properties(NeuralPassAddon PROPERTIES
        PREFIX ""
        OUTPUT_NAME "NeuralPass"
        SUFFIX ".addon64")
endif()
