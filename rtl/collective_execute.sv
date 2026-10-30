// Cross-lane muxes and predicate reductions feed one elastic result register.
// 跨 lane 多路器和谓词归约连接一个可反压的结果寄存器。
module collective_execute (
    input wire clk, reset, input_valid,
    output wire input_ready, input_accept,
    output reg [3:0] input_error,
    input wire [4:0] input_entry,
    input wire [7:0] opcode, mode,
    input wire [5:0] ra, rb,
    input wire [31:0] mask, members, group_mask, predicates,
    input wire [1:0] source_count,
    input wire [17:0] source_registers,
    input wire [3071:0] source_data,
    output wire output_valid,
    input wire output_ready,
    output reg [4:0] output_entry,
    output reg [1023:0] output_data,
    output wire clock_enable
);
    reg [1023:0] a, b, calculated;
    reg found_a, found_b, bad_index, occupied;
    reg [31:0] reduction, leader;
    wire [31:0] participating = mask == 0 ? 32'b0 : members;
    wire [31:0] ballot = predicates & participating;
    integer source, lane;
    always @* begin
        a = 0; b = 0; found_a = ra == 63; found_b = rb == 63;
        for (source = 0; source < 3; source = source+1)
            if (source < 32'(source_count)) begin
                if (ra != 63 && source_registers[source*6+:6] == ra) begin a = source_data[source*1024+:1024]; found_a = 1; end
                if (rb != 63 && source_registers[source*6+:6] == rb) begin b = source_data[source*1024+:1024]; found_b = 1; end
            end
        reduction = ballot;
        case (mode)
            1: reduction = {31'b0,(|ballot)};
            2: reduction = {31'b0,ballot == participating};
            3: reduction = {31'b0,ballot == 0 || ballot == participating};
            default: reduction = ballot;
        endcase
        leader = participating & (~participating+32'd1);
        calculated = 0; bad_index = 0;
        for (lane = 0; lane < 32; lane = lane+1) begin
            if (opcode == 33 && mask[lane]) calculated[lane*32+:32] = group_mask;
            if (participating[lane]) begin
                if (opcode == 31) calculated[lane*32+:32] = reduction;
                if (opcode == 32) begin
                    calculated[lane*32+:32] = a[b[lane*32+:5]*32+:32];
                    if (b[lane*32+5+:27] != 0 || !participating[b[lane*32+:5]]) bad_index = 1;
                end
            end
        end
        if (opcode == 30) calculated = {992'b0,leader};
        input_error = 0;
        if (input_valid && !reset) begin
            if (opcode < 30 || opcode > 33) input_error = 1;
            else if (mask != 0 && opcode != 33) begin
                if (members == 0) input_error = 2;
                else if ((members & ~mask) != 0) input_error = 3;
                else if (opcode == 31 && ra >= 8) input_error = 4;
                else if (opcode == 31 && mode > 3) input_error = 5;
                else if (opcode == 32 && (!found_a || !found_b)) input_error = 7;
                else if (opcode == 32 && bad_index) input_error = 6;
            end
        end
    end
    assign input_ready = !reset && (!occupied || output_ready);
    assign input_accept = input_valid && input_ready && input_error == 0;
    assign output_valid = !reset && occupied;
    assign clock_enable = reset || input_accept || (output_valid && output_ready);
    // A rejected descriptor cannot consume either its collector or the older pending result.
    // 被拒绝的描述符不能消耗自己的收集器，也不能破坏较早的待完成结果。
    always @(posedge clk) begin
        if (reset) begin occupied <= 0; output_entry <= 0; output_data <= 0; end
        else begin
            if (output_valid && output_ready) occupied <= 0;
            if (input_accept) begin occupied <= 1; output_entry <= input_entry; output_data <= calculated; end
        end
    end
endmodule
