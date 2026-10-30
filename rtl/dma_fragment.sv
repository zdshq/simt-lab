// A fragment owns returned payload until the shared write acknowledges; no command-wide source snapshot exists.
// 片段持有返回载荷直到 shared 写确认；不存在整条命令的源快照。
module dma_fragment (
    input wire clk, reset, allocate, read_accept, read_return, write_accept, write_return,
    input wire [5:0] input_owner,
    input wire [32:0] input_source, input_destination,
    input wire [7:0] input_bytes,
    input wire input_word,
    input wire [1023:0] returned_data,
    output reg [2:0] state,
    output reg [5:0] owner,
    output reg [32:0] source, destination,
    output reg [7:0] bytes,
    output reg word_width,
    output reg [1023:0] payload
);
    always @(posedge clk) begin
        if (reset) begin state<=0; owner<=0; source<=0; destination<=0; bytes<=0; word_width<=0; end
        else if (allocate) begin
            state<=1; owner<=input_owner; source<=input_source; destination<=input_destination;
            bytes<=input_bytes; word_width<=input_word;
        end else begin
            if (read_accept) state<=2;
            if (read_return) state<=3;
            if (write_accept) state<=4;
            if (write_return) state<=0;
        end
        if (!reset && read_return) payload<=returned_data;
    end
endmodule
