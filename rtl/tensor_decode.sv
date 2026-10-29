// Decode stored tensor operands without a host floating-point conversion.
// 解码存储的 Tensor 操作数，不调用宿主浮点转换。
module tensor_decode (
    input wire [2:0] format,
    input wire [31:0] bits,
    input wire high_nibble,
    output reg [31:0] value,
    output wire valid_format
);
    wire [3:0] nibble = high_nibble ? bits[7:4] : bits[3:0];
    assign valid_format = format <= 4;

    function automatic [31:0] canonical(input [31:0] raw);
        canonical = (&raw[30:23]) && (|raw[22:0]) ? 32'h7fc00000 : raw;
    endfunction

    // Normalize the ten-bit subnormal fraction before constructing the binary32 exponent.
    // 构造 binary32 指数前，对十位次正规尾数进行规格化。
    function automatic [31:0] half(input [15:0] raw);
        integer top, bit_at;
        reg [22:0] fraction;
        begin
            top = 0;
            for (bit_at = 0; bit_at < 10; bit_at = bit_at+1) if (raw[bit_at]) top = bit_at;
            fraction = {13'b0, raw[9:0]} << (23-top);
            if (raw[14:10] == 31) half = canonical({raw[15], 8'hff, raw[9:0], 13'b0});
            else if (raw[14:10] != 0) half = {raw[15], 8'(32'(raw[14:10])+112), raw[9:0], 13'b0};
            else if (raw[9:0] == 0) half = {raw[15], 31'b0};
            else half = {raw[15], 8'(top+103), fraction};
        end
    endfunction

    function automatic [31:0] fp4(input [3:0] raw);
        reg [30:0] magnitude;
        begin
            case (raw[2:0])
                0: magnitude = 0;
                1: magnitude = 31'h3f000000;
                2: magnitude = 31'h3f800000;
                3: magnitude = 31'h3fc00000;
                4: magnitude = 31'h40000000;
                5: magnitude = 31'h40400000;
                6: magnitude = 31'h40800000;
                7: magnitude = 31'h40c00000;
            endcase
            fp4 = {raw[3], magnitude};
        end
    endfunction

    always @* begin
        case (format)
            0: value = canonical(bits);
            1: value = half(bits[15:0]);
            2: value = canonical({bits[15:0], 16'b0});
            3: value = {{24{bits[7]}}, bits[7:0]};
            4: value = fp4(nibble);
            default: value = 0;
        endcase
    end
endmodule
