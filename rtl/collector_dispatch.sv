// Each execution port selects its oldest eligible ready collector independently.
// 每个执行端口独立选择符合准入条件的最老就绪收集器。
module collector_dispatch #(
    parameter integer ENTRIES = 32,
    parameter integer ENTRY_BITS = $clog2(ENTRIES)
) (
    input wire enable,
    input wire [ENTRIES-1:0] ready_entries, eligible_entries,
    input wire [ENTRIES*3-1:0] pipes,
    input wire [ENTRIES*6-1:0] ranks,
    output reg [6:0] valid,
    output reg [7*ENTRY_BITS-1:0] entries
);
    integer port_index, entry_index, best_rank;
    always @* begin
        valid = 0; entries = 0; best_rank = ENTRIES;
        for (port_index = 0; port_index < 7; port_index = port_index+1) begin
            best_rank = ENTRIES;
            for (entry_index = 0; entry_index < ENTRIES; entry_index = entry_index+1)
                if (enable && ready_entries[entry_index] && eligible_entries[entry_index] &&
                    pipes[entry_index*3+:3] == 3'(port_index) && 32'(ranks[entry_index*6+:6]) < best_rank) begin
                    best_rank = 32'(ranks[entry_index*6+:6]);
                    valid[port_index] = 1;
                    entries[port_index*ENTRY_BITS+:ENTRY_BITS] = ENTRY_BITS'(entry_index);
                end
        end
    end
endmodule
