// Four elastic pipeline stages implement binary32 fused multiply-add with nearest-even rounding.
// 四级可暂停流水线实现 binary32 融合乘加，使用最近偶数舍入。
module fp32_fma (
    input wire clk, reset,
    input wire input_valid,
    output wire input_ready,
    input wire [31:0] a, b, c,
    output wire output_valid,
    input wire output_ready,
    output reg [31:0] result
);
    reg [3:0] valid;
    wire advance = !valid[3] || output_ready;
    reg [47:0] product1;
    reg [23:0] addend1;
    reg signed [9:0] product_exponent1, addend_exponent1;
    reg product_sign1, addend_sign1;
    reg [76:0] product2, addend2, magnitude3;
    reg signed [9:0] exponent2, exponent3;
    reg product_sign2, addend_sign2, sign3;
    reg [32:0] special1, special2, special3;
    wire [47:0] product = significand(a[30:0])*significand(b[30:0]);
    wire signed [9:0] common_exponent = product_exponent1 > addend_exponent1 ?
                                        product_exponent1 : addend_exponent1;

    assign input_ready = !reset && advance;
    assign output_valid = !reset && valid[3];

    function automatic [23:0] significand(input [30:0] value);
        significand = {(|value[30:23]), value[22:0]};
    endfunction

    function automatic signed [9:0] exponent(input [7:0] field);
        exponent = field == 0 ? -10'sd126 : $signed({2'b00, field})-10'sd127;
    endfunction

    function automatic is_nan(input [30:0] value);
        is_nan = (&value[30:23]) && (|value[22:0]);
    endfunction

    function automatic is_infinite(input [30:0] value);
        is_infinite = value[30:0] == 31'h7f800000;
    endfunction

    // The project's numerical contract canonicalizes every NaN and retains signed infinities.
    // 项目数值约定将所有 NaN 规范化，并保留无穷大的符号。
    function automatic [32:0] exceptional(input [31:0] x, y, z);
        reg infinity_product, invalid_product, opposite_infinity;
        begin
            infinity_product = is_infinite(x[30:0]) || is_infinite(y[30:0]);
            invalid_product = (is_infinite(x[30:0]) && y[30:0] == 0) || (is_infinite(y[30:0]) && x[30:0] == 0);
            opposite_infinity = infinity_product && is_infinite(z[30:0]) && ((x[31]^y[31]) != z[31]);
            if (is_nan(x[30:0]) || is_nan(y[30:0]) || is_nan(z[30:0]) || invalid_product || opposite_infinity)
                exceptional = {1'b1, 32'h7fc00000};
            else if (infinity_product) exceptional = {1'b1, x[31]^y[31], 8'hff, 23'b0};
            else if (is_infinite(z[30:0])) exceptional = {1'b1, z};
            else exceptional = 0;
        end
    endfunction

    // Keep discarded information in a sticky bit; shifts beyond the vector width never wrap.
    // 将丢弃的信息保留为 sticky 位；超过向量宽度的移位不会回绕。
    function automatic [76:0] shift_jam(input [76:0] value, input [9:0] distance);
        reg [76:0] discarded;
        begin
            discarded = value << (10'd77-distance);
            if (distance == 0) shift_jam = value;
            else if (distance >= 77) shift_jam = {76'b0, |value};
            else shift_jam = (value >> distance) | {76'b0, |discarded};
        end
    endfunction

    function automatic integer leading_index(input [76:0] value);
        integer bit_at;
        begin
            leading_index = 0;
            for (bit_at = 0; bit_at < 77; bit_at = bit_at+1)
                if (value[bit_at]) leading_index = bit_at;
        end
    endfunction

    // Guard and sticky information select the even neighbor on an exact halfway result.
    // guard 与 sticky 信息在精确中点选择偶数邻值。
    function automatic [24:0] round_significand(input [76:0] value, input integer distance);
        reg [76:0] shifted;
        begin
            shifted = shift_jam(value, 10'(distance-2));
            if (distance < 0) round_significand = 25'(value << -distance);
            else if (distance == 0) round_significand = value[24:0];
            else if (distance == 1) round_significand = 25'(value >> 1)+25'(value[0] && value[1]);
            else round_significand = 25'(shifted >> 2)+25'(shifted[1] && (shifted[0] || shifted[2]));
        end
    endfunction

    // The fixed-point binary point is common_exponent minus 73; subnormals round at 2^-149.
    // 定点二进制小数点对应公共指数减 73；次正规数在 2^-149 位置舍入。
    function automatic [31:0] pack(input [76:0] value, input signed [9:0] scale, input sign_bit);
        integer top, unbiased, distance;
        reg [24:0] rounded;
        begin
            top = leading_index(value);
            unbiased = 32'(scale)-73+top;
            distance = unbiased < -126 ? -76-32'(scale) : top-23;
            rounded = round_significand(value, distance);
            if (unbiased >= -126 && rounded[24]) begin rounded = rounded >> 1; unbiased = unbiased+1; end
            if (value == 0) pack = {sign_bit, 31'b0};
            else if (unbiased > 127) pack = {sign_bit, 8'hff, 23'b0};
            else if (unbiased < -126) pack = {sign_bit, 7'b0, rounded[23:0]};
            else pack = {sign_bit, 8'(unbiased+127), rounded[22:0]};
        end
    endfunction

    task multiply_inputs;
        begin
            product1 <= product; addend1 <= significand(c[30:0]);
            product_exponent1 <= product == 0 ? -10'sd511 : exponent(a[30:23])+exponent(b[30:23]);
            addend_exponent1 <= c[30:0] == 0 ? -10'sd511 : exponent(c[30:23]);
            product_sign1 <= a[31]^b[31]; addend_sign1 <= c[31];
            special1 <= exceptional(a, b, c);
        end
    endtask

    // Product precision has 27 low guard positions; the addend has 50, preserving near cancellation.
    // 乘积低端保留 27 位保护位置，加数保留 50 位，以保留接近抵消时的信息。
    task align_operands;
        begin
            product2 <= shift_jam({2'b0, product1, 27'b0}, common_exponent-product_exponent1);
            addend2 <= shift_jam({3'b0, addend1, 50'b0}, common_exponent-addend_exponent1);
            exponent2 <= common_exponent;
            product_sign2 <= product_sign1; addend_sign2 <= addend_sign1;
            special2 <= special1;
        end
    endtask

    task add_magnitudes;
        begin
            if (product_sign2 == addend_sign2) begin
                magnitude3 <= product2+addend2; sign3 <= product_sign2;
            end else if (product2 >= addend2) begin
                magnitude3 <= product2-addend2; sign3 <= product2 == addend2 ? 1'b0 : product_sign2;
            end else begin magnitude3 <= addend2-product2; sign3 <= addend_sign2; end
            exponent3 <= exponent2; special3 <= special2;
        end
    endtask

    // A blocked output freezes every stage, preserving all accepted operations and the visible result.
    // 输出受阻时冻结所有级，保留已接受运算及可见结果。
    always @(posedge clk) begin
        if (reset) begin valid <= 0; result <= 0; end
        else if (advance) begin
            valid <= {valid[2:0], input_valid};
            multiply_inputs(); align_operands(); add_magnitudes();
            result <= special3[32] ? special3[31:0] : pack(magnitude3, exponent3, sign3);
        end
    end
endmodule
