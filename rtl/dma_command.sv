// One descriptor retains credits until transaction publication acknowledges its completion.
// 一条描述符保留额度，直到事务发布确认其完成。
module dma_command (
    input wire clk, reset, allocate, fragment_allocate, fragment_retire, claim, release_command,
    input wire [63:0] input_id,
    input wire [31:0] input_cta, input_source, input_destination, input_barrier, input_phase,
    input wire [12:0] input_bytes,
    input wire [7:0] fragment_bytes,
    output reg valid, claimed,
    output reg [63:0] id,
    output reg [31:0] cta, source, destination, barrier, phase,
    output reg [12:0] bytes, cursor,
    output reg [6:0] outstanding
);
    always @(posedge clk) begin
        if (reset) begin
            valid<=0; claimed<=0; id<=0; cta<=0; source<=0; destination<=0; barrier<=0; phase<=0;
            bytes<=0; cursor<=0; outstanding<=0;
        end else if (allocate) begin
            valid<=1; claimed<=0; id<=input_id; cta<=input_cta; source<=input_source;
            destination<=input_destination; barrier<=input_barrier; phase<=input_phase;
            bytes<=input_bytes; cursor<=0; outstanding<=0;
        end else begin
            if (fragment_allocate) cursor<=cursor+{5'b0,fragment_bytes};
            if (fragment_allocate || fragment_retire)
                outstanding<=outstanding+{6'b0,fragment_allocate}-{6'b0,fragment_retire};
            if (claim) claimed<=1;
            if (release_command) begin valid<=0; claimed<=0; end
        end
    end
endmodule
