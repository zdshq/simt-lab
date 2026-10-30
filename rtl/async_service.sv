// One table owner arbitrates DMA completion, round-robin CTA release and foreground instructions.
// 单个事务表所有者仲裁 DMA 完成、轮转 CTA 回收和前台指令。
module async_service (
    input wire clk, reset,
    input wire [6:0] cta_limit,
    input wire [31:0] latency,
    input wire dma_valid, table_ready,
    input wire [63:0] dma_id,
    input wire [31:0] dma_cta,
    input wire [63:0] retire_requests,
    input wire [2047:0] retire_ctas,
    input wire accept, complete, retry, retire_ack,
    input wire [63:0] instruction_id,
    input wire [31:0] instruction_cta,
    output wire [1:0] grant_kind, accepted_kind, completed_kind, owner_kind,
    output wire [63:0] grant_id, owner_id,
    output wire [31:0] grant_cta, owner_cta, remaining,
    output wire [5:0] grant_slot, owner_slot,
    output reg [5:0] cursor,
    output wire foreground_ready, service_ready, retirement_done, busy, pending, clock_enable,
    output reg [3:0] error
);
    localparam [1:0] IDLE=0, FOREGROUND=1, DMA=2, RETIRE=3;
    reg [1:0] kind;
    reg [63:0] tag;
    reg [31:0] cta, countdown;
    reg [5:0] slot;
    reg retired;
    wire released=(complete && kind!=RETIRE) || retire_ack;
    reg found;
    reg [5:0] chosen, at;
    reg [1:0] choice;
    integer offset;
    always @* begin
        found=0; chosen=0; at=0;
        for (offset=0; offset<64; offset=offset+1) begin
            at=cursor+6'(offset);
            if (!found && {1'b0,at}<cta_limit && retire_requests[at]) begin found=1; chosen=at; end
        end
        choice=IDLE;
        if (kind==IDLE || released) begin
            if (dma_valid) choice=DMA;
            else if (found) begin if (table_ready) choice=RETIRE; end
            else choice=FOREGROUND;
        end
    end
    wire [63:0] chosen_id=choice==DMA ? dma_id : choice==FOREGROUND ? instruction_id : 64'b0;
    wire [31:0] chosen_cta=choice==DMA ? dma_cta : choice==FOREGROUND ? instruction_cta : retire_ctas[32'(chosen)*32+:32];
    always @* begin
        error=0;
        if (cta_limit==0 || cta_limit>64 || latency==0) error=1;
        else if ((retire_requests>>cta_limit)!=0) error=2;
        else if (complete && (kind==IDLE || countdown!=0 || retired)) error=3;
        else if (retry && (kind!=FOREGROUND || countdown!=0)) error=4;
        else if (retire_ack && (kind!=RETIRE || !(retired || complete))) error=5;
        else if (accept && choice==IDLE) error=6;
        else if ((complete && retry) || (retry && accept)) error=7;
    end
    wire valid=!reset && error==0;
    assign grant_kind=valid && !accept ? choice : IDLE;
    assign grant_id=chosen_id;
    assign grant_cta=chosen_cta;
    assign grant_slot=chosen;
    assign accepted_kind=valid && accept ? choice : IDLE;
    assign completed_kind=valid && complete ? kind : IDLE;
    assign owner_kind=accept && valid ? choice : released && valid ? IDLE : kind;
    assign owner_id=accept && valid ? chosen_id : released && valid ? 64'b0 : tag;
    assign owner_cta=accept && valid ? chosen_cta : released && valid ? 32'b0 : cta;
    assign owner_slot=accept && valid ? (choice==RETIRE ? chosen : 6'b0) : released && valid ? 6'b0 : slot;
    assign remaining=valid && (accept || retry) ? (choice==RETIRE && accept ? 32'b0 : latency-1'b1) :
                     valid && released ? 32'b0 : countdown;
    assign foreground_ready=grant_kind==FOREGROUND;
    assign service_ready=valid && !accept && !complete && !retry && (kind==FOREGROUND || kind==DMA) && countdown==0;
    assign retirement_done=valid && !accept && !retire_ack && kind==RETIRE && (retired || complete);
    assign busy=owner_kind!=IDLE;
    assign pending=busy || dma_valid || retire_requests!=0;
    assign clock_enable=reset || kind!=IDLE || accept || complete || retry || retire_ack;

    always @(posedge clk) begin
        if (reset) begin kind<=IDLE; tag<=0; cta<=0; slot<=0; countdown<=0; retired<=0; cursor<=0; end
        else if (error==0) begin
            if (countdown!=0) countdown<=countdown-1'b1;
            if (released) begin kind<=IDLE; tag<=0; cta<=0; slot<=0; countdown<=0; retired<=0; end
            else if (complete) retired<=1;
            if (retry) countdown<=latency-1'b1;
            if (accept) begin
                kind<=choice; tag<=chosen_id; cta<=chosen_cta; slot<=choice==RETIRE ? chosen : 6'b0;
                countdown<=choice==RETIRE ? 32'b0 : latency-1'b1; retired<=0;
                if (choice==RETIRE) cursor<=chosen+1'b1;
            end
        end
    end
endmodule
