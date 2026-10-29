// Each bank stores 32 lane words per row; validity implements zero-on-allocation without resetting data.
// 每个 bank 的每行保存 32 个 lane 字；有效位实现分配清零，不复位数据阵列。
module register_bank #(
    parameter integer SLOTS = 8,
    parameter integer SLOT_BITS = $clog2(SLOTS),
    parameter integer ROW_BITS = SLOT_BITS+4
) (
    input wire clk, reset,
    input wire [SLOTS-1:0] clear_slots,
    input wire read_valid,
    input wire [ROW_BITS-1:0] read_row,
    output wire [1023:0] read_data,
    input wire write_valid,
    input wire [ROW_BITS-1:0] write_row,
    input wire [31:0] write_mask,
    input wire [1023:0] write_data
);
    genvar lane;
    generate for (lane = 0; lane < 32; lane = lane+1) begin: lanes
        reg [31:0] data [0:SLOTS*16-1];
        reg [SLOTS*16-1:0] valid;
        integer row;
        wire stored_valid = valid[read_row] && !clear_slots[read_row[ROW_BITS-1:4]];
        wire bypass = write_valid && write_row == read_row && write_mask[lane];
        assign read_data[lane*32+:32] = !read_valid || reset ? 32'b0 :
            bypass ? write_data[lane*32+:32] : stored_valid ? data[read_row] : 32'b0;

        // One masked write port per lane, with no asynchronous data reset or host initialization.
        // 每 lane 一个掩码写端口，没有数据异步复位或宿主初始化。
        always @(posedge clk) begin
            if (write_valid && write_mask[lane] && !reset) data[write_row] <= write_data[lane*32+:32];
            for (row = 0; row < SLOTS*16; row = row+1)
                if (reset || clear_slots[row/16]) valid[row] <= 0;
            if (write_valid && write_mask[lane] && !reset) valid[write_row] <= 1;
        end
    end endgenerate
endmodule
