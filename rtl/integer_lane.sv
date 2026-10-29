// ISA v1 integer arithmetic is modulo 2^32; this is a combinational lane, not a vendor ALU replica.
// ISA v1 整数运算按 2^32 取模；这是组合 lane 电路，不是厂商 ALU 的复刻。
module integer_lane (
    input wire enable,
    input wire [7:0] opcode, mode,
    input wire [31:0] a, b, c, immediate,
    input wire [4:0] lane_id,
    input wire [31:0] warp_id, block_id, sm_id, threads, blocks,
    output reg [31:0] result,
    output reg predicate,
    output reg address_error
);
    wire [31:0] base = mode[0] ? 32'hf1000000 : 32'hf0000000;
    wire [31:0] limit = mode[0] ? 32'd16384 : 32'd65536;
    always @* begin
        result = 0; predicate = 0; address_error = 0;
        if (enable) case (opcode)
            1: result = immediate;
            2: result = a;
            3: case (immediate)
                0: result = {27'b0,lane_id};
                1: result = (warp_id << 5) + {27'b0,lane_id};
                2: result = warp_id;
                3: result = block_id;
                4: result = sm_id;
                5: result = threads;
                6: result = blocks;
                default: result = 0;
            endcase
            4: result = a+b;
            5: result = a-b;
            6: result = a*b;
            7: result = a*b+c;
            8: result = a&b;
            9: result = a|b;
            10: result = a^b;
            11: result = a << b[4:0];
            12: result = a >> b[4:0];
            13: result = $signed(a) >>> b[4:0];
            14: case (mode)
                0: predicate = a == b;
                1: predicate = a != b;
                2: predicate = a < b;
                3: predicate = a <= b;
                4: predicate = $signed(a) < $signed(b);
                5: predicate = $signed(a) <= $signed(b);
                default: predicate = 0;
            endcase
            43: begin
                result = mode[1] ? a-base : a+base;
                address_error = mode[1] ? a < base || a-base >= limit : a >= limit;
            end
            default: result = 0;
        endcase
    end
endmodule
