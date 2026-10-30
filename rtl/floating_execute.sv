// Reserve finite result slots before arithmetic, so four physical pipeline stages always have a destination.
// 算术开始前预约有限结果槽，使四级物理流水线始终有结果去处。
module floating_execute #(
    parameter integer ENTRIES = 32,
    parameter integer INDEX_BITS = $clog2(ENTRIES)
) (
    input wire clk, reset, input_valid,
    output wire input_ready, input_accept,
    output reg [3:0] input_error,
    input wire [4:0] input_entry,
    input wire [31:0] latency,
    input wire [7:0] opcode, mode,
    input wire [5:0] ra, rb, rc,
    input wire [31:0] mask,
    input wire [1:0] source_count,
    input wire [17:0] source_registers,
    input wire [3071:0] source_data,
    output wire output_valid,
    input wire output_ready,
    output wire [4:0] output_entry,
    output wire [1023:0] output_data,
    output wire [5:0] occupancy,
    output wire clock_enable, protocol_error
);
    reg [1023:0] a, b, c;
    reg found_a, found_b, found_c;
    integer source;
    wire need_b = opcode >= 15 && opcode <= 18;
    wire need_c = opcode == 18;
    always @* begin
        a = 0; b = 0; c = 0; found_a = ra == 63; found_b = rb == 63; found_c = rc == 63;
        for (source = 0; source < 3; source = source+1)
            if (input_valid && source < 32'(source_count)) begin
                if (ra != 63 && source_registers[source*6+:6] == ra) begin a = source_data[source*1024+:1024]; found_a = 1; end
                if (rb != 63 && source_registers[source*6+:6] == rb) begin b = source_data[source*1024+:1024]; found_b = 1; end
                if (rc != 63 && source_registers[source*6+:6] == rc) begin c = source_data[source*1024+:1024]; found_c = 1; end
            end
    end

    reg [ENTRIES-1:0] occupied, finished;
    reg [INDEX_BITS-1:0] head, tail;
    reg [5:0] count;
    reg [31:0] remaining [0:ENTRIES-1];
    reg [4:0] identity [0:ENTRIES-1];
    reg [1023:0] payload [0:ENTRIES-1];
    reg [3:0] pipeline_valid, predicate_kind;
    reg [INDEX_BITS-1:0] pipeline_index [0:3];
    wire [INDEX_BITS-1:0] result_index = pipeline_index[3];
    wire [31:0] lane_valid, lane_ready, compare_errors, conversion_errors;
    wire [1023:0] lane_results, calculated;
    wire [31:0] predicates;
    wire arriving = pipeline_valid[3];
    wire forwarding = arriving && result_index == head;
    wire pop = output_valid && output_ready;
    reg counting;
    integer check_entry, update_entry, stage;
    assign input_ready = !reset && (&lane_ready) && (32'(count) < ENTRIES || pop);
    assign input_accept = input_valid && input_ready && input_error == 0;
    assign output_valid = !reset && count != 0 && remaining[head] == 0 && (finished[head] || forwarding);
    assign output_entry = output_valid ? identity[head] : 5'b0;
    assign output_data = !output_valid ? 1024'b0 : forwarding ? calculated : payload[head];
    assign occupancy = count;
    assign clock_enable = reset || input_accept || pop || counting || (|pipeline_valid);
    assign protocol_error = !reset && ((lane_valid != {32{arriving}}) ||
        (arriving && (!occupied[result_index] || finished[result_index])));
    assign calculated = predicate_kind[3] ? {992'b0,predicates} : lane_results;
    genvar lane;
    generate for (lane = 0; lane < 32; lane = lane+1) begin: lanes
        wire [31:0] alternative;
        wire [1:0] error;
        fp32_misc misc (.enable(input_valid && !reset && mask[lane]), .opcode(opcode), .mode(mode),
            .a(a[lane*32+:32]), .b(b[lane*32+:32]), .result(alternative), .error(error));
        assign compare_errors[lane] = error == 1;
        assign conversion_errors[lane] = error == 2;
        fp32_scalar_lane arithmetic (.clk(clk), .reset(reset), .input_valid(input_accept), .active(mask[lane]),
            .opcode(opcode), .a(a[lane*32+:32]), .b(b[lane*32+:32]), .c(c[lane*32+:32]), .alternative(alternative),
            .output_valid(lane_valid[lane]), .input_ready(lane_ready[lane]), .result(lane_results[lane*32+:32]));
        assign predicates[lane] = lane_results[lane*32];
    end endgenerate
    always @* begin
        input_error = 0; counting = 0;
        for (check_entry = 0; check_entry < ENTRIES; check_entry = check_entry+1)
            if (occupied[check_entry] && remaining[check_entry] != 0) counting = 1;
        if (input_valid && !reset) begin
            if (!((opcode >= 15 && opcode <= 18) || opcode == 22 || opcode == 23)) input_error = 1;
            else if (|compare_errors) input_error = 2;
            else if (|conversion_errors) input_error = 3;
            else if (mask != 0 && (!found_a || (need_b && !found_b) || (need_c && !found_c))) input_error = 4;
            else if (latency < 4) input_error = 5;
        end
    end

    // Physical arithmetic advances independently of output stalls because each in-flight operation owns a slot.
    // 每项在途运算持有自己的槽，物理算术因此可以独立于输出反压继续推进。
    always @(posedge clk) begin
        if (reset) begin occupied <= 0; finished <= 0; head <= 0; tail <= 0; count <= 0; pipeline_valid <= 0; predicate_kind <= 0; end
        else begin
            pipeline_valid <= {pipeline_valid[2:0],input_accept};
            if (input_accept) begin pipeline_index[0] <= tail; predicate_kind[0] <= opcode == 15; end
            for (stage = 1; stage < 4; stage = stage+1) if (pipeline_valid[stage-1]) begin
                pipeline_index[stage] <= pipeline_index[stage-1]; predicate_kind[stage] <= predicate_kind[stage-1];
            end
            for (update_entry = 0; update_entry < ENTRIES; update_entry = update_entry+1)
                if (occupied[update_entry] && remaining[update_entry] != 0) remaining[update_entry] <= remaining[update_entry]-1'b1;
            if (arriving) begin payload[result_index] <= calculated; finished[result_index] <= 1; end
            if (pop) begin occupied[head] <= 0; finished[head] <= 0; head <= head+1'b1; end
            if (input_accept) begin
                occupied[tail] <= 1; finished[tail] <= 0; identity[tail] <= input_entry; remaining[tail] <= latency-1'b1; tail <= tail+1'b1;
            end
            case ({input_accept,pop})
                2'b10: count <= count+1'b1;
                2'b01: count <= count-1'b1;
                default: count <= count;
            endcase
        end
    end
endmodule
