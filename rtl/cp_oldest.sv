// Monotonic job handles encode age, so the smallest candidate handle is the oldest request.
// 单调递增的任务句柄表示年龄，因此句柄最小的候选就是最早的请求。
module cp_oldest (
    input wire [15:0] candidates,
    input wire [511:0] handles,
    output reg found,
    output reg [3:0] slot
);
    integer at;
    always @* begin
        found=0; slot=0;
        for (at=0; at<16; at=at+1)
            if (candidates[at] && (!found || handles[at*32+:32]<handles[32'(slot)*32+:32])) begin
                found=1; slot=4'(at);
            end
    end
endmodule
