// One finite command port serializes arrivals and exits for a resident CTA of at most 1024 threads.
// 一个有限命令端口串行化最多 1024 线程驻留 CTA 的到达与退出。
module cta_barrier (
    input wire clk, reset, initialize,
    input wire [10:0] initial_threads,
    input wire command_valid, is_exit,
    input wire [4:0] warp,
    input wire [5:0] barrier,
    input wire [31:0] mask, requested_count,
    output reg [3:0] error,
    output wire ready, accept, clock_enable,
    output wire [15:0] release_mask, active_mask,
    output wire [175:0] counts, arrived_counts, expected_counts,
    output wire [1023:0] generations,
    output wire [16383:0] arrivals,
    output wire [1023:0] observed_live,
    output wire [10:0] observed_threads, observed_live_count,
    output wire [5:0] arrival_lanes, exit_lanes
);
    reg initialized;
    reg [10:0] threads, live_count;
    reg [1023:0] live;
    wire init_accept = initialize && !reset && initial_threads != 0 && initial_threads <= 1024;
    wire [31:0] live_word = live[{warp,5'b0}+:32];
    wire [31:0] exiting = mask & live_word;
    wire [31:0] errors;
    wire [3:0] index = barrier[3:0];
    lane_popcount count_arrivals(.mask(mask), .count(arrival_lanes));
    lane_popcount count_exits(.mask(exiting), .count(exit_lanes));
    assign ready = initialized && !reset && !initialize;
    always @* begin
        error = 0;
        if (initialize && !reset && !init_accept) error = 1;
        else if (command_valid && !reset && !initialize) begin
            if (!initialized) error = 2;
            else if (!is_exit && mask != 0 && barrier >= 16) error = 3;
            else if (!is_exit && mask != 0 && requested_count > {21'b0,threads}) error = 4;
            else if (!is_exit && mask != 0 && errors[index*2+:2] == 1) error = 5;
            else if (!is_exit && mask != 0 && errors[index*2+:2] == 2) error = 6;
            else if (!is_exit && mask != 0 && errors[index*2+:2] == 3) error = 7;
            else if (!is_exit && (mask & ~live_word) != 0) error = 8;
        end
    end
    assign accept = ready && command_valid && error == 0;
    assign clock_enable = reset || init_accept || accept;
    reg [1023:0] initial_live;
    integer lane;
    always @* begin
        initial_live = 0;
        for (lane = 0; lane < 1024; lane = lane+1) initial_live[lane] = lane < {21'b0,initial_threads};
    end
    wire [1023:0] exit_bits = {992'b0,exiting} << {warp,5'b0};
    assign observed_live = reset ? 1024'b0 : init_accept ? initial_live : accept && is_exit ? live & ~exit_bits : live;
    assign observed_threads = reset ? 11'b0 : init_accept ? initial_threads : threads;
    assign observed_live_count = reset ? 11'b0 : init_accept ? initial_threads : accept && is_exit ? live_count-{5'b0,exit_lanes} : live_count;

    genvar id;
    generate for (id = 0; id < 16; id = id+1) begin: records
        cta_barrier_record record (
            .clk(clk), .reset(reset), .clear(init_accept), .apply(accept),
            .arrival(command_valid && !is_exit && index == 4'(id)), .exit_event(is_exit),
            .warp(warp), .mask(mask), .exiting(exiting), .arrival_count(arrival_lanes),
            .requested_count(requested_count[10:0]), .live_count(live_count),
            .error(errors[id*2+:2]), .release_event(release_mask[id]), .observed_active(active_mask[id]),
            .observed_count(counts[id*11+:11]), .observed_arrived(arrived_counts[id*11+:11]),
            .observed_expected(expected_counts[id*11+:11]), .observed_generation(generations[id*64+:64]),
            .observed_members(arrivals[id*1024+:1024])
        );
    end endgenerate
    always @(posedge clk) begin
        if (reset || init_accept || accept) begin
            threads <= observed_threads; live_count <= observed_live_count; live <= observed_live;
        end
        if (reset) initialized <= 0;
        else if (init_accept) initialized <= 1;
    end
endmodule
