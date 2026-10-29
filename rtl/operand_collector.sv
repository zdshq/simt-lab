// Finite collectors retain source metadata and values; oldest pending sources arbitrate physical ports.
// 有限收集器保存源元数据和值；最老的待收集源竞争物理端口。
module operand_collector #(
    parameter integer ENTRIES = 32,
    parameter integer ENTRY_BITS = $clog2(ENTRIES),
    parameter integer SLOTS = 8,
    parameter integer SLOT_BITS = $clog2(SLOTS),
    parameter integer ROW_BITS = SLOT_BITS+4
) (
    input wire clk, reset, collect_enable,
    input wire [5:0] entry_limit,
    input wire [2:0] port_groups,
    input wire allocate_valid,
    input wire [SLOT_BITS-1:0] allocate_warp,
    input wire [1:0] allocate_count,
    input wire [17:0] allocate_registers,
    input wire [95:0] allocate_masks,
    output reg allocate_ready, allocate_error,
    output reg [ENTRY_BITS-1:0] allocate_index,
    input wire [ENTRIES-1:0] release_entries,
    output wire [ENTRIES-1:0] release_error,
    output wire [ENTRIES-1:0] ready_entries,
    output wire [ENTRIES*3-1:0] waiting_sources,
    output reg [5:0] occupancy,
    output wire clock_enable,
    output reg [3:0] read_valid,
    output reg [4*ROW_BITS-1:0] read_rows,
    output reg [4*ENTRY_BITS-1:0] grant_entries,
    output reg [7:0] grant_sources,
    output reg [23:0] grant_registers,
    output reg [127:0] grant_masks,
    input wire [4095:0] read_data,
    input wire [ENTRY_BITS-1:0] query_entry,
    output reg [1:0] query_count,
    output reg [17:0] query_registers,
    output reg [3071:0] query_data
);
    reg [ENTRIES-1:0] occupied;
    reg [2:0] waiting [0:ENTRIES-1];
    reg [5:0] rank [0:ENTRIES-1];
    reg [SLOT_BITS-1:0] warp [0:ENTRIES-1];
    reg [1:0] source_count [0:ENTRIES-1];
    reg [17:0] source_registers [0:ENTRIES-1];
    reg [95:0] source_masks [0:ENTRIES-1];
    reg [1023:0] values [0:ENTRIES*3-1];
    reg [2:0] granted [0:ENTRIES-1];
    wire [ENTRIES-1:0] releases = release_entries & ready_entries;
    wire allocation = allocate_valid && allocate_ready;
    wire geometry = port_groups == 1 || port_groups == 2 || port_groups == 4;
    integer group_index, entry_index, source_index, bank_index;
    reg [ENTRY_BITS-1:0] chosen_entry;
    integer chosen_source, best_rank, candidate_register, candidate_group;

    // Rank is compacted on release, so a long-lived collector cannot wrap an issue timestamp.
    // 释放时压缩相对排名，因此长期存活的收集器不会发生发射时间戳回绕。
    function [5:0] removed_before(input [5:0] position);
        integer other;
        begin
            removed_before = 0;
            for (other = 0; other < ENTRIES; other = other+1)
                if (releases[other] && rank[other] < position) removed_before = removed_before+1;
        end
    endfunction

    always @* begin
        read_valid = 0; read_rows = 0; grant_entries = 0; grant_sources = 0;
        grant_registers = 0; grant_masks = 0;
        chosen_entry = 0; chosen_source = 0; best_rank = ENTRIES;
        candidate_register = 0; candidate_group = 0; bank_index = 0;
        for (entry_index = 0; entry_index < ENTRIES; entry_index = entry_index+1) granted[entry_index] = 0;
        for (group_index = 0; group_index < 4; group_index = group_index+1) begin
            best_rank = ENTRIES; chosen_entry = 0; chosen_source = 0;
            for (entry_index = 0; entry_index < ENTRIES; entry_index = entry_index+1)
                for (source_index = 0; source_index < 3; source_index = source_index+1) begin
                    candidate_register = 32'(source_registers[entry_index][source_index*6+:6]);
                    candidate_group = port_groups == 1 ? 0 : port_groups == 2 ? candidate_register%2 : candidate_register%4;
                    if (collect_enable && !reset && geometry && occupied[entry_index] && waiting[entry_index][source_index] &&
                        candidate_group == group_index && 32'(rank[entry_index]) < best_rank) begin
                        best_rank = 32'(rank[entry_index]); chosen_entry = ENTRY_BITS'(entry_index); chosen_source = source_index;
                    end
                end
            if (best_rank < ENTRIES) begin
                candidate_register = 32'(source_registers[chosen_entry][chosen_source*6+:6]);
                bank_index = candidate_register%4;
                read_valid[bank_index] = 1; granted[chosen_entry][chosen_source] = 1;
                read_rows[bank_index*ROW_BITS+:ROW_BITS] = {warp[chosen_entry], 4'(candidate_register/4)};
                grant_entries[bank_index*ENTRY_BITS+:ENTRY_BITS] = ENTRY_BITS'(chosen_entry);
                grant_sources[bank_index*2+:2] = 2'(chosen_source);
                grant_registers[bank_index*6+:6] = 6'(candidate_register);
                grant_masks[bank_index*32+:32] = source_masks[chosen_entry][chosen_source*32+:32];
            end
        end
    end

    genvar entry;
    generate for (entry = 0; entry < ENTRIES; entry = entry+1) begin: status
        assign waiting_sources[entry*3+:3] = occupied[entry] ? waiting[entry] : 3'b0;
        assign ready_entries[entry] = !reset && occupied[entry] && (waiting[entry] & ~granted[entry]) == 0;
    end endgenerate
    assign release_error = release_entries & ~ready_entries;
    assign clock_enable = reset || allocate_valid || (|release_entries) || collect_enable;
    wire state_clock = clk && clock_enable;
    integer free_index, check_source, check_other;
    reg found_free;
    reg [5:0] remaining;
    always @* begin
        occupancy = 0; remaining = 0; allocate_index = 0; found_free = 0; allocate_error = 0;
        for (free_index = 0; free_index < ENTRIES; free_index = free_index+1) begin
            occupancy = occupancy + 6'(occupied[free_index]);
            remaining = remaining + 6'(occupied[free_index] && !releases[free_index]);
            if ((!occupied[free_index] || releases[free_index]) && !found_free) begin
                allocate_index = ENTRY_BITS'(free_index); found_free = 1;
            end
        end
        for (check_source = 0; check_source < 3; check_source = check_source+1)
            if (check_source < 32'(allocate_count) && allocate_valid) begin
                if (allocate_registers[check_source*6+:6] == 63 || allocate_masks[check_source*32+:32] == 0) allocate_error = 1;
                for (check_other = 0; check_other < check_source; check_other = check_other+1)
                    if (allocate_registers[check_source*6+:6] == allocate_registers[check_other*6+:6]) allocate_error = 1;
            end
        allocate_ready = !reset && found_free && entry_limit != 0 && 32'(entry_limit) <= ENTRIES &&
            remaining < entry_limit && !allocate_error;
    end

    integer query_source, query_lane, query_bank;
    reg [1023:0] selected_value;
    always @* begin
        query_count = 0; query_registers = 0; query_data = 0; selected_value = 0; query_bank = 0;
        if (ready_entries[query_entry]) begin
            query_count = source_count[query_entry]; query_registers = source_registers[query_entry];
            for (query_source = 0; query_source < 3; query_source = query_source+1) begin
                query_bank = 32'(source_registers[query_entry][query_source*6+:6])%4;
                selected_value = granted[query_entry][query_source] ? read_data[query_bank*1024+:1024] : values[32'(query_entry)*3+query_source];
                for (query_lane = 0; query_lane < 32; query_lane = query_lane+1)
                    if (query_source < 32'(query_count) && source_masks[query_entry][query_source*32+query_lane])
                        query_data[query_source*1024+query_lane*32+:32] = selected_value[query_lane*32+:32];
            end
        end
    end

    integer update_entry, update_source, update_bank;
    always @(posedge state_clock) begin
        if (reset) occupied <= 0;
        else begin
            occupied <= occupied & ~releases;
            for (update_entry = 0; update_entry < ENTRIES; update_entry = update_entry+1) begin
                if (occupied[update_entry]) begin
                    rank[update_entry] <= rank[update_entry] - removed_before(rank[update_entry]);
                    waiting[update_entry] <= waiting[update_entry] & ~granted[update_entry];
                end
            end
            for (update_bank = 0; update_bank < 4; update_bank = update_bank+1)
                if (read_valid[update_bank])
                    values[32'(grant_entries[update_bank*ENTRY_BITS+:ENTRY_BITS])*3+32'(grant_sources[update_bank*2+:2])] <= read_data[update_bank*1024+:1024];
            if (allocation) begin
                occupied[allocate_index] <= 1; rank[allocate_index] <= remaining;
                warp[allocate_index] <= allocate_warp; source_count[allocate_index] <= allocate_count;
                source_registers[allocate_index] <= allocate_registers; source_masks[allocate_index] <= allocate_masks;
                for (update_source = 0; update_source < 3; update_source = update_source+1)
                    waiting[allocate_index][update_source] <= update_source < 32'(allocate_count);
            end
        end
    end
endmodule
