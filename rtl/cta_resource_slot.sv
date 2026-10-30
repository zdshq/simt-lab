// One physical CTA entry owns warp credits, shared credits and asynchronous retirement state.
// 一个物理 CTA 条目持有 warp 额度、shared 额度和异步退休状态。
module cta_resource_slot (
    input wire clk, reset, enable,
    input wire allocate, release_slot, begin_release, initialize_table, clean_table, start_dma, finish_dma,
    input wire [31:0] new_owner, new_warps, done_warps,
    input wire [16:0] new_shared,
    output reg live, releasing, table_used, table_clean,
    output reg [31:0] owner, warp_mask,
    output reg [16:0] shared_size,
    output reg [6:0] dma_count,
    output wire drained, cleanup,
    output wire invalid_event
);
    assign drained=live && (warp_mask & ~done_warps)==0 && dma_count==0;
    assign cleanup=drained && table_used && !table_clean && !releasing;
    wire has_event=release_slot || begin_release || initialize_table || clean_table || start_dma || finish_dma;
    // Releasing latches completed drain checks; reused frontend wires may already describe the next owner.
    // releasing 锁存已完成的排空检查；复用的前端连线此时可能已经描述下个所有者。
    assign invalid_event=(has_event && !live) ||
        (release_slot && (!releasing || initialize_table || clean_table || start_dma || finish_dma || begin_release)) ||
        (begin_release && (releasing || !drained || (table_used && !table_clean) || initialize_table || start_dma)) ||
        (initialize_table && (releasing || clean_table || table_clean)) ||
        (clean_table && (!table_used || table_clean || !drained || releasing || start_dma)) ||
        (start_dma && (releasing || table_clean || (dma_count==64 && !finish_dma))) ||
        (finish_dma && dma_count==0);
    always @(posedge clk) begin
        if (reset) begin
            live<=0; releasing<=0; table_used<=0; table_clean<=0; dma_count<=0;
            owner<=0; warp_mask<=0; shared_size<=0;
        end else if (enable) begin
            if (initialize_table) begin table_used<=1; table_clean<=0; end
            if (clean_table) table_clean<=1;
            if (begin_release) releasing<=1;
            if (start_dma && !finish_dma) dma_count<=dma_count+1'b1;
            if (finish_dma && !start_dma) dma_count<=dma_count-1'b1;
            if (release_slot) begin live<=0; releasing<=0; table_used<=0; table_clean<=0; dma_count<=0; end
            if (allocate) begin
                live<=1; releasing<=0; table_used<=0; table_clean<=0; dma_count<=0;
                owner<=new_owner; warp_mask<=new_warps; shared_size<=new_shared;
            end
        end
    end
endmodule
