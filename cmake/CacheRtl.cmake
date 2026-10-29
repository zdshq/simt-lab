# Compile real L1 and L2 circuits with distinct geometry and generated symbol namespaces.
# 以不同几何参数和生成符号命名空间编译实际 L1 与 L2 电路。
function(simt_enable_cache_rtl)
    find_package(verilator REQUIRED HINTS
        $ENV{VERILATOR_ROOT} ${PROJECT_SOURCE_DIR}/.tools/rtl/root/usr/share/verilator)
    set(launcher ${PROJECT_BINARY_DIR}/simt-cache-verilator)
    configure_file(${PROJECT_SOURCE_DIR}/tools/verilator_cmake.py ${launcher} COPYONLY)
    file(WRITE ${launcher}.config "${VERILATOR_ROOT}\n${VERILATOR_BIN}\n")
    file(CHMOD ${launcher} PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
    set(VERILATOR_BIN ${launcher})
    add_library(simt_cache_circuit STATIC)
    verilate(simt_cache_circuit TOP_MODULE sector_cache PREFIX Vsimt_l1
        SOURCES rtl/sector_sram.sv rtl/sector_cache.sv
        VERILATOR_ARGS -Wall --unroll-count 4096 -GSETS=128 -GWAYS=4)
    verilate(simt_cache_circuit TOP_MODULE sector_cache PREFIX Vsimt_l2
        SOURCES rtl/sector_sram.sv rtl/sector_cache.sv
        VERILATOR_ARGS -Wall --unroll-count 4096 --unroll-stmts 100000 -GSETS=256 -GWAYS=8)
    target_compile_options(simt_cache_circuit PRIVATE ${VERILATOR_CFLAGS})
    get_target_property(circuit_includes simt_cache_circuit INTERFACE_INCLUDE_DIRECTORIES)
    set_property(TARGET simt_cache_circuit PROPERTY INTERFACE_SYSTEM_INCLUDE_DIRECTORIES ${circuit_includes})
    target_sources(simt_hardware PRIVATE src/rtl/cache_engine.cpp)
    target_compile_definitions(simt_hardware PRIVATE SIMT_HAS_CACHE_RTL=1)
    target_link_libraries(simt_hardware PRIVATE simt_cache_circuit)
endfunction()
