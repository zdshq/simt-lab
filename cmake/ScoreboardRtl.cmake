# Compile the physical warp scoreboard and its bounded same-cycle forwarding network.
# 编译物理 warp scoreboard 及其有界同拍旁路网络。
function(simt_enable_scoreboard_rtl)
    find_package(verilator REQUIRED HINTS
        $ENV{VERILATOR_ROOT} ${PROJECT_SOURCE_DIR}/.tools/rtl/root/usr/share/verilator)
    set(launcher ${PROJECT_BINARY_DIR}/simt-scoreboard-verilator)
    configure_file(${PROJECT_SOURCE_DIR}/tools/verilator_cmake.py ${launcher} COPYONLY)
    file(WRITE ${launcher}.config "${VERILATOR_ROOT}\n${VERILATOR_BIN}\n")
    file(CHMOD ${launcher} PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
    set(VERILATOR_BIN ${launcher})
    add_library(simt_scoreboard_circuit STATIC)
    verilate(simt_scoreboard_circuit TOP_MODULE warp_scoreboard PREFIX Vsimt_scoreboard
        SOURCES rtl/scoreboard_cell.sv rtl/warp_scoreboard.sv
        VERILATOR_ARGS -Wall --unroll-count 4096)
    target_compile_options(simt_scoreboard_circuit PRIVATE ${VERILATOR_CFLAGS})
    get_target_property(circuit_includes simt_scoreboard_circuit INTERFACE_INCLUDE_DIRECTORIES)
    set_property(TARGET simt_scoreboard_circuit PROPERTY INTERFACE_SYSTEM_INCLUDE_DIRECTORIES ${circuit_includes})
    target_sources(simt_hardware PRIVATE src/rtl/scoreboard_engine.cpp)
    target_compile_definitions(simt_hardware PRIVATE SIMT_HAS_SCOREBOARD_RTL=1)
    target_link_libraries(simt_hardware PRIVATE simt_scoreboard_circuit)
endfunction()
