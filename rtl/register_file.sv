// Four independent vector read/write banks expose accepted port reservations to the issue adapter.
// 四个独立向量读写 bank 向发射适配层暴露已接受的端口预约。
module register_file #(
    parameter integer SLOTS = 8,
    parameter integer SHARED_CLOCK = 0,
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
    output wire [4095:0] read_data,
    input wire [SLOT_BITS-1:0] predicate_query_slot, predicate_write_slot,
    input wire [3:0] predicate_query_guard,
    input wire [2:0] predicate_query_source, predicate_write_register,
    input wire [31:0] predicate_query_group, predicate_write_mask, predicate_write_data,
    input wire predicate_write_valid,
    input wire [SLOT_BITS-1:0] execution_predicate_slot,
    input wire [2:0] execution_predicate_source,
    output wire [31:0] execution_predicate_data,
    output wire [31:0] predicate_participating, predicate_source_data,
    output wire predicate_write_event, predicate_write_error, predicate_clock_enable
);
    assign slot_count = 8'(SLOTS);
    // One or two shared port groups serialize accesses across the unchanged four storage banks.
    // 一个或两个共享端口组在不改变四个存储 bank 的前提下串行化访问。
    wire [3:0] group_mask = port_groups == 1 ? 4'b1111 : port_groups == 2 ?
        (query_register[0] ? 4'b1010 : 4'b0101) : (4'b0001 << query_register[1:0]);
    wire valid_geometry = port_groups == 1 || port_groups == 2 || port_groups == 4;
    assign read_available = !reset && valid_geometry && (query_register == 63 || !(|(read_valid & group_mask)));
    assign write_available = !reset && valid_geometry && (query_register == 63 || !(|(write_valid & group_mask)));
    // Standalone host pins stay stable while high; internal RTL producers instead share the source clock.
    // 独立宿主引脚在高电平保持稳定；内部 RTL 生产者则使用共同的源时钟。
    assign clock_enable = reset || (|clear_slots) || (|write_valid) || predicate_clock_enable;
    wire storage_clock = SHARED_CLOCK != 0 ? clk : clk && clock_enable;
    predicate_file #(.SLOTS(SLOTS)) predicates (
        .clk(storage_clock), .reset(reset), .clear_slots(clear_slots),
        .query_slot(predicate_query_slot), .query_guard(predicate_query_guard), .query_source(predicate_query_source),
        .query_group(predicate_query_group), .participating(predicate_participating), .source_data(predicate_source_data),
        .execution_slot(execution_predicate_slot), .execution_source(execution_predicate_source), .execution_data(execution_predicate_data),
        .write_valid(predicate_write_valid), .write_slot(predicate_write_slot), .write_register(predicate_write_register),
        .write_mask(predicate_write_mask), .write_data(predicate_write_data), .write_accept(predicate_write_event),
        .write_error(predicate_write_error), .clock_enable(predicate_clock_enable)
    );
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
