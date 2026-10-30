// A balanced reduction exposes credits and physical warp ownership without a host-side scan.
// 平衡归约树给出资源额度和物理 warp 归属，不依赖宿主扫描。
module cta_resource_totals (
    input wire [63:0] live,
    input wire [2047:0] warp_masks,
    input wire [1087:0] shared_sizes,
    output wire [6:0] ctas,
    output wire [16:0] shared_bytes,
    output wire [31:0] warps
);
    genvar node;
    generate for (node=1; node<128; node=node+1) begin: nodes
        wire [6:0] count;
        wire [16:0] size;
        wire [31:0] mask;
        if (node>=64) begin: leaf
            assign count={6'b0,live[node-64]};
            assign size=live[node-64] ? shared_sizes[(node-64)*17+:17] : 17'b0;
            assign mask=live[node-64] ? warp_masks[(node-64)*32+:32] : 32'b0;
        end else begin: branch
            assign count=nodes[node*2].count+nodes[node*2+1].count;
            assign size=nodes[node*2].size+nodes[node*2+1].size;
            assign mask=nodes[node*2].mask | nodes[node*2+1].mask;
        end
    end endgenerate
    assign ctas=nodes[1].count;
    assign shared_bytes=nodes[1].size;
    assign warps=nodes[1].mask;
endmodule
