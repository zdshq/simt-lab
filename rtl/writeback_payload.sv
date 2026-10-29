// Each reserved completion slot retains its result until retirement acknowledges it.
// 每个已预约完成槽保存结果，直到退休确认该结果。
module writeback_payload (
    input wire clk,
    input wire capture,
    input wire [2:0] source,
    input wire [7167:0] completion_data,
    output wire [1023:0] data
);
    reg [1023:0] stored;
    reg [1023:0] incoming;
    always @* begin
        case (source)
            0: incoming = completion_data[0+:1024];
            1: incoming = completion_data[1024+:1024];
            2: incoming = completion_data[2048+:1024];
            3: incoming = completion_data[3072+:1024];
            4: incoming = completion_data[4096+:1024];
            5: incoming = completion_data[5120+:1024];
            6: incoming = completion_data[6144+:1024];
            default: incoming = 0;
        endcase
    end
    assign data = capture ? incoming : stored;
    always @(posedge clk) if (capture) stored <= incoming;
endmodule
