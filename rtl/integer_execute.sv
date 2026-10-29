// A bounded result FIFO and hardware countdowns model integer execution latency and completion backpressure.
// 有界结果 FIFO 和硬件倒计数实现整数执行延迟与完成端反压。
module integer_execute #(
    parameter integer ENTRIES = 32,
    parameter integer INDEX_BITS = $clog2(ENTRIES)
) (
    input wire clk, reset,
    input wire input_valid,
    output wire input_ready, input_accept,
    output wire [3:0] input_error,
    input wire [4:0] input_entry,
    input wire [31:0] latency,
    input wire [7:0] opcode, mode,
    input wire [5:0] ra, rb, rc,
    input wire [31:0] mask, immediate, warp_id, block_id, sm_id, threads, blocks,
    input wire [1:0] source_count,
    input wire [17:0] source_registers,
    input wire [3071:0] source_data,
    output wire output_valid,
    input wire output_ready,
    output wire [4:0] output_entry,
    output wire [1023:0] output_data,
    output wire [5:0] occupancy,
    output wire clock_enable
);
    wire [1023:0] calculated;
    wire [3:0] arithmetic_error;
    reg [ENTRIES-1:0] occupied;
    reg [INDEX_BITS-1:0] head, tail;
    reg [5:0] count;
    reg [31:0] remaining [0:ENTRIES-1];
    reg [4:0] identity [0:ENTRIES-1];
    reg [1023:0] payload [0:ENTRIES-1];
    reg counting;
    integer check_entry, update_entry;
    wire pop = output_valid && output_ready;
    assign input_error = input_valid && latency == 0 ? 4'd7 : arithmetic_error;
    assign input_ready = !reset && (32'(count) < ENTRIES || pop);
    assign input_accept = input_valid && input_ready && input_error == 0;
    assign output_valid = !reset && count != 0 && remaining[head] == 0;
    assign output_entry = output_valid ? identity[head] : 5'b0;
    assign output_data = output_valid ? payload[head] : 1024'b0;
    assign occupancy = count;
    assign clock_enable = reset || input_accept || pop || counting;
    integer_vector arithmetic (.valid(input_valid && !reset), .opcode(opcode), .mode(mode), .ra(ra), .rb(rb), .rc(rc),
        .mask(mask), .immediate(immediate), .warp_id(warp_id), .block_id(block_id), .sm_id(sm_id), .threads(threads), .blocks(blocks),
        .source_count(source_count), .source_registers(source_registers), .source_data(source_data), .result(calculated), .error(arithmetic_error));
    always @* begin
        counting = 0;
        for (check_entry = 0; check_entry < ENTRIES; check_entry = check_entry+1)
            if (occupied[check_entry] && remaining[check_entry] != 0) counting = 1;
    end

    // Store arithmetic data once; neither downstream stalls nor later input changes can recompute it.
    // 算术数据只保存一次；下游等待或后续输入变化都不能重新计算已有结果。
    always @(posedge clk) begin
        if (reset) begin occupied <= 0; head <= 0; tail <= 0; count <= 0; end
        else begin
            for (update_entry = 0; update_entry < ENTRIES; update_entry = update_entry+1)
                if (occupied[update_entry] && remaining[update_entry] != 0) remaining[update_entry] <= remaining[update_entry]-1'b1;
            if (pop) begin occupied[head] <= 0; head <= head+1'b1; end
            if (input_accept) begin
                occupied[tail] <= 1; tail <= tail+1'b1; remaining[tail] <= latency-1'b1;
                identity[tail] <= input_entry; payload[tail] <= calculated;
            end
            case ({input_accept,pop})
                2'b10: count <= count+1'b1;
                2'b01: count <= count-1'b1;
                default: count <= count;
            endcase
        end
    end
endmodule
