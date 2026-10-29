cmake_minimum_required(VERSION 3.20)
project(simt_lab VERSION 0.1 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)

set(GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
add_custom_command(OUTPUT ${GENERATED_DIR}/isa.hpp ${GENERATED_DIR}/simt_isa.py
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_isa.py
        --schema ${CMAKE_CURRENT_SOURCE_DIR}/isa/isa.json --out ${GENERATED_DIR}
    DEPENDS tools/gen_isa.py tools/templates/isa.hpp.in tools/templates/isa.py.in isa/isa.json VERBATIM)
add_custom_target(generate_isa DEPENDS ${GENERATED_DIR}/isa.hpp ${GENERATED_DIR}/simt_isa.py)

add_library(simt_storage src/memory.cpp src/atomic_numeric.cpp)
target_include_directories(simt_storage PUBLIC include)
target_compile_options(simt_storage PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_library(simt_core src/machine.cpp src/execution.cpp src/control.cpp src/collective.cpp
    src/dump.cpp src/module.cpp src/memory_execution.cpp src/barrier.cpp
    src/diagnostics.cpp src/tensor.cpp src/tensor_numeric.cpp src/matrix.cpp src/matrix_descriptor.cpp
    src/async.cpp src/async_copy.cpp src/timing/footprint.cpp src/timing/scheduler.cpp src/timing/issue.cpp
    src/timing/execute.cpp src/timing/memory.cpp src/timing/retire.cpp src/timing/report.cpp
    src/hardware/tensor_array.cpp src/timing/tensor_transfer.cpp src/timing/tensor_control.cpp
    src/timing/tensor_tile.cpp src/timing/tensor_operands.cpp src/timing/async_launch.cpp
    src/timing/async_barrier.cpp src/timing/async_dma.cpp src/timing/async_report.cpp src/timing/atomic.cpp ${GENERATED_DIR}/isa.hpp)
add_dependencies(simt_core generate_isa)
target_include_directories(simt_core PUBLIC include ${GENERATED_DIR})
target_compile_options(simt_core PRIVATE -Wall -Wextra -Wpedantic -Werror -ffp-contract=off)
add_executable(simt-run src/main.cpp)
target_link_libraries(simt-run PRIVATE simt_core)
target_compile_options(simt-run PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_library(simt_host src/device.cpp src/device_memory.cpp src/device_engines.cpp src/device_graph.cpp
    src/device_report.cpp src/protocol.cpp src/protocol_codec.cpp src/protocol_commands.cpp)
add_library(simt_monitor src/monitor.cpp)
target_include_directories(simt_monitor PUBLIC include)
set_target_properties(simt_monitor PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_options(simt_monitor PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(simt_host PUBLIC simt_core simt_monitor)
target_compile_options(simt_host PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_executable(simt-device src/device_main.cpp)
target_link_libraries(simt-device PRIVATE simt_host)
target_compile_options(simt-device PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_library(simt_hardware src/hardware/port.cpp src/hardware/dram.cpp src/hardware/cache_tags.cpp
    src/hardware/cache_request.cpp src/hardware/cache_fill.cpp src/hardware/coalescer.cpp src/hardware/shared.cpp
    src/hardware/hierarchy.cpp src/hardware/scoreboard.cpp src/hardware/resources.cpp src/hardware/local.cpp
    src/hardware/atomic_queue.cpp src/hardware/atomic_execute.cpp src/hardware/shared_atomic.cpp)
target_link_libraries(simt_hardware PUBLIC simt_storage)
target_link_libraries(simt_core PUBLIC simt_hardware)
target_compile_options(simt_hardware PRIVATE -Wall -Wextra -Wpedantic -Werror)
find_path(SIMT_NVML_INCLUDE nvml.h HINTS /usr/local/cuda/include)
find_library(SIMT_NATIVE_NVML NAMES libnvidia-ml.so.1 HINTS /usr/lib/x86_64-linux-gnu)
if(SIMT_NVML_INCLUDE AND SIMT_NATIVE_NVML)
    get_filename_component(SIMT_REAL_NVML ${SIMT_NATIVE_NVML} REALPATH)
    add_custom_command(OUTPUT ${GENERATED_DIR}/nvml_stubs.cpp
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_nvml_stubs.py
            --header ${SIMT_NVML_INCLUDE}/nvml.h --source ${CMAKE_CURRENT_SOURCE_DIR}/src/nvml
            --out ${GENERATED_DIR}/nvml_stubs.cpp
        DEPENDS tools/gen_nvml_stubs.py src/nvml/api.cpp src/nvml/events.cpp VERBATIM)
    add_library(simt_nvml SHARED src/nvml/api.cpp src/nvml/registry.cpp src/nvml/events.cpp ${GENERATED_DIR}/nvml_stubs.cpp)
    target_include_directories(simt_nvml PRIVATE ${SIMT_NVML_INCLUDE})
    target_link_libraries(simt_nvml PRIVATE simt_monitor ${CMAKE_DL_LIBS})
    target_compile_definitions(simt_nvml PRIVATE SIMT_REAL_NVML_PATH="${SIMT_REAL_NVML}")
    target_compile_options(simt_nvml PRIVATE -Wall -Wextra -Wpedantic -Werror -Wno-unused-parameter)
    set_target_properties(simt_nvml PROPERTIES OUTPUT_NAME nvidia-ml SOVERSION 1
        LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/nvml)
endif()
option(SIMT_SANITIZE "Enable address and undefined behavior sanitizers" OFF)
if(SIMT_SANITIZE)
    target_compile_options(simt_storage PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(simt_storage PUBLIC -fsanitize=address,undefined)
    target_compile_options(simt_core PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(simt_core PUBLIC -fsanitize=address,undefined)
    target_compile_options(simt_monitor PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    if(TARGET simt_nvml)
        target_compile_options(simt_nvml PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(simt_nvml PRIVATE -fsanitize=address,undefined)
        execute_process(COMMAND ${CMAKE_CXX_COMPILER} -print-file-name=libasan.so
            OUTPUT_VARIABLE SIMT_NVML_ASAN_PRELOAD OUTPUT_STRIP_TRAILING_WHITESPACE)
    endif()
endif()

enable_testing()
add_executable(test-hardware-memory tests/test_hardware_memory.cpp)
target_link_libraries(test-hardware-memory PRIVATE simt_hardware)
target_compile_options(test-hardware-memory PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME hardware_memory_units COMMAND test-hardware-memory)
set_tests_properties(hardware_memory_units PROPERTIES TIMEOUT 90)
add_executable(test-hardware-pipeline tests/test_hardware_pipeline.cpp)
target_link_libraries(test-hardware-pipeline PRIVATE simt_hardware)
target_compile_options(test-hardware-pipeline PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME hardware_pipeline_units COMMAND test-hardware-pipeline)
set_tests_properties(hardware_pipeline_units PROPERTIES TIMEOUT 90)
add_executable(test-hardware-tensor tests/test_hardware_tensor.cpp)
target_link_libraries(test-hardware-tensor PRIVATE simt_core)
target_compile_options(test-hardware-tensor PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME hardware_tensor_array COMMAND test-hardware-tensor)
set_tests_properties(hardware_tensor_array PROPERTIES TIMEOUT 90)
add_executable(test-hardware-atomic tests/test_hardware_atomic.cpp)
target_link_libraries(test-hardware-atomic PRIVATE simt_hardware)
target_compile_options(test-hardware-atomic PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME hardware_atomic_local COMMAND test-hardware-atomic)
set_tests_properties(hardware_atomic_local PROPERTIES TIMEOUT 90)
function(add_simt_test name script)
    add_test(NAME ${name} COMMAND ${CMAKE_COMMAND} -E env
        "PYTHONPATH=${GENERATED_DIR}:${CMAKE_CURRENT_SOURCE_DIR}/python"
        "SIMT_RUNNER=$<TARGET_FILE:simt-run>"
        ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/${script})
    set_tests_properties(${name} PROPERTIES TIMEOUT 90)
endfunction()
add_simt_test(isa_and_execution test_simulator.py)
add_simt_test(timed_sm_execution test_timing.py)
add_simt_test(timed_tensor_execution test_timed_tensor.py)
add_simt_test(timed_async_execution test_timed_async.py)
add_simt_test(timed_memory_execution test_timed_memory.py)
add_simt_test(control_and_collectives test_control.py)
add_simt_test(memory_and_barriers test_memory.py)
add_simt_test(tensor_numeric test_tensor.py)
add_simt_test(async_transactions test_async.py)
add_simt_test(compositional_workloads test_workloads.py)
add_simt_test(native_host_protocol test_protocol.py)
add_simt_test(host_command_processor test_host.py)
add_simt_test(graph_submission_and_capture test_graph.py)
if(TARGET simt_nvml)
    add_executable(test-nvml-api tests/test_nvml_api.cpp)
    target_include_directories(test-nvml-api PRIVATE ${SIMT_NVML_INCLUDE})
    target_link_libraries(test-nvml-api PRIVATE simt_nvml)
    target_compile_options(test-nvml-api PRIVATE -Wall -Wextra -Wpedantic -Werror)
    if(SIMT_SANITIZE)
        target_compile_options(test-nvml-api PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(test-nvml-api PRIVATE -fsanitize=address,undefined)
    endif()
    add_simt_test(native_nvidia_smi test_nvml.py)
    set_tests_properties(native_nvidia_smi PROPERTIES ENVIRONMENT "SIMT_NVML_ASAN_PRELOAD=${SIMT_NVML_ASAN_PRELOAD}")
endif()
