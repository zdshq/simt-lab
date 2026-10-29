// One output-stationary PE reuses its real arithmetic pipeline for dot, scale, and accumulation.
// 一个输出驻留 PE 复用其真实算术流水线，执行点积、缩放和累加。
module tensor_pe (
    input wire clk, reset, initialize,
    input wire [31:0] initial_value,
    input wire integer_mode, scaled,
    input wire input_valid,
    output wire input_ready,
    input wire [31:0] a, b, scale_a, scale_b,
    input wire enabled, block_end, last,
    output reg [31:0] accumulator,
    output reg finished
);
    localparam [1:0] IDLE = 0, DOT = 1, SCALE = 2, ACCUMULATE = 3;
    reg [1:0] state;
    reg [31:0] partial, saved_scale_a, saved_scale_b, saved_value;
    reg saved_enabled, saved_end, saved_last;
    wire fp_valid, fp_ready, int_valid, int_ready;
    wire [31:0] fp_result, int_result;
    wire arithmetic_valid = integer_mode ? int_valid : fp_valid;
    wire [31:0] arithmetic_result = integer_mode ? int_result : fp_result;
    wire [31:0] dot_result = saved_enabled ? arithmetic_result : saved_value;
    wire complete = arithmetic_valid && (state == ACCUMULATE || (state == DOT && (!scaled || !saved_end)));
    wire dot_accept = input_valid && input_ready;
    wire scale_issue = state == DOT && arithmetic_valid && scaled && saved_end;
    wire accumulate_issue = state == SCALE && arithmetic_valid;
    wire [31:0] next_accumulator = complete && (!scaled || state == ACCUMULATE) ?
                                    (state == DOT ? dot_result : arithmetic_result) : accumulator;
    wire [31:0] next_partial = complete ? (state == ACCUMULATE ? 32'b0 : dot_result) : partial;
    wire [31:0] dot_initial = scaled ? next_partial : next_accumulator;
    reg [31:0] operand_a, operand_b, operand_c;

    assign input_ready = !reset && !initialize && !finished && (state == IDLE || (complete && !saved_last)) &&
                         (integer_mode ? int_ready : fp_ready);

    // Sparse holes preserve accumulator bits, including signed zero and NaN, without evaluating A*B.
    // 稀疏缺失项不计算 A*B，原样保留累加器位，包括负零和 NaN。
    always @* begin
        operand_a = enabled ? a : 32'b0; operand_b = enabled ? b : 32'b0;
        operand_c = enabled ? dot_initial : 32'b0;
        if (scale_issue) begin
            operand_a = saved_scale_a; operand_b = saved_scale_b;
            operand_c = {saved_scale_a[31]^saved_scale_b[31], 31'b0};
        end else if (accumulate_issue) begin
            operand_a = partial; operand_b = arithmetic_result; operand_c = accumulator;
        end
    end

    fp32_fma floating_pipeline (
        .clk(clk), .reset(reset || initialize), .input_valid(!integer_mode && (dot_accept || scale_issue || accumulate_issue)),
        .input_ready(fp_ready), .a(operand_a), .b(operand_b), .c(operand_c),
        .output_valid(fp_valid), .output_ready(1'b1), .result(fp_result)
    );
    int8_mac integer_pipeline (
        .clk(clk), .reset(reset || initialize), .input_valid(integer_mode && dot_accept), .input_ready(int_ready),
        .a(operand_a[7:0]), .b(operand_b[7:0]), .c(operand_c),
        .output_valid(int_valid), .output_ready(1'b1), .result(int_result)
    );

    task retire_arithmetic;
        begin
            if (state == DOT && arithmetic_valid) begin
                if (scaled) partial <= dot_result; else accumulator <= dot_result;
            end
            if (state == ACCUMULATE && arithmetic_valid) begin accumulator <= arithmetic_result; partial <= 0; end
            if (complete) begin state <= IDLE; if (saved_last) finished <= 1; end
            if (scale_issue) state <= SCALE;
            if (accumulate_issue) state <= ACCUMULATE;
        end
    endtask

    task accept_dot;
        begin
            state <= DOT; saved_enabled <= enabled; saved_end <= block_end; saved_last <= last;
            saved_scale_a <= scale_a; saved_scale_b <= scale_b; saved_value <= dot_initial;
        end
    endtask

    always @(posedge clk) begin
        if (reset || initialize) begin
            state <= IDLE; partial <= 0; accumulator <= reset ? 32'b0 : initial_value;
            finished <= 0;
        end else begin
            retire_arithmetic();
            if (dot_accept) accept_dot();
        end
    end
endmodule
