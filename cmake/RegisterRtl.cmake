# Partition geometries divide the same 65,536-word SM register capacity without duplicating storage.
# 分区几何划分同一份 65,536 字 SM 寄存器容量，不重复配置存储。
function(simt_enable_register_rtl)
    find_package(verilator REQUIRED HINTS
        $ENV{VERILATOR_ROOT} ${PROJECT_SOURCE_DIR}/.tools/rtl/root/usr/share/verilator)
    set(launcher ${PROJECT_BINARY_DIR}/simt-register-verilator)
    configure_file(${PROJECT_SOURCE_DIR}/tools/verilator_cmake.py ${launcher} COPYONLY)
    file(WRITE ${launcher}.config "${VERILATOR_ROOT}\n${VERILATOR_BIN}\n")
    file(CHMOD ${launcher} PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
    set(VERILATOR_BIN ${launcher})
    add_library(simt_register_circuit STATIC)
    foreach(slots 4 8 16 32)
        verilate(simt_register_circuit TOP_MODULE register_file PREFIX Vsimt_register${slots}
            SOURCES rtl/register_bank.sv rtl/register_file.sv
            VERILATOR_ARGS -Wall --unroll-count 4096 -GSLOTS=${slots})
    endforeach()
    target_compile_options(simt_register_circuit PRIVATE ${VERILATOR_CFLAGS})
    get_target_property(circuit_includes simt_register_circuit INTERFACE_INCLUDE_DIRECTORIES)
    set_property(TARGET simt_register_circuit PROPERTY INTERFACE_SYSTEM_INCLUDE_DIRECTORIES ${circuit_includes})
    target_sources(simt_hardware PRIVATE src/rtl/register_file.cpp)
    target_compile_definitions(simt_hardware PRIVATE SIMT_HAS_REGISTER_RTL=1)
    target_link_libraries(simt_hardware PRIVATE simt_register_circuit)
endfunction()
