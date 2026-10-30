// A physical warp slot retains issue credits and serialization flags across arbitrary stalls.
// 物理 warp 槽在任意停顿期间保留发射计数与串行化标志。
module warp_issue_slot (
    input wire clk, reset, commit_enable, allocate, release_slot,
    input wire [5:0] retire_count,
    input wire retire_boundary, retire_memory, issue, issue_boundary, issue_memory,
    output reg live, boundary, memory_pending,
    output reg [5:0] inflight,
    output reg [2:0] error
);
    reg stored_live, stored_boundary, stored_memory;
    reg [5:0] stored_count;
    always @* begin
        live=stored_live; boundary=stored_boundary && !retire_boundary;
        memory_pending=stored_memory && !retire_memory;
        inflight=retire_count<=stored_count ? stored_count-retire_count : stored_count;
        error=0;
        if (retire_count>stored_count) error=1;
        else if ((retire_boundary && (!stored_boundary || retire_count==0)) ||
                 (retire_memory && (!stored_memory || retire_count==0))) error=2;
        else if (release_slot && (!stored_live || inflight!=0 || boundary || memory_pending)) error=3;
        else if (allocate && stored_live && !release_slot) error=4;
        if (release_slot) begin live=0; inflight=0; boundary=0; memory_pending=0; end
        if (allocate) begin live=1; inflight=0; boundary=0; memory_pending=0; end
    end
    always @(posedge clk) begin
        if (reset) begin stored_live<=0; stored_boundary<=0; stored_memory<=0; stored_count<=0; end
        else if (commit_enable && (allocate || release_slot || retire_count!=0 || issue)) begin
            stored_live<=live; stored_count<=inflight+{5'b0,issue};
            stored_boundary<=boundary || (issue && issue_boundary);
            stored_memory<=memory_pending || (issue && issue_memory);
        end
    end
endmodule
