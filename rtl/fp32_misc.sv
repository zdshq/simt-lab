// Bit circuits implement signed conversions and unordered comparisons without host floating-point calls.
// 位电路实现有符号转换和无序比较，不调用宿主浮点运算。
module fp32_misc (
    input wire enable,
    input wire [7:0] opcode, mode,
    input wire [31:0] a, b,
    output reg [31:0] result,
    output reg [1:0] error
);
    function automatic [31:0] integer_to_float(input [31:0] value);
        reg [31:0] magnitude, discarded;
        reg [24:0] rounded;
        integer top, bit_at, distance, exponent;
        begin
            magnitude = value[31] ? 32'b0-value : value;
            top = 0;
            for (bit_at = 0; bit_at < 32; bit_at = bit_at+1) if (magnitude[bit_at]) top = bit_at;
            exponent = top+127; distance = top-23; discarded = 0;
            if (distance <= 0) rounded = 25'(magnitude << -distance);
            else begin
                discarded = magnitude & ((32'b1 << (distance-1))-1'b1);
                rounded = 25'(magnitude >> distance) + 25'(magnitude[distance-1] && ((|discarded) || magnitude[distance]));
            end
            if (rounded[24]) begin rounded = rounded >> 1; exponent = exponent+1; end
            integer_to_float = magnitude == 0 ? 32'b0 : {value[31],8'(exponent),rounded[22:0]};
        end
    endfunction

    // Float-to-int truncates toward zero; only the exact negative 2^31 endpoint is legal at exponent 158.
    // 浮点转整数向零截断；指数域为 158 时，仅精确的负 2^31 端点合法。
    function automatic [32:0] float_to_integer(input [31:0] value);
        reg [31:0] magnitude;
        reg invalid;
        begin
            magnitude = 0;
            invalid = value[30:23] > 158 || (value[30:23] == 158 && (!value[31] || (|value[22:0])));
            if (!invalid && value[30:23] >= 127) begin
                if (value[30:23] >= 150) magnitude = {8'b0,1'b1,value[22:0]} << (value[30:23]-8'd150);
                else magnitude = {8'b0,1'b1,value[22:0]} >> (8'd150-value[30:23]);
            end
            float_to_integer = {invalid,value[31] ? 32'b0-magnitude : magnitude};
        end
    endfunction

    function automatic compare(input [31:0] x, y, input [7:0] comparison);
        reg unordered, equal_value, less_value;
        begin
            unordered = ((&x[30:23]) && (|x[22:0])) || ((&y[30:23]) && (|y[22:0]));
            equal_value = x == y || (x[30:0] == 0 && y[30:0] == 0);
            less_value = !equal_value && (x[31] != y[31] ? x[31] : x[31] ? x[30:0] > y[30:0] : x[30:0] < y[30:0]);
            case (comparison)
                0: compare = !unordered && equal_value;
                1: compare = unordered || !equal_value;
                2: compare = !unordered && less_value;
                3: compare = !unordered && (less_value || equal_value);
                default: compare = 0;
            endcase
        end
    endfunction

    reg [32:0] converted;
    always @* begin
        result = 0; error = 0; converted = 0;
        if (enable) case (opcode)
            15: begin result = {31'b0,compare(a,b,mode)}; if (mode > 3) error = 1; end
            22: result = integer_to_float(a);
            23: begin converted = float_to_integer(a); result = converted[31:0]; if (converted[32]) error = 2; end
            default: result = 0;
        endcase
    end
endmodule
