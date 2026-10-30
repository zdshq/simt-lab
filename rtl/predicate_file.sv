// Seven writable lane masks per warp use flip-flops; P7 is a constant and occupies no storage.
// 每个 warp 的七个可写 lane 掩码使用触发器；P7 为常量，不占存储。
module predicate_file #(
    parameter integer SLOTS = 8,
    parameter integer SLOT_BITS = $clog2(SLOTS)
) (
    input wire clk, reset,
    input wire [SLOTS-1:0] clear_slots,
    input wire [SLOT_BITS-1:0] query_slot,
    input wire [3:0] query_guard,
    input wire [2:0] query_source,
    input wire [31:0] query_group,
    output wire [31:0] participating, source_data,
    input wire write_valid,
    input wire [SLOT_BITS-1:0] write_slot,
    input wire [2:0] write_register,
    input wire [31:0] write_mask, write_data,
    output wire write_accept, write_error, clock_enable
);
    reg [31:0] data [0:SLOTS*7-1];
    reg [SLOTS*7-1:0] valid;
    localparam integer ROW_BITS = $clog2(SLOTS*7);
    assign write_error = !reset && write_valid && write_register == 7;
    assign write_accept = !reset && write_valid && write_register != 7 && (|write_mask);
    assign clock_enable = reset || (|clear_slots) || write_accept;

    // Clear precedes a masked write; forwarding uses the same order before the capturing edge.
    // 清零先于掩码写入；捕获边沿之前的旁路也遵循相同顺序。
    function automatic [31:0] read_value(input [2:0] predicate);
        reg [ROW_BITS-1:0] row;
        reg [31:0] value;
        begin
            row = ROW_BITS'(32'(query_slot)*7+32'(predicate));
            value = 0;
            if (predicate == 7) value = 32'hffffffff;
            else begin
                if (!reset && !clear_slots[query_slot] && valid[row]) value = data[row];
                if (write_accept && write_slot == query_slot && write_register == predicate)
                    value = (value & ~write_mask) | (write_data & write_mask);
            end
            read_value = value;
        end
    endfunction
    wire [31:0] guard_data = read_value(query_guard[2:0]);
    assign participating = reset ? 32'b0 : query_group & (query_guard[3] ? ~guard_data : guard_data);
    assign source_data = read_value(query_source);

    genvar row;
    generate for (row = 0; row < SLOTS*7; row = row+1) begin: words
        wire selected = write_accept && write_slot == SLOT_BITS'(row/7) && write_register == 3'(row%7);
        always @(posedge clk) begin
            if (reset) valid[row] <= 0;
            else begin
                if (clear_slots[row/7]) valid[row] <= 0;
                if (selected) begin
                    data[row] <= ((valid[row] && !clear_slots[row/7] ? data[row] : 32'b0) & ~write_mask) | (write_data & write_mask);
                    valid[row] <= 1;
                end
            end
        end
    end endgenerate
endmodule
