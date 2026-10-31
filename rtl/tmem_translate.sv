// Parallel CAM lookup translates each logical page without a software mapping table.
// 并行 CAM 查询转换每个逻辑页，不使用软件映射表。
module tmem_translate (
    input wire [31:0] page_live, mask,
    input wire [191:0] page_owners,
    input wire [159:0] page_logical,
    input wire [5:0] owner,
    input wire [479:0] logical_words,
    output wire [479:0] physical_words,
    output wire [31:0] missing
);
    genvar lane,page;
    generate for (lane=0; lane<32; lane=lane+1) begin: lanes
        wire [31:0] hit;
        for (page=0; page<32; page=page+1) begin: pages
            assign hit[page]=page_live[page] && page_owners[page*6+:6]==owner &&
                page_logical[page*5+:5]==logical_words[lane*15+10+:5];
        end
        reg [4:0] physical;
        integer at;
        always @* begin
            physical=0;
            for (at=0; at<32; at=at+1) if (hit[at]) physical=physical | 5'(at);
        end
        assign physical_words[lane*15+:15]={physical,logical_words[lane*15+:10]};
        assign missing[lane]=mask[lane] && !(|hit);
    end endgenerate
endmodule
