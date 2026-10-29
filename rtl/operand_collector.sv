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
    input wire [2:0] allocate_pipe,
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
    output reg [3071:0] query_data,
    input wire dispatch_enable,
    input wire [ENTRIES-1:0] dispatch_eligible,
    input wire [6:0] dispatch_accept,
    output wire [6:0] dispatch_valid, dispatch_error,
    output wire [7*ENTRY_BITS-1:0] dispatch_entries,
    output wire [ENTRIES-1:0] dispatch_releases,
    output wire [13:0] dispatch_counts,
    output wire [125:0] dispatch_registers,
    output wire [21503:0] dispatch_data
);
    reg [ENTRIES-1:0] occupied;
    reg [2:0] waiting [0:ENTRIES-1];
    reg [5:0] rank [0:ENTRIES-1];
    reg [2:0] pipe [0:ENTRIES-1];
    wire [ENTRIES*6-1:0] packed_ranks;
    wire [ENTRIES*3-1:0] packed_pipes;
    reg [SLOT_BITS-1:0] warp [0:ENTRIES-1];
    reg [1:0] source_count [0:ENTRIES-1];
    reg [17:0] source_registers [0:ENTRIES-1];
    reg [95:0] source_masks [0:ENTRIES-1];
    wire [3071:0] prepared [0:ENTRIES-1];
    reg [2:0] granted [0:ENTRIES-1];
    wire [ENTRIES-1:0] releases = (release_entries & ready_entries) | dispatch_releases;
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
        assign packed_ranks[entry*6+:6] = rank[entry];
        assign packed_pipes[entry*3+:3] = pipe[entry];
        assign waiting_sources[entry*3+:3] = occupied[entry] ? waiting[entry] : 3'b0;
        assign ready_entries[entry] = !reset && occupied[entry] && (waiting[entry] & ~granted[entry]) == 0;
    end endgenerate
    assign release_error = release_entries & ~ready_entries;
    assign clock_enable = reset || allocate_valid || (|release_entries) || collect_enable || (|dispatch_accept);
    wire state_clock = clk && clock_enable;
    integer free_index, check_source, check_other;
    reg found_free;
    reg [5:0] remaining;
    always @* begin
        occupancy = 0; remaining = 0; allocate_index = 0; found_free = 0; allocate_error = allocate_valid && allocate_pipe == 7;
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

    // Seven physical vector buses replace serial host queries in the execution path.
    // 执行路径用七条物理向量总线替代宿主串行查询。
    collector_dispatch #(.ENTRIES(ENTRIES)) dispatch (
        .enable(dispatch_enable && !reset), .ready_entries(ready_entries), .eligible_entries(dispatch_eligible),
        .pipes(packed_pipes), .ranks(packed_ranks), .valid(dispatch_valid), .entries(dispatch_entries)
    );
    assign dispatch_error = dispatch_accept & ~dispatch_valid;
    genvar port_index, release_index;
    generate for (release_index = 0; release_index < ENTRIES; release_index = release_index+1) begin: accept_entry
        wire [6:0] accepted;
        for (port_index = 0; port_index < 7; port_index = port_index+1) begin: from_port
            assign accepted[port_index] = dispatch_accept[port_index] && dispatch_valid[port_index] &&
                dispatch_entries[port_index*ENTRY_BITS+:ENTRY_BITS] == ENTRY_BITS'(release_index);
        end
        assign dispatch_releases[release_index] = |accepted;
    end endgenerate

    // Repeated entries instantiate one payload design instead of flattening every lane's storage logic.
    // 重复条目实例化同一载荷电路，不将全部 lane 存储逻辑摊入顶层。
    genvar payload_entry;
    generate for (payload_entry = 0; payload_entry < ENTRIES; payload_entry = payload_entry+1) begin: payload
        collector_payload storage (
            .clk(state_clock), .capture(granted[payload_entry]),
            .banks({source_registers[payload_entry][12+:2],source_registers[payload_entry][6+:2],source_registers[payload_entry][0+:2]}),
            .source_count(source_count[payload_entry]), .masks(source_masks[payload_entry]),
            .bank_data(read_data), .data(prepared[payload_entry])
        );
    end endgenerate

    function [3071:0] operand_data(input [ENTRY_BITS-1:0] selected_entry);
        integer candidate;
        begin
            operand_data = 0;
            if (ready_entries[selected_entry] && source_count[selected_entry] != 0)
                for (candidate = 0; candidate < ENTRIES; candidate = candidate+1)
                    if (selected_entry == ENTRY_BITS'(candidate)) operand_data = prepared[candidate];
        end
    endfunction

    always @* begin
        query_count = ready_entries[query_entry] ? source_count[query_entry] : 2'b0;
        query_registers = ready_entries[query_entry] ? source_registers[query_entry] : 18'b0;
        query_data = operand_data(query_entry);
    end
    generate for (port_index = 0; port_index < 7; port_index = port_index+1) begin: execution_port
        wire [ENTRY_BITS-1:0] selected_entry = dispatch_entries[port_index*ENTRY_BITS+:ENTRY_BITS];
        assign dispatch_counts[port_index*2+:2] = dispatch_valid[port_index] ? source_count[selected_entry] : 2'b0;
        assign dispatch_registers[port_index*18+:18] = dispatch_valid[port_index] ? source_registers[selected_entry] : 18'b0;
        assign dispatch_data[port_index*3072+:3072] = dispatch_valid[port_index] ? operand_data(selected_entry) : 3072'b0;
    end endgenerate

    integer update_entry, update_source;
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
            if (allocation) begin
                occupied[allocate_index] <= 1; rank[allocate_index] <= remaining;
                pipe[allocate_index] <= allocate_pipe;
                warp[allocate_index] <= allocate_warp; source_count[allocate_index] <= allocate_count;
                source_registers[allocate_index] <= allocate_registers; source_masks[allocate_index] <= allocate_masks;
                for (update_source = 0; update_source < 3; update_source = update_source+1)
                    waiting[allocate_index][update_source] <= update_source < 32'(allocate_count);
            end
        end
    end
endmodule
