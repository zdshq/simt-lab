# Use Verilator's own CMake dependency tracking while keeping private-install environment scoped to its child.
# 使用 Verilator 自带的 CMake 依赖跟踪，仅为其子进程设置私有安装环境。
function(simt_enable_tensor_rtl)
    find_package(verilator REQUIRED HINTS
        $ENV{VERILATOR_ROOT} ${PROJECT_SOURCE_DIR}/.tools/rtl/root/usr/share/verilator)
    set(launcher ${PROJECT_BINARY_DIR}/simt-verilator)
    configure_file(${PROJECT_SOURCE_DIR}/tools/verilator_cmake.py ${launcher} COPYONLY)
    file(WRITE ${launcher}.config "${VERILATOR_ROOT}\n${VERILATOR_BIN}\n")
    file(CHMOD ${launcher} PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
    set(VERILATOR_BIN ${launcher})
    add_library(simt_tensor_circuit STATIC)
    verilate(simt_tensor_circuit TOP_MODULE tensor_array PREFIX Vsimt_tensor
        SOURCES rtl/fp32_fma.sv rtl/int8_mac.sv rtl/tensor_decode.sv rtl/tensor_pe.sv rtl/tensor_array.sv
        VERILATOR_ARGS -Wall)
    target_compile_options(simt_tensor_circuit PRIVATE ${VERILATOR_CFLAGS})
    get_target_property(circuit_includes simt_tensor_circuit INTERFACE_INCLUDE_DIRECTORIES)
    set_property(TARGET simt_tensor_circuit PROPERTY INTERFACE_SYSTEM_INCLUDE_DIRECTORIES ${circuit_includes})
    target_sources(simt_core PRIVATE src/rtl/tensor_engine.cpp)
    target_compile_definitions(simt_core PRIVATE SIMT_HAS_TENSOR_RTL=1)
    target_link_libraries(simt_core PRIVATE simt_tensor_circuit)
endfunction()
