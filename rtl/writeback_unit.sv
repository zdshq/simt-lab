// Reservations preserve issue age; seven producer ports feed finite result storage and RF arbitration.
// 预约保留发射年龄；七个生产端口连接有限结果存储与寄存器写回仲裁。
module writeback_unit #(
    parameter integer SLOTS = 8,
    parameter integer ENTRIES = 32,
    parameter integer SLOT_BITS = $clog2(SLOTS),
    parameter integer ROW_BITS = SLOT_BITS+4,
    parameter integer ENTRY_BITS = $clog2(ENTRIES)
) (
    input wire clk, reset,
    input wire [2:0] port_groups,
    input wire reserve_valid,
    input wire [SLOT_BITS-1:0] reserve_warp,
    input wire [2:0] reserve_pipe,
    input wire [1:0] reserve_kind,
    input wire [5:0] reserve_register,
    input wire [31:0] reserve_mask,
    output wire reserve_ready, reserve_error,
    output reg [ENTRY_BITS-1:0] reserve_index,
    input wire [6:0] complete_valid,
    input wire [7*ENTRY_BITS-1:0] complete_entries,
    input wire [7167:0] complete_data,
    output reg [6:0] complete_accept,
    output wire [6:0] complete_error,
    input wire [ENTRIES-1:0] acknowledge,
    output reg [ENTRIES-1:0] retire_ready,
    output wire [ENTRIES-1:0] acknowledge_error,
    output reg [4:0] retire_valid,
    output reg [5*ENTRY_BITS-1:0] retire_entries,
    output reg [5119:0] retire_data,
    output reg [3:0] write_valid,
    output reg [4*ROW_BITS-1:0] write_rows,
    output reg [127:0] write_masks,
    output wire [4095:0] write_data,
    output reg [5:0] occupancy,
    output wire clock_enable
);
    reg [ENTRIES-1:0] occupied, finished;
    reg [SLOT_BITS-1:0] warp [0:ENTRIES-1];
    reg [2:0] pipe [0:ENTRIES-1];
    reg [1:0] kind [0:ENTRIES-1];
    reg [5:0] destination [0:ENTRIES-1];
    reg [31:0] mask [0:ENTRIES-1];
    reg [5:0] rank [0:ENTRIES-1];
    reg [5:0] removed [0:ENTRIES-1];
    reg [ENTRY_BITS-1:0] result_entry;
    reg [ENTRIES-1:0] arriving;
    wire [ENTRIES-1:0] ready = occupied & (finished | arriving);
    wire [ENTRIES-1:0] released = acknowledge & retire_ready;
    wire [ENTRIES*1024-1:0] results;
    wire groups_valid = port_groups == 1 || port_groups == 2 || port_groups == 4;
    wire descriptor_valid = reserve_pipe < 7 && reserve_kind < 3 &&
                            (reserve_kind != 2 || reserve_register < 7);
    reg free_found;
    integer alloc_entry, producer, group_index, candidate, winner, oldest, bank;
    integer output_port, output_entry, update_entry, rank_entry, older_entry, remaining;
    assign reserve_ready = !reset && groups_valid && descriptor_valid && free_found;
    assign reserve_error = reserve_valid && (!groups_valid || !descriptor_valid);
    assign complete_error = complete_valid & ~complete_accept;
    assign acknowledge_error = acknowledge & ~retire_ready;
    assign clock_enable = reset || (reserve_valid && reserve_ready) || |complete_accept || |released;
    assign write_data = retire_data[4095:0];

    // Only the reserved producer may complete a slot, and duplicate completions cannot replace stored data.
    // 只有预约的生产端口可以完成该槽；重复完成不能覆盖已保存的数据。
    always @* begin
        complete_accept = 0; arriving = 0; result_entry = 0;
        for (producer = 0; producer < 7; producer = producer+1) begin
            result_entry = complete_entries[producer*ENTRY_BITS+:ENTRY_BITS];
            if (!reset && complete_valid[producer] && occupied[result_entry] && !finished[result_entry] &&
                pipe[result_entry] == 3'(producer)) begin
                complete_accept[producer] = 1; arriving[result_entry] = 1;
            end
        end
    end
    genvar entry;
    generate for (entry = 0; entry < ENTRIES; entry = entry+1) begin: payload
        writeback_payload value (.clk(clk), .capture(arriving[entry]), .source(pipe[entry]),
            .completion_data(complete_data), .data(results[entry*1024+:1024]));
    end endgenerate

    // Grants ignore acknowledgement so a port cannot be consumed twice during one clock phase.
    // 授权不受确认位影响，避免同一时钟相位内重复占用端口。
    always @* begin
        retire_ready = 0; retire_valid = 0; retire_entries = 0;
        winner = -1; oldest = ENTRIES; bank = 0;
        if (!reset && groups_valid) begin
            for (candidate = 0; candidate < ENTRIES; candidate = candidate+1)
                if (ready[candidate] && kind[candidate] == 0) retire_ready[candidate] = 1;
            for (group_index = 0; group_index < 5; group_index = group_index+1) begin
                winner = -1; oldest = ENTRIES;
                for (candidate = 0; candidate < ENTRIES; candidate = candidate+1)
                    if (ready[candidate] && 32'(rank[candidate]) < oldest &&
                        ((group_index < 4 && group_index < 32'(port_groups) && kind[candidate] == 1 &&
                          (32'(destination[candidate]) & (32'(port_groups)-1)) == group_index) ||
                         (group_index == 4 && kind[candidate] == 2))) begin
                        winner = candidate; oldest = 32'(rank[candidate]);
                    end
                if (winner >= 0) begin
                    bank = group_index == 4 ? 4 : 32'(destination[winner][1:0]);
                    retire_ready[winner] = 1; retire_valid[bank] = 1;
                    retire_entries[bank*ENTRY_BITS+:ENTRY_BITS] = ENTRY_BITS'(winner);
                end
            end
        end
    end

    // Physical retirement buses directly drive bank addresses, lane masks and data after acknowledgement.
    // 物理退休总线在确认后直接驱动 bank 地址、lane 掩码和数据。
    always @* begin
        retire_data = 0; write_valid = 0; write_rows = 0; write_masks = 0;
        for (output_port = 0; output_port < 5; output_port = output_port+1)
            for (output_entry = 0; output_entry < ENTRIES; output_entry = output_entry+1)
                if (retire_valid[output_port] && retire_entries[output_port*ENTRY_BITS+:ENTRY_BITS] == ENTRY_BITS'(output_entry)) begin
                    retire_data[output_port*1024+:1024] = results[output_entry*1024+:1024];
                    if (output_port < 4) begin
                        write_valid[output_port] = acknowledge[output_entry];
                        write_rows[output_port*ROW_BITS+:ROW_BITS] = {warp[output_entry], destination[output_entry][5:2]};
                        write_masks[output_port*32+:32] = mask[output_entry];
                    end
                end
    end

    always @* begin
        occupancy = 0; reserve_index = 0; free_found = 0; remaining = 0;
        for (alloc_entry = 0; alloc_entry < ENTRIES; alloc_entry = alloc_entry+1) begin
            occupancy = occupancy + {5'b0, occupied[alloc_entry]};
            if (occupied[alloc_entry] && !released[alloc_entry]) remaining = remaining+1;
            if ((!occupied[alloc_entry] || released[alloc_entry]) && !free_found) begin
                reserve_index = ENTRY_BITS'(alloc_entry); free_found = 1;
            end
        end
    end

    // Compact relative ages on retirement; reusing a low numbered slot must not make it older.
    // 退休时压紧相对年龄；复用低编号槽不能让新指令变老。
    always @* begin
        for (rank_entry = 0; rank_entry < ENTRIES; rank_entry = rank_entry+1) begin
            removed[rank_entry] = 0;
            for (older_entry = 0; older_entry < ENTRIES; older_entry = older_entry+1)
                if (released[older_entry] && rank[older_entry] < rank[rank_entry]) removed[rank_entry] = removed[rank_entry]+1'b1;
        end
    end
    always @(posedge clk) begin
        if (reset) begin occupied <= 0; finished <= 0; end
        else begin
            for (update_entry = 0; update_entry < ENTRIES; update_entry = update_entry+1) begin
                if (arriving[update_entry]) finished[update_entry] <= 1;
                if (released[update_entry]) begin occupied[update_entry] <= 0; finished[update_entry] <= 0; end
                if (occupied[update_entry]) rank[update_entry] <= rank[update_entry]-removed[update_entry];
            end
            if (reserve_valid && reserve_ready) begin
                occupied[reserve_index] <= 1; finished[reserve_index] <= 0;
                warp[reserve_index] <= reserve_warp; pipe[reserve_index] <= reserve_pipe;
                kind[reserve_index] <= reserve_mask == 0 || (reserve_kind == 1 && reserve_register == 63) ? 2'b0 : reserve_kind;
                destination[reserve_index] <= reserve_register; mask[reserve_index] <= reserve_mask;
                rank[reserve_index] <= 6'(remaining);
            end
        end
    end
endmodule
