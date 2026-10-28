cmake_minimum_required(VERSION 3.20)
project(simt_lab VERSION 0.1 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
add_custom_command(OUTPUT ${GENERATED_DIR}/isa.hpp ${GENERATED_DIR}/simt_isa.py
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_isa.py
        --schema ${CMAKE_CURRENT_SOURCE_DIR}/isa/isa.json --out ${GENERATED_DIR}
    DEPENDS tools/gen_isa.py tools/templates/isa.hpp.in tools/templates/isa.py.in isa/isa.json VERBATIM)
add_custom_target(generate_isa DEPENDS ${GENERATED_DIR}/isa.hpp ${GENERATED_DIR}/simt_isa.py)

add_library(simt_core src/machine.cpp src/execution.cpp src/control.cpp src/collective.cpp
    src/dump.cpp src/module.cpp ${GENERATED_DIR}/isa.hpp)
add_dependencies(simt_core generate_isa)
target_include_directories(simt_core PUBLIC include ${GENERATED_DIR})
target_compile_options(simt_core PRIVATE -Wall -Wextra -Wpedantic -Werror -ffp-contract=off)
add_executable(simt-run src/main.cpp)
target_link_libraries(simt-run PRIVATE simt_core)
target_compile_options(simt-run PRIVATE -Wall -Wextra -Wpedantic -Werror)
option(SIMT_SANITIZE "Enable address and undefined behavior sanitizers" OFF)
if(SIMT_SANITIZE)
    target_compile_options(simt_core PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(simt_core PUBLIC -fsanitize=address,undefined)
endif()

enable_testing()
function(add_simt_test name script)
    add_test(NAME ${name} COMMAND ${CMAKE_COMMAND} -E env
        "PYTHONPATH=${GENERATED_DIR}:${CMAKE_CURRENT_SOURCE_DIR}/python"
        "SIMT_RUNNER=$<TARGET_FILE:simt-run>"
        ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/${script})
    set_tests_properties(${name} PROPERTIES TIMEOUT 90)
endfunction()
add_simt_test(isa_and_execution test_simulator.py)
add_simt_test(control_and_collectives test_control.py)
