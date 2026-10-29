// Direct wires carry register-file vectors into collector storage without a host data buffer.
// 寄存器堆向量通过直接连线进入收集器存储，不经过宿主数据缓冲。
module register_operand_unit #(
    parameter integer SLOTS = 8,
    parameter integer SLOT_BITS = $clog2(SLOTS),
    parameter integer ROW_BITS = SLOT_BITS+4
) (
    input wire clk, reset,
    input wire [SLOTS-1:0] clear_slots,
    input wire [5:0] query_register,
    input wire [2:0] port_groups,
    output wire read_available, write_available, clock_enable, register_clock_enable, collector_clock_enable,
    output wire [7:0] slot_count,
    input wire [3:0] read_valid, write_valid,
    input wire [4*ROW_BITS-1:0] read_rows, write_rows,
    input wire [127:0] write_masks,
    input wire [4095:0] write_data,
    output wire [4095:0] read_data,
    input wire collect_enable,
    input wire [5:0] entry_limit,
    input wire allocate_valid,
    input wire [SLOT_BITS-1:0] allocate_warp,
    input wire [1:0] allocate_count,
    input wire [17:0] allocate_registers,
    input wire [95:0] allocate_masks,
    output wire allocate_ready, allocate_error,
    output wire [4:0] allocate_index,
    input wire [31:0] release_entries,
    output wire [31:0] release_error, ready_entries,
    output wire [95:0] waiting_sources,
    output wire [5:0] occupancy,
    output wire [3:0] read_events,
    output wire [19:0] grant_entries,
    output wire [7:0] grant_sources,
    output wire [23:0] grant_registers,
    output wire [127:0] grant_masks,
    input wire [4:0] query_entry,
    output wire [1:0] query_count,
    output wire [17:0] query_registers,
    output wire [3071:0] query_data
);
    wire [3:0] collector_reads;
    wire [4*ROW_BITS-1:0] collector_rows;
    assign read_events = collect_enable ? collector_reads : read_valid;
    assign clock_enable = register_clock_enable || collector_clock_enable;
    register_file #(.SLOTS(SLOTS)) registers (
        .clk(clk), .reset(reset), .clear_slots(clear_slots), .query_register(query_register), .port_groups(port_groups),
        .read_available(read_available), .write_available(write_available), .clock_enable(register_clock_enable), .slot_count(slot_count),
        .read_valid(read_events), .write_valid(write_valid), .read_rows(collect_enable ? collector_rows : read_rows),
        .write_rows(write_rows), .write_masks(write_masks), .write_data(write_data), .read_data(read_data)
    );
    operand_collector #(.SLOTS(SLOTS)) collectors (
        .clk(clk), .reset(reset), .collect_enable(collect_enable), .entry_limit(entry_limit), .port_groups(port_groups),
        .allocate_valid(allocate_valid), .allocate_warp(allocate_warp), .allocate_count(allocate_count),
        .allocate_registers(allocate_registers), .allocate_masks(allocate_masks), .allocate_ready(allocate_ready),
        .allocate_error(allocate_error), .allocate_index(allocate_index), .release_entries(release_entries),
        .release_error(release_error), .ready_entries(ready_entries), .waiting_sources(waiting_sources), .occupancy(occupancy),
        .clock_enable(collector_clock_enable), .read_valid(collector_reads), .read_rows(collector_rows),
        .grant_entries(grant_entries), .grant_sources(grant_sources), .grant_registers(grant_registers), .grant_masks(grant_masks),
        .read_data(read_data), .query_entry(query_entry), .query_count(query_count), .query_registers(query_registers), .query_data(query_data)
    );
endmodule
