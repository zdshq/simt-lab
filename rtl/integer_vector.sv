// Register identifiers route collected vectors to operands; R63 has no storage or read port.
// 寄存器编号将已收集向量路由到操作数；R63 不占存储和读取端口。
module integer_vector (
    input wire valid,
    input wire [7:0] opcode, mode,
    input wire [5:0] ra, rb, rc,
    input wire [31:0] mask, immediate, warp_id, block_id, sm_id, threads, blocks,
    input wire [1:0] source_count,
    input wire [17:0] source_registers,
    input wire [3071:0] source_data,
    output wire [1023:0] result,
    output reg [3:0] error
);
    reg [1023:0] a, b, c;
    reg found_a, found_b, found_c;
    integer source;
    wire need_a = opcode == 2 || (opcode >= 4 && opcode <= 14) || opcode == 43;
    wire need_b = opcode >= 4 && opcode <= 14;
    wire need_c = opcode == 7;
    wire [1023:0] lane_results;
    wire [31:0] predicates, address_errors;
    always @* begin
        a = 0; b = 0; c = 0; found_a = ra == 63; found_b = rb == 63; found_c = rc == 63;
        for (source = 0; source < 3; source = source+1)
            if (valid && source < 32'(source_count)) begin
                if (ra != 63 && source_registers[source*6+:6] == ra) begin a = source_data[source*1024+:1024]; found_a = 1; end
                if (rb != 63 && source_registers[source*6+:6] == rb) begin b = source_data[source*1024+:1024]; found_b = 1; end
                if (rc != 63 && source_registers[source*6+:6] == rc) begin c = source_data[source*1024+:1024]; found_c = 1; end
            end
    end
    genvar lane;
    generate for (lane = 0; lane < 32; lane = lane+1) begin: lanes
        integer_lane unit (.enable(valid && mask[lane]), .opcode(opcode), .mode(mode),
            .a(a[lane*32+:32]), .b(b[lane*32+:32]), .c(c[lane*32+:32]), .immediate(immediate),
            .lane_id(5'(lane)), .warp_id(warp_id), .block_id(block_id), .sm_id(sm_id), .threads(threads), .blocks(blocks),
            .result(lane_results[lane*32+:32]), .predicate(predicates[lane]), .address_error(address_errors[lane]));
    end endgenerate
    assign result = opcode == 14 ? {992'b0,predicates} : lane_results;

    // Reject the whole vector before capture, while matching the ISA's inactive-lane validation rules.
    // 捕获前整体拒绝错误向量，同时遵守 ISA 对非活动 lane 的检查规则。
    always @* begin
        error = 0;
        if (valid) begin
            if (opcode > 14 && opcode != 43) error = 1;
            else if (opcode == 43 && mode > 3) error = 4;
            else if (mask != 0) begin
                if (opcode == 14 && mode > 5) error = 2;
                else if (opcode == 3 && immediate > 6) error = 3;
                else if ((need_a && !found_a) || (need_b && !found_b) || (need_c && !found_c)) error = 6;
                else if (|address_errors) error = 5;
            end
        end
    end
endmodule
