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
    input wire [SLOT_BITS-1:0] predicate_query_slot, predicate_write_slot,
    input wire [3:0] predicate_query_guard,
    input wire [2:0] predicate_query_source, predicate_write_register,
    input wire [31:0] predicate_query_group, predicate_write_mask, predicate_write_data,
    input wire predicate_write_valid,
    output wire [31:0] predicate_participating, predicate_source_data,
    output wire predicate_write_event, predicate_write_error, predicate_clock_enable,
    output wire predicate_port_busy,
    input wire collect_enable,
    input wire [5:0] entry_limit,
    input wire allocate_valid,
    input wire [SLOT_BITS-1:0] allocate_warp,
    input wire [1:0] allocate_count,
    input wire [2:0] allocate_pipe,
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
    output wire [3071:0] query_data,
    input wire dispatch_enable,
    input wire [31:0] dispatch_eligible,
    input wire [6:0] dispatch_accept,
    output wire [6:0] dispatch_valid, dispatch_error,
    output wire [6:0] dispatch_accept_events,
    output wire [34:0] dispatch_entries,
    output wire [31:0] dispatch_releases,
    output wire [13:0] dispatch_counts,
    output wire [125:0] dispatch_registers,
    output wire [21503:0] dispatch_data,
    input wire integer_launch,
    input wire [4:0] integer_collector, integer_entry,
    input wire [7:0] integer_opcode, integer_mode,
    input wire [5:0] integer_ra, integer_rb, integer_rc,
    input wire [31:0] integer_latency, integer_mask, integer_immediate,
    input wire [31:0] integer_warp, integer_block, integer_sm, integer_threads, integer_blocks,
    output wire integer_accept, integer_ready, integer_clock_enable, integer_output_valid,
    output wire [3:0] integer_error,
    output wire [5:0] integer_occupancy,
    input wire floating_launch,
    input wire [4:0] floating_collector, floating_entry,
    input wire [7:0] floating_opcode, floating_mode,
    input wire [5:0] floating_ra, floating_rb, floating_rc,
    input wire [31:0] floating_latency, floating_mask,
    output wire floating_accept, floating_ready, floating_clock_enable, floating_output_valid, floating_protocol_error,
    output wire [3:0] floating_error,
    output wire [5:0] floating_occupancy,
    input wire wb_reserve_valid,
    input wire [SLOT_BITS-1:0] wb_reserve_warp,
    input wire [2:0] wb_reserve_pipe,
    input wire [1:0] wb_reserve_kind,
    input wire [5:0] wb_reserve_register,
    input wire [31:0] wb_reserve_mask,
    output wire wb_reserve_ready, wb_reserve_error,
    output wire [4:0] wb_reserve_index,
    input wire [6:0] wb_complete_valid,
    input wire [34:0] wb_complete_entries,
    input wire [7167:0] wb_complete_data,
    output wire [6:0] wb_complete_accept, wb_complete_error,
    input wire [31:0] wb_acknowledge,
    output wire [31:0] wb_retire_ready, wb_acknowledge_error,
    output wire [31:0] wb_completed_entries,
    output wire [4:0] wb_retire_valid,
    output wire [24:0] wb_retire_entries,
    output wire [5119:0] wb_retire_data,
    output wire [5:0] wb_occupancy,
    output wire wb_clock_enable,
    output wire [3:0] write_events
);
    wire [3:0] collector_reads;
    wire [4*ROW_BITS-1:0] collector_rows;
    assign read_events = collect_enable ? collector_reads : read_valid;
    wire [3:0] wb_writes;
    wire [4*ROW_BITS-1:0] wb_rows;
    wire [127:0] wb_masks;
    wire [4095:0] wb_data;
    wire [SLOT_BITS-1:0] wb_predicate_slot;
    wire [2:0] wb_predicate_register;
    wire [31:0] wb_predicate_mask, wb_predicate_data;
    wire wb_predicate_valid;
    assign predicate_port_busy = wb_predicate_valid || predicate_write_valid;
    assign write_events = write_valid | wb_writes;
    assign clock_enable = register_clock_enable || collector_clock_enable || wb_clock_enable || integer_clock_enable || floating_clock_enable;
    wire integer_selected = dispatch_valid[0] && !dispatch_accept[0] && dispatch_entries[4:0] == integer_collector;
    wire [3:0] integer_arithmetic_error;
    wire [4:0] integer_result_entry;
    wire [1023:0] integer_result_data;
    assign integer_error = integer_launch && !integer_selected ? 4'd8 : integer_arithmetic_error;
    wire floating_selected = dispatch_valid[1] && !dispatch_accept[1] && dispatch_entries[9:5] == floating_collector;
    wire [3:0] floating_arithmetic_error;
    wire [4:0] floating_result_entry;
    wire [1023:0] floating_result_data;
    assign floating_error = floating_launch && !floating_selected ? 4'd6 : floating_arithmetic_error;
    assign dispatch_accept_events = dispatch_accept | {5'b0,floating_accept,integer_accept};
    wire [6:0] actual_complete_valid = wb_complete_valid | {5'b0,floating_output_valid,integer_output_valid};
    wire [34:0] actual_complete_entries = {wb_complete_entries[34:10],
        floating_output_valid ? floating_result_entry : wb_complete_entries[9:5],
        integer_output_valid ? integer_result_entry : wb_complete_entries[4:0]};
    wire [7167:0] actual_complete_data = {wb_complete_data[7167:2048],
        floating_output_valid ? floating_result_data : wb_complete_data[2047:1024],
        integer_output_valid ? integer_result_data : wb_complete_data[1023:0]};
    // Integer operands and results stay on circuit wires; the host supplies only instruction metadata.
    // 整数操作数和结果始终走电路连线；宿主只提供指令元数据。
    integer_execute integers (
        .clk(clk), .reset(reset), .input_valid(integer_launch && integer_selected),
        .input_ready(integer_ready), .input_accept(integer_accept), .input_error(integer_arithmetic_error),
        .input_entry(integer_entry), .latency(integer_latency), .opcode(integer_opcode), .mode(integer_mode),
        .ra(integer_ra), .rb(integer_rb), .rc(integer_rc), .mask(integer_mask), .immediate(integer_immediate),
        .warp_id(integer_warp), .block_id(integer_block), .sm_id(integer_sm), .threads(integer_threads), .blocks(integer_blocks),
        .source_count(dispatch_counts[1:0]), .source_registers(dispatch_registers[17:0]), .source_data(dispatch_data[3071:0]),
        .output_valid(integer_output_valid), .output_ready(wb_complete_accept[0]),
        .output_entry(integer_result_entry), .output_data(integer_result_data),
        .occupancy(integer_occupancy), .clock_enable(integer_clock_enable)
    );
    floating_execute floating_point (
        .clk(clk), .reset(reset), .input_valid(floating_launch && floating_selected),
        .input_ready(floating_ready), .input_accept(floating_accept), .input_error(floating_arithmetic_error),
        .input_entry(floating_entry), .latency(floating_latency), .opcode(floating_opcode), .mode(floating_mode),
        .ra(floating_ra), .rb(floating_rb), .rc(floating_rc), .mask(floating_mask),
        .source_count(dispatch_counts[3:2]), .source_registers(dispatch_registers[35:18]), .source_data(dispatch_data[6143:3072]),
        .output_valid(floating_output_valid), .output_ready(wb_complete_accept[1]),
        .output_entry(floating_result_entry), .output_data(floating_result_data),
        .occupancy(floating_occupancy), .clock_enable(floating_clock_enable), .protocol_error(floating_protocol_error)
    );
    wire [4*ROW_BITS-1:0] actual_rows;
    wire [127:0] actual_masks;
    wire [4095:0] actual_data;
    genvar bank;
    generate for (bank = 0; bank < 4; bank = bank+1) begin: write_ports
        assign actual_rows[bank*ROW_BITS+:ROW_BITS] = wb_writes[bank] ? wb_rows[bank*ROW_BITS+:ROW_BITS] : write_rows[bank*ROW_BITS+:ROW_BITS];
        assign actual_masks[bank*32+:32] = wb_writes[bank] ? wb_masks[bank*32+:32] : write_masks[bank*32+:32];
        assign actual_data[bank*1024+:1024] = wb_writes[bank] ? wb_data[bank*1024+:1024] : write_data[bank*1024+:1024];
    end endgenerate
    register_file #(.SLOTS(SLOTS), .SHARED_CLOCK(1)) registers (
        .clk(clk), .reset(reset), .clear_slots(clear_slots), .query_register(query_register), .port_groups(port_groups),
        .read_available(read_available), .write_available(write_available), .clock_enable(register_clock_enable), .slot_count(slot_count),
        .read_valid(read_events), .write_valid(write_events), .read_rows(collect_enable ? collector_rows : read_rows),
        .write_rows(actual_rows), .write_masks(actual_masks), .write_data(actual_data), .read_data(read_data),
        .predicate_query_slot(predicate_query_slot), .predicate_query_guard(predicate_query_guard),
        .predicate_query_source(predicate_query_source), .predicate_query_group(predicate_query_group),
        .predicate_participating(predicate_participating), .predicate_source_data(predicate_source_data),
        .predicate_write_valid(wb_predicate_valid || predicate_write_valid),
        .predicate_write_slot(wb_predicate_valid ? wb_predicate_slot : predicate_write_slot),
        .predicate_write_register(wb_predicate_valid ? wb_predicate_register : predicate_write_register),
        .predicate_write_mask(wb_predicate_valid ? wb_predicate_mask : predicate_write_mask),
        .predicate_write_data(wb_predicate_valid ? wb_predicate_data : predicate_write_data),
        .predicate_write_event(predicate_write_event), .predicate_write_error(predicate_write_error),
        .predicate_clock_enable(predicate_clock_enable)
    );
    writeback_unit #(.SLOTS(SLOTS)) completions (
        .clk(clk), .reset(reset), .port_groups(port_groups),
        .reserve_valid(wb_reserve_valid), .reserve_warp(wb_reserve_warp), .reserve_pipe(wb_reserve_pipe),
        .reserve_kind(wb_reserve_kind), .reserve_register(wb_reserve_register), .reserve_mask(wb_reserve_mask),
        .reserve_ready(wb_reserve_ready), .reserve_error(wb_reserve_error), .reserve_index(wb_reserve_index),
        .complete_valid(actual_complete_valid), .complete_entries(actual_complete_entries), .complete_data(actual_complete_data),
        .complete_accept(wb_complete_accept), .complete_error(wb_complete_error), .acknowledge(wb_acknowledge),
        .retire_ready(wb_retire_ready), .acknowledge_error(wb_acknowledge_error), .retire_valid(wb_retire_valid),
        .completed_entries(wb_completed_entries),
        .retire_entries(wb_retire_entries), .retire_data(wb_retire_data), .write_valid(wb_writes),
        .write_rows(wb_rows), .write_masks(wb_masks), .write_data(wb_data), .occupancy(wb_occupancy), .clock_enable(wb_clock_enable),
        .predicate_write_valid(wb_predicate_valid), .predicate_write_slot(wb_predicate_slot),
        .predicate_write_register(wb_predicate_register), .predicate_write_mask(wb_predicate_mask), .predicate_write_data(wb_predicate_data)
    );
    operand_collector #(.SLOTS(SLOTS), .SHARED_CLOCK(1)) collectors (
        .clk(clk), .reset(reset), .collect_enable(collect_enable), .entry_limit(entry_limit), .port_groups(port_groups),
        .allocate_valid(allocate_valid), .allocate_warp(allocate_warp), .allocate_count(allocate_count), .allocate_pipe(allocate_pipe),
        .allocate_registers(allocate_registers), .allocate_masks(allocate_masks), .allocate_ready(allocate_ready),
        .allocate_error(allocate_error), .allocate_index(allocate_index), .release_entries(release_entries),
        .release_error(release_error), .ready_entries(ready_entries), .waiting_sources(waiting_sources), .occupancy(occupancy),
        .clock_enable(collector_clock_enable), .read_valid(collector_reads), .read_rows(collector_rows),
        .grant_entries(grant_entries), .grant_sources(grant_sources), .grant_registers(grant_registers), .grant_masks(grant_masks),
        .read_data(read_data), .query_entry(query_entry), .query_count(query_count), .query_registers(query_registers), .query_data(query_data),
        .dispatch_enable(dispatch_enable), .dispatch_eligible(dispatch_eligible), .dispatch_accept(dispatch_accept_events),
        .dispatch_valid(dispatch_valid), .dispatch_error(dispatch_error), .dispatch_entries(dispatch_entries),
        .dispatch_releases(dispatch_releases), .dispatch_counts(dispatch_counts),
        .dispatch_registers(dispatch_registers), .dispatch_data(dispatch_data)
    );
endmodule
