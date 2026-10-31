// A full-word SRAM bank has one synchronous read/write service port and a tagged response.
// 全字 SRAM bank 具有一个同步读写服务端口和带标记的响应。
module tmem_bank #(
    parameter integer ROWS=2048, ROW_BITS=$clog2(ROWS)
) (
    input wire clk,reset,enable,request_valid,request_write,
    input wire [ROW_BITS-1:0] request_row,
    input wire [31:0] request_value,request_lanes,
    input wire [2:0] request_slot,
    output reg response_valid,
    output reg [2:0] response_slot,
    output reg [31:0] response_lanes,
    output wire [31:0] response_value
);
    reg [31:0] data [0:ROWS-1];
    reg [31:0] read_data;
    reg response_write;
    wire accepted=enable && !reset && request_valid;
    assign response_value=response_write ? 32'b0 : read_data;
    // Allocation zeroing traverses this write port; SRAM data has no reset or host import.
    // 分配清零经过此写端口；SRAM 数据没有复位或宿主导入。
    always @(posedge clk) begin
        if (accepted && request_write) data[request_row]<=request_value;
        if (accepted && !request_write) read_data<=data[request_row];
        if (reset) begin response_valid<=0; response_slot<=0; response_lanes<=0; response_write<=0; end
        else if (enable) begin
            response_valid<=accepted;
            if (accepted) begin response_slot<=request_slot; response_lanes<=request_lanes; response_write<=request_write; end
        end
    end
endmodule
