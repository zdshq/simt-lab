// Scan a finite mask from a rotating cursor; empty selection never consumes a slot.
// 从轮转游标搜索有限掩码；空选择不消费槽位。
module dma_select #(parameter integer COUNT=64) (
    input wire [COUNT-1:0] eligible,
    input wire [5:0] cursor,
    output reg valid,
    output reg [5:0] index
);
    integer offset, position;
    always @* begin
        valid=0; index=0; position=0;
        for (offset=0; offset<COUNT; offset=offset+1) begin
            position={26'b0,cursor}+offset;
            if (position>=COUNT) position=position-COUNT;
            if (!valid && eligible[position]) begin valid=1; index=6'(position); end
        end
    end
endmodule
