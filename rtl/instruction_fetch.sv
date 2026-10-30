// Eight physical partition caches share one non-interleaved round-robin refill channel per SM.
// 每 SM 的八个物理分区缓存共享一个不交错的轮转补充通道。
module instruction_fetch (
    input wire clk, reset,
    input wire [3:0] partition_limit,
    input wire [32:0] program_words,
    input wire [7:0] query_valid,
    input wire [255:0] query_pcs,
    output wire [7:0] response_valid, address_errors,
    output wire [1023:0] response_words,
    output wire memory_request_valid,
    output wire [31:0] memory_request_pc,
    input wire memory_request_ready,
    input wire memory_response_valid,
    input wire [127:0] memory_response_words,
    output wire memory_response_ready,
    output wire busy,
    output wire [7:0] read_events, hit_events, miss_events, fill_events,
    output reg refill_active, request_pending,
    output reg [2:0] refill_owner, cursor,
    output reg [1:0] refill_beat,
    output wire configuration_error
);
    wire [7:0] misses, fill_ready, cache_busy;
    wire [255:0] miss_pcs;
    reg chosen_valid;
    reg [2:0] chosen, at;
    reg [31:0] refill_pc;
    integer offset;
    assign configuration_error=partition_limit==0 || partition_limit>8 || program_words==0 || program_words>33'h100000000;
    always @* begin
        chosen_valid=0; chosen=0; at=0;
        for (offset=0; offset<8; offset=offset+1) begin
            at=cursor+3'(offset);
            if (!chosen_valid && {1'b0,at}<partition_limit && misses[at]) begin chosen_valid=1; chosen=at; end
        end
    end
    wire grant=!reset && !configuration_error && !refill_active && chosen_valid;
    assign memory_request_valid=!reset && refill_active && !request_pending;
    assign memory_request_pc=refill_pc+{30'b0,refill_beat};
    assign memory_response_ready=!reset && refill_active && request_pending && fill_ready[refill_owner];
    wire request_accept=memory_request_valid && memory_request_ready;
    wire response_accept=memory_response_ready && memory_response_valid;
    assign busy=refill_active || (|cache_busy);

    always @(posedge clk) begin
        if (reset) begin refill_active<=0; request_pending<=0; refill_owner<=0; cursor<=0; refill_beat<=0; refill_pc<=0; end
        else begin
            if (grant) begin
                refill_active<=1; refill_owner<=chosen; refill_beat<=0; request_pending<=0;
                refill_pc<=miss_pcs[32'(chosen)*32+:32];
            end
            if (request_accept) request_pending<=1;
            if (response_accept) begin
                request_pending<=0; refill_beat<=refill_beat+1'b1;
                if (refill_beat==3) begin refill_active<=0; cursor<=refill_owner+1'b1; end
            end
        end
    end

    genvar part;
    generate for (part=0; part<8; part=part+1) begin: partitions
        instruction_cache cache (
            .clk(clk), .reset(reset), .program_words(program_words),
            .query_valid(query_valid[part] && !configuration_error && 4'(part)<partition_limit),
            .query_pc(query_pcs[part*32+:32]), .response_valid(response_valid[part]),
            .response_words(response_words[part*128+:128]), .address_error(address_errors[part]),
            .miss_valid(misses[part]), .miss_pc(miss_pcs[part*32+:32]), .miss_ready(grant && chosen==3'(part)),
            .fill_valid(memory_response_valid && memory_response_ready && refill_owner==3'(part)),
            .fill_words(memory_response_words), .fill_ready(fill_ready[part]), .busy(cache_busy[part]),
            .read_event(read_events[part]), .hit_event(hit_events[part]), .miss_event(miss_events[part]), .fill_event(fill_events[part])
        );
    end endgenerate
endmodule
