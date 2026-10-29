// Each set owns a small rank register bank; only its selected way drives local comparisons.
// 每个组持有一个小型排名寄存器组，仅由选中 way 驱动组内比较。
module cache_lru #(
    parameter integer SETS = 128,
    parameter integer WAYS = 4,
    parameter integer SET_BITS = $clog2(SETS),
    parameter integer RANK_BITS = $clog2(WAYS)
) (
    input wire clk,
    input wire reset,
    input wire [SET_BITS-1:0] lookup_set,
    input wire touch,
    input wire [RANK_BITS-1:0] touch_way,
    output wire [WAYS*RANK_BITS-1:0] lookup_ranks
);
    wire [WAYS*RANK_BITS-1:0] ranks_by_set [0:SETS-1];
    assign lookup_ranks = ranks_by_set[lookup_set];

    genvar set_at, way_at;
    generate for (set_at = 0; set_at < SETS; set_at = set_at+1) begin: sets
        wire enabled = touch && lookup_set == SET_BITS'(set_at);
        wire [RANK_BITS-1:0] selected_rank = ranks_by_set[set_at][touch_way*RANK_BITS+:RANK_BITS];
        for (way_at = 0; way_at < WAYS; way_at = way_at+1) begin: ways
            reg [RANK_BITS-1:0] rank;
            assign ranks_by_set[set_at][way_at*RANK_BITS+:RANK_BITS] = rank;
            // Moving a way to MRU increments only the ranks that were more recent than it.
            // 将一个 way 移到 MRU 时，只增加原先比它更新的排名。
            always @(posedge clk) begin
                if (reset) rank <= RANK_BITS'(way_at);
                else if (enabled) begin
                    if (touch_way == RANK_BITS'(way_at)) rank <= 0;
                    else if (rank < selected_rank) rank <= rank+1'b1;
                end
            end
        end
    end endgenerate
endmodule
