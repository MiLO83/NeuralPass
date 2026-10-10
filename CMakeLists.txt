cmake_minimum_required(VERSION 3.21)
project(NeuralPass VERSION 0.1.0 LANGUAGES CXX)

if(MSVC)
    add_link_options(/Brepro "/PDBALTPATH:%_PDB%")
endif()

option(NEURALPASS_BUILD_ADDON "Build the Windows ReShade add-on" ${WIN32})
option(NEURALPASS_BUILD_TESTS "Build platform-neutral tests" ON)
option(NEURALPASS_TEST_DIRECTML "Execute DirectML inference tests (requires a GPU adapter)" ON)
set(RESHADE_SDK_DIR "" CACHE PATH "Path to a ReShade checkout (must contain include/reshade.hpp)")
set(ONNXRUNTIME_ROOT "" CACHE PATH "Optional ONNX Runtime DirectML package root")
set(NEURALPASS_EXTERNAL_WORKER "" CACHE FILEPATH "Optional x64 NeuralPassWorker.exe for Win32 tests/package")
set(VULKAN_HEADERS_DIR "" CACHE PATH "Optional Vulkan-Headers checkout for native runtime tests")

add_library(neuralpass_core STATIC
    src/adaptive_budget.cpp
    src/atlas_cache.cpp
    src/binding_identity.cpp
    src/binding_style.cpp
    src/cache_control.cpp
    src/cache_migration.cpp
    src/cache_namespace.cpp
    src/color_pipeline.cpp
    src/depth_pyramid.cpp
    src/inference_preview.cpp
    src/runtime_evidence.cpp
    src/scene_cache.cpp
    src/surface_capture.cpp
    src/temporal.cpp
    src/texture_baker.cpp
    src/tile_scheduler.cpp)
