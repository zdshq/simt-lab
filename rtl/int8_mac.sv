// Signed eight-bit multiplication feeds a modulo-2^32 accumulator in four registered stages.
// 有符号八位乘法经四级寄存器送入模 2^32 累加器。
module int8_mac (
    input wire clk, reset,
    input wire input_valid,
    output wire input_ready,
    input wire signed [7:0] a, b,
    input wire [31:0] c,
    output wire output_valid,
    input wire output_ready,
    output reg [31:0] result
);
    reg [3:0] valid;
    reg signed [15:0] product1;
    reg [31:0] addend1, sum2, sum3;
    wire advance = !valid[3] || output_ready;
    assign input_ready = !reset && advance;
    assign output_valid = !reset && valid[3];

    always @(posedge clk) begin
        if (reset) begin valid <= 0; result <= 0; end
        else if (advance) begin
            valid <= {valid[2:0], input_valid};
            product1 <= a*b; addend1 <= c;
            sum2 <= {{16{product1[15]}}, product1}+addend1;
            sum3 <= sum2; result <= sum3;
        end
    end
endmodule
