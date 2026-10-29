// Four independent vector read/write banks expose accepted port reservations to the issue adapter.
// 四个独立向量读写 bank 向发射适配层暴露已接受的端口预约。
module register_file #(
    parameter integer SLOTS = 8,
    parameter integer SLOT_BITS = $clog2(SLOTS),
    parameter integer ROW_BITS = SLOT_BITS+4
) (
    input wire clk, reset,
    input wire [SLOTS-1:0] clear_slots,
    input wire [5:0] query_register,
    input wire [2:0] port_groups,
    output wire read_available, write_available, clock_enable,
    output wire [7:0] slot_count,
    input wire [3:0] read_valid, write_valid,
    input wire [4*ROW_BITS-1:0] read_rows, write_rows,
    input wire [127:0] write_masks,
    input wire [4095:0] write_data,
    output wire [4095:0] read_data
);
    assign slot_count = 8'(SLOTS);
    // One or two shared port groups serialize accesses across the unchanged four storage banks.
    // 一个或两个共享端口组在不改变四个存储 bank 的前提下串行化访问。
    wire [3:0] group_mask = port_groups == 1 ? 4'b1111 : port_groups == 2 ?
        (query_register[0] ? 4'b1010 : 4'b0101) : (4'b0001 << query_register[1:0]);
    wire valid_geometry = port_groups == 1 || port_groups == 2 || port_groups == 4;
    assign read_available = !reset && valid_geometry && (query_register == 63 || !(|(read_valid & group_mask)));
    assign write_available = !reset && valid_geometry && (query_register == 63 || !(|(write_valid & group_mask)));
    // Controls change only while clk is low; reads are combinational and do not open the storage gate.
    // 控制信号仅在 clk 低电平改变；读取为组合逻辑，不打开存储时钟门。
    assign clock_enable = reset || (|clear_slots) || (|write_valid);
    wire storage_clock = clk && clock_enable;
    genvar bank;
    generate for (bank = 0; bank < 4; bank = bank+1) begin: banks
        register_bank #(.SLOTS(SLOTS)) storage (
            .clk(storage_clock), .reset(reset), .clear_slots(clear_slots),
            .read_valid(read_valid[bank] && (bank != 3 || read_rows[bank*ROW_BITS+:4] != 15)), .read_row(read_rows[bank*ROW_BITS+:ROW_BITS]),
            .read_data(read_data[bank*1024+:1024]),
            .write_valid(write_valid[bank] && (bank != 3 || write_rows[bank*ROW_BITS+:4] != 15)), .write_row(write_rows[bank*ROW_BITS+:ROW_BITS]),
            .write_mask(write_masks[bank*32+:32]), .write_data(write_data[bank*1024+:1024])
        );
    end endgenerate
endmodule