target_sources(neuralpass_core PRIVATE src/visibility.cpp)
target_include_directories(neuralpass_core PUBLIC include)
target_compile_features(neuralpass_core PUBLIC cxx_std_20)
if(MSVC)
    target_compile_options(neuralpass_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(neuralpass_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(NEURALPASS_BUILD_TESTS)
    enable_testing()
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME neuralpass_package_metadata
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/tests/package_metadata_tests.py")
    add_executable(neuralpass_tests tests/core_tests.cpp)
    target_link_libraries(neuralpass_tests PRIVATE neuralpass_core)
    add_test(NAME neuralpass_core COMMAND neuralpass_tests)
    add_executable(neuralpass_vulkan_spirv_tests tests/vulkan_spirv_tests.cpp)
    target_include_directories(neuralpass_vulkan_spirv_tests PRIVATE addon)
    target_compile_features(neuralpass_vulkan_spirv_tests PRIVATE cxx_std_20)
    add_test(NAME neuralpass_vulkan_spirv COMMAND neuralpass_vulkan_spirv_tests)
    add_executable(neuralpass_worker_protocol_tests tests/worker_protocol_tests.cpp)
    target_include_directories(neuralpass_worker_protocol_tests PRIVATE include)
    target_compile_features(neuralpass_worker_protocol_tests PRIVATE cxx_std_20)
    add_test(NAME neuralpass_worker_protocol COMMAND neuralpass_worker_protocol_tests)
    if(WIN32)
        if(VULKAN_HEADERS_DIR AND EXISTS "${VULKAN_HEADERS_DIR}/include/vulkan/vulkan.h")
            add_executable(neuralpass_vulkan_runtime_tests tests/vulkan_runtime_tests.cpp)
            target_include_directories(neuralpass_vulkan_runtime_tests PRIVATE
                addon tests "${VULKAN_HEADERS_DIR}/include")
            target_compile_features(neuralpass_vulkan_runtime_tests PRIVATE cxx_std_20)
            target_compile_definitions(neuralpass_vulkan_runtime_tests PRIVATE
                WIN32_LEAN_AND_MEAN NOMINMAX VK_NO_PROTOTYPES)
            add_test(NAME neuralpass_vulkan_runtime COMMAND neuralpass_vulkan_runtime_tests)
            set_tests_properties(neuralpass_vulkan_runtime PROPERTIES SKIP_RETURN_CODE 77)
        endif()
        add_executable(neuralpass_d3d9_capture_tests
            addon/d3d9_surface_capture.cpp
            tests/d3d9_capture_tests.cpp)
        target_include_directories(neuralpass_d3d9_capture_tests PRIVATE include addon)
        target_compile_features(neuralpass_d3d9_capture_tests PRIVATE cxx_std_20)
        target_compile_definitions(neuralpass_d3d9_capture_tests PRIVATE
            WIN32_LEAN_AND_MEAN NOMINMAX)
        target_link_libraries(neuralpass_d3d9_capture_tests PRIVATE
            neuralpass_core d3d9 d3dcompiler user32)
        add_test(NAME neuralpass_d3d9_capture COMMAND neuralpass_d3d9_capture_tests)
        add_executable(neuralpass_d3d10_capture_tests
            addon/d3d10_surface_capture.cpp
            tests/d3d10_capture_tests.cpp)
        target_include_directories(neuralpass_d3d10_capture_tests PRIVATE include addon)
        target_compile_features(neuralpass_d3d10_capture_tests PRIVATE cxx_std_20)
        target_compile_definitions(neuralpass_d3d10_capture_tests PRIVATE
            WIN32_LEAN_AND_MEAN NOMINMAX)
        target_link_libraries(neuralpass_d3d10_capture_tests PRIVATE
            neuralpass_core d3d10 dxgi d3dcompiler)
        add_test(NAME neuralpass_d3d10_capture COMMAND neuralpass_d3d10_capture_tests)
        add_executable(neuralpass_d3d11_capture_tests
            addon/d3d11_surface_capture.cpp
            tests/d3d11_capture_tests.cpp)
        target_include_directories(neuralpass_d3d11_capture_tests PRIVATE include addon)
        target_compile_features(neuralpass_d3d11_capture_tests PRIVATE cxx_std_20)
        target_link_libraries(neuralpass_d3d11_capture_tests PRIVATE
            neuralpass_core d3d11 d3dcompiler)
        add_test(NAME neuralpass_d3d11_capture COMMAND neuralpass_d3d11_capture_tests)
        if(RESHADE_SDK_DIR AND EXISTS "${RESHADE_SDK_DIR}/include/reshade.hpp")
            add_executable(neuralpass_d3d12_capture_tests
                addon/d3d12_surface_capture.cpp
                tests/d3d12_capture_tests.cpp)
            target_include_directories(neuralpass_d3d12_capture_tests PRIVATE include addon
                "${RESHADE_SDK_DIR}/include" "${RESHADE_SDK_DIR}/deps/imgui")
            target_compile_features(neuralpass_d3d12_capture_tests PRIVATE cxx_std_20)
            target_compile_definitions(neuralpass_d3d12_capture_tests PRIVATE
                WIN32_LEAN_AND_MEAN NOMINMAX)
            target_link_libraries(neuralpass_d3d12_capture_tests PRIVATE
                neuralpass_core d3d12 dxgi d3dcompiler)
            add_test(NAME neuralpass_d3d12_capture COMMAND neuralpass_d3d12_capture_tests)
        endif()
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
        addon/d3d9_surface_capture.cpp
        addon/d3d10_surface_capture.cpp
        addon/d3d11_surface_capture.cpp
        addon/d3d12_surface_capture.cpp
        addon/vulkan_surface_capture.cpp
        addon/neuralpass_addon.cpp
        src/inference_worker_client.cpp
        "${RESHADE_SDK_DIR}/examples/utils/descriptor_tracking.cpp")
    target_compile_features(NeuralPassAddon PRIVATE cxx_std_20)
    target_include_directories(NeuralPassAddon PRIVATE include
        "${RESHADE_SDK_DIR}/include" "${RESHADE_SDK_DIR}/deps/imgui"
        "${RESHADE_SDK_DIR}/examples/utils")
    target_link_libraries(NeuralPassAddon PRIVATE neuralpass_core d3d9 d3d10 d3d11 d3d12 d3dcompiler)
    target_compile_definitions(NeuralPassAddon PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(NeuralPassAddon PRIVATE /W4 /permissive-)
    if(EXISTS "${ONNXRUNTIME_ROOT}/include/onnxruntime_cxx_api.h")
        target_sources(NeuralPassAddon PRIVATE src/inference_onnx.cpp)
        target_include_directories(NeuralPassAddon PRIVATE "${ONNXRUNTIME_ROOT}/include")
        target_link_directories(NeuralPassAddon PRIVATE "${ONNXRUNTIME_ROOT}/lib")
        target_link_libraries(NeuralPassAddon PRIVATE onnxruntime)
        target_compile_definitions(NeuralPassAddon PRIVATE
            NEURALPASS_HAS_ONNXRUNTIME NEURALPASS_USE_DIRECTML)
        if(NEURALPASS_BUILD_TESTS)
            add_executable(neuralpass_onnx_smoke_tests
                src/inference_onnx.cpp
                tests/onnx_smoke_tests.cpp)
            target_include_directories(neuralpass_onnx_smoke_tests PRIVATE
                include "${ONNXRUNTIME_ROOT}/include")
            target_compile_features(neuralpass_onnx_smoke_tests PRIVATE cxx_std_20)
            target_compile_definitions(neuralpass_onnx_smoke_tests PRIVATE
                NEURALPASS_HAS_ONNXRUNTIME NEURALPASS_USE_DIRECTML
                WIN32_LEAN_AND_MEAN NOMINMAX)
            target_link_directories(neuralpass_onnx_smoke_tests PRIVATE
                "${ONNXRUNTIME_ROOT}/lib")
            target_link_libraries(neuralpass_onnx_smoke_tests PRIVATE
                neuralpass_core onnxruntime dxgi)
            add_custom_command(TARGET neuralpass_onnx_smoke_tests POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${ONNXRUNTIME_ROOT}/lib/onnxruntime.dll"
                    "${ONNXRUNTIME_ROOT}/lib/onnxruntime_providers_shared.dll"
                    "${ONNXRUNTIME_ROOT}/lib/DirectML.dll"
                    "$<TARGET_FILE_DIR:neuralpass_onnx_smoke_tests>")
            add_test(NAME neuralpass_onnx_cpu
                COMMAND neuralpass_onnx_smoke_tests
                    "${CMAKE_SOURCE_DIR}/models/downloads/candy-9.onnx" cpu)
            if(NEURALPASS_TEST_DIRECTML)
                add_test(NAME neuralpass_onnx_directml
                    COMMAND neuralpass_onnx_smoke_tests
                        "${CMAKE_SOURCE_DIR}/models/downloads/candy-9.onnx" directml)
            endif()
        endif()
        if(CMAKE_SIZEOF_VOID_P EQUAL 8)
            add_executable(NeuralPassWorker
                src/inference_onnx.cpp
                src/worker_main.cpp)
            target_include_directories(NeuralPassWorker PRIVATE
                include "${ONNXRUNTIME_ROOT}/include")
            target_compile_features(NeuralPassWorker PRIVATE cxx_std_20)
            target_compile_definitions(NeuralPassWorker PRIVATE
                NEURALPASS_HAS_ONNXRUNTIME NEURALPASS_USE_DIRECTML
                WIN32_LEAN_AND_MEAN NOMINMAX)
            target_link_directories(NeuralPassWorker PRIVATE "${ONNXRUNTIME_ROOT}/lib")
            target_link_libraries(NeuralPassWorker PRIVATE neuralpass_core onnxruntime)
            add_custom_command(TARGET NeuralPassWorker POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${ONNXRUNTIME_ROOT}/lib/onnxruntime.dll"
                    "${ONNXRUNTIME_ROOT}/lib/onnxruntime_providers_shared.dll"
                    "${ONNXRUNTIME_ROOT}/lib/DirectML.dll"
                    "$<TARGET_FILE_DIR:NeuralPassWorker>")
            if(NEURALPASS_BUILD_TESTS)
                add_executable(neuralpass_worker_smoke_tests
                    src/inference_worker_client.cpp
                    tests/worker_smoke_tests.cpp)
                target_include_directories(neuralpass_worker_smoke_tests PRIVATE include)
                target_compile_features(neuralpass_worker_smoke_tests PRIVATE cxx_std_20)
                target_compile_definitions(neuralpass_worker_smoke_tests PRIVATE
                    WIN32_LEAN_AND_MEAN NOMINMAX)
                target_link_libraries(neuralpass_worker_smoke_tests PRIVATE neuralpass_core)
                add_dependencies(neuralpass_worker_smoke_tests NeuralPassWorker)
                add_test(NAME neuralpass_worker_cpu
                    COMMAND neuralpass_worker_smoke_tests "$<TARGET_FILE:NeuralPassWorker>"
                        "${CMAKE_SOURCE_DIR}/models/downloads/candy-9.onnx" cpu)
                if(NEURALPASS_TEST_DIRECTML)
                    add_test(NAME neuralpass_worker_directml
                        COMMAND neuralpass_worker_smoke_tests "$<TARGET_FILE:NeuralPassWorker>"
                            "${CMAKE_SOURCE_DIR}/models/downloads/candy-9.onnx" directml)
                endif()
            endif()
        endif()
    else()
        if(CMAKE_SIZEOF_VOID_P EQUAL 4 AND NEURALPASS_EXTERNAL_WORKER AND
           EXISTS "${NEURALPASS_EXTERNAL_WORKER}")
            message(STATUS "Win32 add-on will use the x64 NeuralPass inference worker")
        else()
            message(WARNING "ONNXRUNTIME_ROOT not set: add-on will use the clearly-labelled preview backend")
        endif()
    endif()
    if(NEURALPASS_BUILD_TESTS AND CMAKE_SIZEOF_VOID_P EQUAL 4 AND
       NEURALPASS_EXTERNAL_WORKER AND EXISTS "${NEURALPASS_EXTERNAL_WORKER}")
        add_executable(neuralpass_worker_smoke_tests
            src/inference_worker_client.cpp
            tests/worker_smoke_tests.cpp)
        target_include_directories(neuralpass_worker_smoke_tests PRIVATE include)
        target_compile_features(neuralpass_worker_smoke_tests PRIVATE cxx_std_20)
        target_compile_definitions(neuralpass_worker_smoke_tests PRIVATE
            WIN32_LEAN_AND_MEAN NOMINMAX)
        target_link_libraries(neuralpass_worker_smoke_tests PRIVATE neuralpass_core)
        add_test(NAME neuralpass_worker_x86_to_x64
            COMMAND neuralpass_worker_smoke_tests "${NEURALPASS_EXTERNAL_WORKER}"
                "${CMAKE_SOURCE_DIR}/models/downloads/candy-9.onnx" cpu)
    endif()
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(NEURALPASS_ADDON_SUFFIX ".addon64")
    else()
        set(NEURALPASS_ADDON_SUFFIX ".addon32")
    endif()
    set_target_properties(NeuralPassAddon PROPERTIES
        PREFIX ""
        OUTPUT_NAME "NeuralPass"
        SUFFIX "${NEURALPASS_ADDON_SUFFIX}")
endif()
