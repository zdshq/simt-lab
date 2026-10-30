// Arithmetic crosses the four FMA registers; conversion/comparison values travel in matching side registers.
// 算术经过 FMA 的四级寄存器；转换与比较值经过对齐的旁路寄存器。
module fp32_scalar_lane (
    input wire clk, reset, input_valid, active,
    input wire [7:0] opcode,
    input wire [31:0] a, b, c, alternative,
    output wire output_valid,
    output wire [31:0] result,
    output wire input_ready
);
    reg [3:0] valid, use_fma, participating;
    reg [31:0] side [0:3];
    wire [31:0] fma_result;
    wire [31:0] operand_a = active ? a : 32'b0;
    wire [31:0] operand_b = !active ? 32'b0 : opcode == 16 ? 32'h3f800000 : b;
    wire [31:0] operand_c = !active ? 32'b0 : opcode == 16 ? b : opcode == 17 ? {a[31]^b[31],31'b0} : c;
    fp32_fma arithmetic (.clk(clk), .reset(reset), .input_valid(input_valid), .input_ready(input_ready),
        .a(operand_a), .b(operand_b), .c(operand_c), .output_valid(output_valid), .output_ready(1'b1), .result(fma_result));
    assign result = !participating[3] ? 32'b0 : use_fma[3] ? fma_result : side[3];
    integer stage;
    always @(posedge clk) begin
        if (reset) begin valid <= 0; use_fma <= 0; participating <= 0; end
        else begin
            valid <= {valid[2:0],input_valid};
            if (input_valid) begin side[0] <= alternative; use_fma[0] <= opcode >= 16 && opcode <= 18; participating[0] <= active; end
            for (stage = 1; stage < 4; stage = stage+1) if (valid[stage-1]) begin
                side[stage] <= side[stage-1]; use_fma[stage] <= use_fma[stage-1]; participating[stage] <= participating[stage-1];
            end
        end
    end
endmodule
