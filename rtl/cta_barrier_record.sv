// One barrier owns exact thread arrivals, cached counts and a generation counter.
// 一个屏障持有精确线程到达记录、缓存计数和阶段计数器。
module cta_barrier_record (
    input wire clk, reset, clear, apply, arrival, exit_event,
    input wire [4:0] warp,
    input wire [31:0] mask, exiting,
    input wire [5:0] arrival_count,
    input wire [10:0] requested_count, live_count,
    output reg [1:0] error,
    output reg release_event,
    output reg observed_active,
    output reg [10:0] observed_count, observed_arrived, observed_expected,
    output reg [63:0] observed_generation,
    output reg [1023:0] observed_members
);
    reg active;
    reg [10:0] fixed_count, arrived_count, expected_count;
    reg [63:0] generation;
    reg [1023:0] members;
    wire [31:0] word_arrived = members[{warp,5'b0}+:32];
    wire [5:0] missing_exits;
    lane_popcount count_missing(.mask(exiting & ~word_arrived), .count(missing_exits));
    wire [10:0] target = active ? expected_count : requested_count != 0 ? requested_count : live_count;
    wire [11:0] arrivals = {1'b0,active ? arrived_count : 11'b0} + {6'b0,arrival_count};

    // Validate before applying anything, so a rejected mask cannot partially mark arrivals.
    // 在应用任何更新之前验证，防止被拒绝的掩码部分写入到达位。
    always @* begin
        error = 0;
        if (arrival && mask != 0 && !reset && !clear) begin
            if (active && fixed_count != requested_count) error = 1;
            else if (active && (word_arrived & mask) != 0) error = 2;
            else if (arrivals > {1'b0,target}) error = 3;
        end
    end
    always @* begin
        observed_active = active; observed_count = fixed_count; observed_arrived = arrived_count;
        observed_expected = expected_count; observed_generation = generation; observed_members = members;
        release_event = 0;
        if (reset || clear) begin
            observed_active = 0; observed_count = 0; observed_arrived = 0;
            observed_expected = 0; observed_generation = 0; observed_members = 0;
        end else if (apply) begin
            if (arrival && mask != 0) begin
                observed_active = 1; observed_count = requested_count;
                observed_expected = target; observed_arrived = arrivals[10:0];
                observed_members[{warp,5'b0}+:32] = word_arrived | mask;
            end
            if (exit_event && active && fixed_count == 0)
                observed_expected = expected_count - {5'b0,missing_exits};
            if (observed_active && observed_arrived == observed_expected) begin
                release_event = 1; observed_active = 0; observed_arrived = 0; observed_members = 0;
                observed_generation = generation+1'b1;
            end
        end
    end
    always @(posedge clk) begin
        if (reset || clear || apply) begin
            active <= observed_active; fixed_count <= observed_count; arrived_count <= observed_arrived;
            expected_count <= observed_expected; generation <= observed_generation; members <= observed_members;
        end
    end
endmodule
