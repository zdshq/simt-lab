// Five balanced adder levels count a 32-lane mask without a serial accumulator.
// 五层平衡加法器统计 32-lane 掩码，不使用串行累加链。
module lane_popcount(input wire [31:0] mask, output wire [5:0] count);
    genvar node;
    generate for (node = 0; node < 63; node = node+1) begin: tree
        wire [5:0] value;
        if (node >= 31) assign value = {5'b0,mask[node-31]};
        else assign value = tree[node*2+1].value + tree[node*2+2].value;
    end endgenerate
    assign count = tree[0].value;
endmodule
