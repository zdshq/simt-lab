// One collector owns three captured vectors; lane masks and last-source bypass stay local to that entry.
// 一个收集器持有三个捕获向量；lane 掩码与最后源旁路保留在该条目内部。
module collector_payload (
    input wire clk,
    input wire [2:0] capture,
    input wire [5:0] banks,
    input wire [1:0] source_count,
    input wire [95:0] masks,
    input wire [4095:0] bank_data,
    output wire [3071:0] data
);
    function [31:0] select_bank(input [1:0] bank, input integer lane);
        begin
            case (bank)
                0: select_bank = bank_data[lane*32+:32];
                1: select_bank = bank_data[1024+lane*32+:32];
                2: select_bank = bank_data[2048+lane*32+:32];
                3: select_bank = bank_data[3072+lane*32+:32];
            endcase
        end
    endfunction

    genvar source, lane;
    generate for (source = 0; source < 3; source = source+1) begin: source_vector
        for (lane = 0; lane < 32; lane = lane+1) begin: lane_word
            reg [31:0] captured;
            wire [31:0] incoming = select_bank(banks[source*2+:2],lane);
            always @(posedge clk)
                if (capture[source]) captured <= incoming;
            assign data[source*1024+lane*32+:32] = source < 32'(source_count) && masks[source*32+lane] ?
                (capture[source] ? incoming : captured) : 32'b0;
        end
    end endgenerate
endmodule
