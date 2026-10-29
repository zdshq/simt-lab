// One synchronous read port and one synchronous fill port store actual sector bytes.
// 一个同步读端口和一个同步填充端口保存实际 sector 字节。
module sector_sram #(
    parameter integer WORDS = 2048,
    parameter integer INDEX_BITS = $clog2(WORDS)
) (
    input wire clk,
    input wire read_enable,
    input wire [INDEX_BITS-1:0] read_index,
    output reg [255:0] read_data,
    input wire write_enable,
    input wire [INDEX_BITS-1:0] write_index,
    input wire [255:0] write_data
);
    // Data bits have no reset; controller valid bits prevent reading uninitialized storage.
    // 数据位不复位；控制器有效位阻止读取未初始化的存储。
    reg [255:0] data [0:WORDS-1];
    always @(posedge clk) begin
        if (write_enable) data[write_index] <= write_data;
        if (read_enable) read_data <= data[read_index];
    end
endmodule
