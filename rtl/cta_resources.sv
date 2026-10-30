// Admission and retirement use 64 CTA records, 32 physical warps and raw frontend/scheduler wires.
// 准入和退休使用 64 个 CTA 记录、32 个物理 warp 及前端/调度器原始连线。
module cta_resources (
    input wire clk, reset,
    input wire [6:0] cta_limit,
    input wire [5:0] warp_limit,
    input wire [31:0] register_limit, shared_limit,
    input wire request_valid,
    input wire [31:0] request_cta, request_threads, request_shared,
    input wire [5:0] request_slot,
    input wire [31:0] request_warps,
    output wire request_ready, accepted,
    output wire [5:0] grant_slot,
    output wire [31:0] grant_warps, grant_registers, grant_shared,
    input wire [63:0] release_mask, begin_mask, initialize_mask, clean_mask, dma_start_mask, dma_finish_mask,
    input wire [3071:0] lane_states,
    input wire [1023:0] inflight,
    input wire [31:0] query_cta,
    output wire query_found,
    output reg [5:0] query_slot,
    output wire [31:0] query_warps,
    output wire [16:0] query_shared,
    output wire [6:0] query_dma,
    output wire query_table_used, query_table_clean, query_releasing, query_drained,
    output wire [63:0] live, drained, cleanup, releasing,
    output wire [2047:0] owners,
    output wire [31:0] occupied_warps,
    output wire [6:0] occupied_ctas,
    output wire [16:0] occupied_shared,
    output wire [31:0] occupied_registers,
    output wire clock_enable,
    output reg [3:0] error
);
    wire [2047:0] masks;
    wire [1087:0] sizes;
    wire [447:0] dma_counts;
    wire [31:0] query_masks [0:63];
    wire [16:0] query_sizes [0:63];
    wire [6:0] query_dmas [0:63];
    wire [63:0] invalid_events, table_used, table_clean, query_matches, duplicates;
    wire [31:0] done_warps, available_warps, released_warps;
    wire [6:0] remaining_ctas;
    wire [16:0] remaining_shared;
    reg [31:0] valid_lanes [0:31];
    reg [31:0] next_lanes [0:31];
    wire [5:0] required_warps=6'((request_threads+31)>>5);
    wire [31:0] rounded_shared=(request_shared+127)&32'hffffff80;
    wire [31:0] remaining_registers;
    reg [5:0] chosen_slot, chosen_count, selected_count, used_count;
    reg [31:0] chosen_warps, allowed_warps, remaining_threads;
    reg slot_found;
    integer at, w, lane_warp;

    cta_resource_totals current_resources(.live(live),.warp_masks(masks),.shared_sizes(sizes),
        .ctas(occupied_ctas),.shared_bytes(occupied_shared),.warps(occupied_warps));
    cta_resource_totals remaining_resources(.live(live & ~release_mask),.warp_masks(masks),.shared_sizes(sizes),
        .ctas(remaining_ctas),.shared_bytes(remaining_shared),.warps(available_warps));
    assign released_warps=occupied_warps & ~available_warps;
    assign grant_slot=request_valid ? request_slot : chosen_slot;
    assign grant_warps=request_valid ? request_warps : chosen_warps;
    assign grant_shared=rounded_shared;
    assign grant_registers=32'(required_warps)<<11;
    assign remaining_registers=32'(used_count)<<11;
    assign occupied_registers=remaining_registers+(32'(released_count)<<11);
    reg [5:0] released_count;

    // Held request indices prevent a later release from retargeting an already accepted allocation.
    // 保持请求索引，避免随后释放其他槽时改变已接受分配的目标。
    always @* begin
        slot_found=0; chosen_slot=0;
        for (at=0; at<64; at=at+1)
            if (!slot_found && 7'(at)<cta_limit && (!live[at] || release_mask[at])) begin slot_found=1; chosen_slot=6'(at); end
        chosen_count=0; chosen_warps=0; selected_count=0; used_count=0; released_count=0; allowed_warps=0;
        for (w=0; w<32; w=w+1) begin
            if (6'(w)<warp_limit) allowed_warps[w]=1;
            if (6'(w)<warp_limit && !available_warps[w] && chosen_count<required_warps) begin
                chosen_count=chosen_count+1'b1; chosen_warps[w]=1;
            end
            used_count=used_count+{5'b0,available_warps[w]};
            released_count=released_count+{5'b0,released_warps[w]};
            selected_count=selected_count+{5'b0,request_warps[w]};
        end
    end
    always @* begin
        remaining_threads=request_threads;
        for (lane_warp=0; lane_warp<32; lane_warp=lane_warp+1) begin
            next_lanes[lane_warp]=0;
            if (grant_warps[lane_warp]) begin
                next_lanes[lane_warp]=remaining_threads>=32 ? 32'hffffffff : (32'h1<<remaining_threads)-1'b1;
                remaining_threads=remaining_threads>=32 ? remaining_threads-32 : 0;
            end
        end
    end
    wire configuration_valid=cta_limit!=0 && cta_limit<=64 && warp_limit!=0 && warp_limit<=32 &&
        register_limit!=0 && register_limit<=65536 && shared_limit!=0 && shared_limit<=65536 &&
        occupied_ctas<=cta_limit && occupied_registers<=register_limit && 32'(occupied_shared)<=shared_limit &&
        (occupied_warps & ~allowed_warps)==0;
    wire request_validity=request_threads!=0 && request_threads<=1024 && request_shared<=shared_limit;
    wire capacity=slot_found && chosen_count==required_warps && remaining_ctas<cta_limit &&
        grant_registers<=register_limit-remaining_registers && rounded_shared<=shared_limit-32'(remaining_shared);
    always @* begin
        error=0;
        if (!configuration_valid) error=1;
        else if (|invalid_events) error=5;
        else if (request_valid && !request_validity) error=2;
        else if (request_valid && (|duplicates)) error=3;
        else if (request_valid && capacity && ({1'b0,request_slot}>=cta_limit ||
                 (live[request_slot] && !release_mask[request_slot]) || selected_count!=required_warps ||
                 (request_warps & (available_warps | ~allowed_warps))!=0)) error=4;
    end
    wire enabled=!reset && error==0;
    assign request_ready=enabled && request_validity && !(|duplicates) && capacity;
    assign accepted=request_valid && request_ready;
    assign clock_enable=reset || request_valid || (|(release_mask | begin_mask | initialize_mask | clean_mask | dma_start_mask | dma_finish_mask));

    genvar slot, warp, lane;
    generate for (warp=0; warp<32; warp=warp+1) begin: warp_inputs
        wire [31:0] exited;
        for (lane=0; lane<32; lane=lane+1) begin: lanes
            assign exited[lane]=!valid_lanes[warp][lane] || lane_states[(warp*32+lane)*3+:3]==3'd5;
        end
        assign done_warps[warp]=(&exited) && inflight[warp*32+:32]==0;
        always @(posedge clk) begin
            if (reset) valid_lanes[warp]<=0;
            else if (enabled) begin
                if (released_warps[warp]) valid_lanes[warp]<=0;
                if (accepted && grant_warps[warp]) valid_lanes[warp]<=next_lanes[warp];
            end
        end
    end
    for (slot=0; slot<64; slot=slot+1) begin: entries
        wire [31:0] owner;
        cta_resource_slot storage(.clk(clk),.reset(reset),.enable(enabled),
            .allocate(accepted && grant_slot==6'(slot)),.release_slot(release_mask[slot]),.begin_release(begin_mask[slot]),
            .initialize_table(initialize_mask[slot]),.clean_table(clean_mask[slot]),
            .start_dma(dma_start_mask[slot]),.finish_dma(dma_finish_mask[slot]),
            .new_owner(request_cta),.new_warps(grant_warps),.done_warps(done_warps),.new_shared(rounded_shared[16:0]),
            .live(live[slot]),.releasing(releasing[slot]),.table_used(table_used[slot]),.table_clean(table_clean[slot]),
            .owner(owner),.warp_mask(masks[slot*32+:32]),.shared_size(sizes[slot*17+:17]),.dma_count(dma_counts[slot*7+:7]),
            .drained(drained[slot]),.cleanup(cleanup[slot]),.invalid_event(invalid_events[slot]));
        assign owners[slot*32+:32]=live[slot] ? owner : 32'b0;
        assign query_masks[slot]=masks[slot*32+:32];
        assign query_sizes[slot]=sizes[slot*17+:17];
        assign query_dmas[slot]=dma_counts[slot*7+:7];
        assign duplicates[slot]=live[slot] && !release_mask[slot] && owner==request_cta;
        assign query_matches[slot]=live[slot] && owner==query_cta;
    end endgenerate
    assign query_found=|query_matches;
    integer q;
    always @* begin
        query_slot=0;
        for (q=0; q<64; q=q+1) if (query_matches[q]) query_slot=query_slot | 6'(q);
    end
    assign query_warps=query_found ? query_masks[query_slot] : 32'b0;
    assign query_shared=query_found ? query_sizes[query_slot] : 17'b0;
    assign query_dma=query_found ? query_dmas[query_slot] : 7'b0;
    assign query_table_used=query_found && table_used[query_slot];
    assign query_table_clean=query_found && table_clean[query_slot];
    assign query_releasing=query_found && releasing[query_slot];
    assign query_drained=query_found && drained[query_slot];
endmodule
