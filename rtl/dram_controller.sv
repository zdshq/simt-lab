// The on-chip controller owns queues, bank/row state and bus timing; DRAM cells stay outside the chip.
// 片上控制器持有队列、bank/行状态和总线时序；DRAM 存储单元位于芯片之外。
module dram_controller #(
    parameter integer BANKS=8, ENTRIES=64, ROW_SHIFT=11,
    parameter integer BANK_BITS=$clog2(BANKS), ENTRY_BITS=$clog2(ENTRIES)
) (
    input wire clk, reset,
    input wire [15:0] rcd, cas, rp, burst,
    input wire request_valid, request_write,
    input wire [63:0] request_address,
    input wire [255:0] request_data,
    input wire [31:0] request_mask,
    output wire request_ready,
    output wire [ENTRY_BITS-1:0] request_slot,
    input wire [ENTRIES-1:0] acknowledge,
    input wire [ENTRY_BITS-1:0] query_slot,
    output wire [255:0] query_data,
    output wire [ENTRIES-1:0] occupied, completed,
    output wire transfer_valid, transfer_write,
    output wire [63:0] transfer_address,
    output wire [255:0] transfer_data,
    output wire [31:0] transfer_mask,
    input wire [255:0] transfer_result,
    output wire started, started_hit,
    output wire [63:0] now,
    output reg [3:0] error
);
    localparam integer ROW_BITS=64-ROW_SHIFT-BANK_BITS;
    reg [63:0] clock_count, bus_available;
    reg [ENTRIES-1:0] allocated, finished, writes;
    reg [63:0] addresses [0:ENTRIES-1];
    reg [255:0] payloads [0:ENTRIES-1], results [0:ENTRIES-1];
    reg [31:0] masks [0:ENTRIES-1];
    reg [ENTRY_BITS-1:0] waiting [0:ENTRIES-1], active [0:ENTRIES-1];
    reg [63:0] due [0:ENTRIES-1];
    reg [ENTRY_BITS:0] waiting_count, active_count;
    reg [ENTRY_BITS-1:0] waiting_head, waiting_tail, active_head, active_tail;
    reg [BANKS-1:0] row_valid;
    reg [ROW_BITS-1:0] rows [0:BANKS-1];
    reg [63:0] bank_available [0:BANKS-1];
    reg free_found;
    reg [ENTRY_BITS-1:0] free_slot;
    integer at;

    // A slot being acknowledged may be reused by a request on the same clock edge.
    // 正在确认的槽可以在同一时钟边沿被新请求复用。
    always @* begin
        free_found=0; free_slot=0;
        for (at=0; at<ENTRIES; at=at+1)
            if (!free_found && (!allocated[at] || acknowledge[at])) begin free_found=1; free_slot=ENTRY_BITS'(at); end
    end
    wire [63:0] next_clock=clock_count+64'd1;
    wire timing_valid=rcd!=0 && cas!=0 && rp!=0 && burst!=0;
    always @* begin
        error=0;
        if (!timing_valid) error=1;
        else if ((acknowledge & ~completed)!=0) error=2;
        else if (request_valid && !free_found) error=3;
        else if (request_valid && request_address[4:0]!=0) error=4;
    end
    wire enabled=!reset && error==0;
    assign request_ready=free_found;
    assign request_slot=free_slot;
    wire accepted=request_valid && request_ready && enabled;
    assign occupied=allocated;
    assign completed=allocated & finished;
    assign query_data=results[query_slot];
    assign now=clock_count;

    // Dispatch order equals completion order because each new due time follows the bus reservation.
    // 派发顺序等于完成顺序，因为每个新完成时间都排在总线预约之后。
    wire [ENTRY_BITS-1:0] retiring=active[active_head];
    assign transfer_valid=active_count!=0 && due[retiring]<=next_clock;
    assign transfer_write=writes[retiring];
    assign transfer_address=addresses[retiring];
    assign transfer_data=payloads[retiring];
    assign transfer_mask=masks[retiring];

    // An empty queue forwards a same-clock request so a new request can start without an extra wait.
    // 空队列会转发同拍请求，使新请求无需多等一拍即可启动。
    wire front_valid=waiting_count!=0 || accepted;
    wire front_queued=waiting_count!=0;
    wire [ENTRY_BITS-1:0] front_slot=front_queued ? waiting[waiting_head] : free_slot;
    wire [63:0] front_address=front_queued ? addresses[front_slot] : request_address;
    /* verilator lint_off UNUSED */
    wire [63:0] front_line=front_address>>ROW_SHIFT;
    /* verilator lint_on UNUSED */
    wire [BANK_BITS-1:0] front_bank=front_line[BANK_BITS-1:0];
    wire [ROW_BITS-1:0] front_row=front_line[BANK_BITS+:ROW_BITS];
    wire bank_ready=bank_available[front_bank]<=next_clock;
    assign started=front_valid && bank_ready && enabled;
    assign started_hit=row_valid[front_bank] && rows[front_bank]==front_row;
    wire [63:0] penalty=started_hit ? 64'd0 : 64'(rcd)+(row_valid[front_bank] ? 64'(rp) : 64'd0);
    wire [63:0] data_ready=next_clock+64'(cas)+penalty;
    wire [63:0] finish_time=(data_ready>bus_available ? data_ready : bus_available)+64'(burst);

    always @(posedge clk) begin
        if (reset) begin
            clock_count<=0; bus_available<=0; allocated<=0; finished<=0; row_valid<=0; writes<=0;
            waiting_count<=0; active_count<=0; waiting_head<=0; waiting_tail<=0; active_head<=0; active_tail<=0;
        end else if (enabled) begin
            clock_count<=next_clock;
            allocated<=(allocated & ~acknowledge) | (accepted ? ENTRIES'(1)<<free_slot : {ENTRIES{1'b0}});
            finished<=(finished & ~acknowledge & ~(accepted ? ENTRIES'(1)<<free_slot : {ENTRIES{1'b0}}))
                | (transfer_valid ? ENTRIES'(1)<<retiring : {ENTRIES{1'b0}});
            if (accepted) begin
                writes[free_slot]<=request_write; addresses[free_slot]<=request_address;
                payloads[free_slot]<=request_data; masks[free_slot]<=request_mask;
            end
            if (accepted && (front_queued || !bank_ready)) begin
                waiting[waiting_tail]<=free_slot; waiting_tail<=waiting_tail+1'b1;
            end
            if (started && front_queued) waiting_head<=waiting_head+1'b1;
            waiting_count<=waiting_count+(ENTRY_BITS+1)'(accepted && (front_queued || !bank_ready))-(ENTRY_BITS+1)'(started && front_queued);
            if (started) begin
                active[active_tail]<=front_slot; active_tail<=active_tail+1'b1; due[front_slot]<=finish_time;
                bus_available<=finish_time; bank_available[front_bank]<=finish_time;
                rows[front_bank]<=front_row; row_valid[front_bank]<=1;
            end
            if (transfer_valid) begin
                active_head<=active_head+1'b1; results[retiring]<=transfer_result;
            end
            active_count<=active_count+(ENTRY_BITS+1)'(started)-(ENTRY_BITS+1)'(transfer_valid);
        end
    end
endmodule
