// One candidate probe per partition and clock bounds decode/predicate work; rejected candidates rotate away.
// 每分区每拍仅检查一个候选，限制译码/谓词工作量；被拒绝的候选让出选择机会。
module warp_scheduler (
    input wire clk, reset,
    input wire [5:0] slot_limit,
    input wire [1:0] policy,
    input wire [31:0] allocate_mask, release_mask, runnable,
    input wire [191:0] retire_counts,
    input wire [31:0] retire_boundaries, retire_memories,
    input wire collector_ready, completion_ready,
    output wire candidate_valid,
    output reg [4:0] candidate,
    input wire decision_valid, guard_ready, dependencies_ready, instruction_boundary, instruction_memory,
    output wire issue_accept,
    output reg [2:0] rejection,
    output wire [31:0] live_mask, boundary_mask, memory_mask,
    output wire [191:0] inflight_counts,
    output reg [10:0] total_inflight,
    output reg [4:0] cursor,
    output reg [31:0] skipped,
    output wire collector_stall, completion_stall, clock_enable,
    output reg [3:0] error
);
    wire [2:0] slot_errors [0:31];
    wire [5:0] slot_counts [0:31];
    wire [31:0] eligible=runnable & live_mask & ~boundary_mask;
    wire [31:0] previous_skips=skipped & ~(allocate_mask | release_mask);
    wire [31:0] untried=eligible & ~previous_skips;
    wire restart=untried==0;
    wire [31:0] choices=restart ? eligible : untried;
    reg found;
    reg [4:0] at;
    integer offset, index;
    always @* begin
        found=0; candidate=0; at=0;
        for (offset=0; offset<32; offset=offset+1) begin
            at=cursor+5'(offset);
            if (policy==1) at=5'(offset);
            else if (policy==2) at=5'(31-offset);
            if (!found && choices[at]) begin found=1; candidate=at; end
        end
    end
    always @* begin
        error=0; total_inflight=0;
        if (slot_limit==0 || slot_limit>32 || policy==3) error=1;
        for (index=0; index<32; index=index+1) begin
            total_inflight=total_inflight+{5'b0,slot_counts[index]};
            if (slot_errors[index]!=0) error={1'b0,slot_errors[index]}+1'b1;
            if (index>={26'b0,slot_limit} && (allocate_mask[index] || release_mask[index] || runnable[index])) error=6;
        end
    end
    assign candidate_valid=!reset && error==0 && found && collector_ready && completion_ready;
    always @* begin
        rejection=0;
        if (!guard_ready) rejection=1;
        else if (instruction_boundary && slot_counts[candidate]!=0) rejection=2;
        else if (instruction_memory && memory_mask[candidate]) rejection=3;
        else if (!dependencies_ready) rejection=4;
        else if (slot_counts[candidate]>=32) rejection=5;
    end
    assign issue_accept=candidate_valid && decision_valid && rejection==0;
    assign completion_stall=!reset && error==0 && eligible!=0 && !completion_ready;
    assign collector_stall=!reset && error==0 && eligible!=0 && completion_ready && !collector_ready;
    assign clock_enable=reset || allocate_mask!=0 || release_mask!=0 || retire_counts!=0 || decision_valid;
    always @(posedge clk) begin
        if (reset) begin cursor<=0; skipped<=0; end
        else if (error==0) begin
            skipped<=previous_skips;
            if (candidate_valid && decision_valid) begin
                cursor<=candidate+1'b1;
                if (issue_accept) skipped<=0;
                else skipped<=(restart ? 32'b0 : previous_skips) | (32'b1<<candidate);
            end
        end
    end
    genvar slot;
    generate for (slot=0; slot<32; slot=slot+1) begin: slots
        assign inflight_counts[slot*6+:6]=slot_counts[slot];
        warp_issue_slot state (
            .clk(clk), .reset(reset), .commit_enable(error==0), .allocate(allocate_mask[slot]), .release_slot(release_mask[slot]),
            .retire_count(retire_counts[slot*6+:6]), .retire_boundary(retire_boundaries[slot]), .retire_memory(retire_memories[slot]),
            .issue(issue_accept && candidate==5'(slot)), .issue_boundary(instruction_boundary), .issue_memory(instruction_memory),
            .live(live_mask[slot]), .boundary(boundary_mask[slot]), .memory_pending(memory_mask[slot]),
            .inflight(slot_counts[slot]), .error(slot_errors[slot])
        );
    end endgenerate
endmodule
