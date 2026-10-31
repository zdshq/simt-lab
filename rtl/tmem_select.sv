// Each bank selects the oldest requesting vector and its first lane, then forms same-word read broadcast.
// 每个 bank 选择最早请求向量及其首个 lane，然后形成同字读取广播。
module tmem_select #(
    parameter integer BANK=0, BANKS=16
) (
    input wire [7:0] allocated, writes,
    input wire [63:0] older,
    input wire [255:0] remaining,
    input wire [3839:0] addresses,
    output reg found,
    output reg [2:0] selected_slot,
    output reg [4:0] selected_lane,
    output reg [31:0] selected_lanes
);
    wire [7:0] candidates;
    wire [4:0] first_lanes [0:7];
    wire [31:0] lane_masks [0:7];
    genvar request, index;
    // Resolve each request locally before comparing ages, keeping age logic out of the lane scan.
    // 先在各请求内部选择，再比较请求年龄，避免在 lane 扫描中重复年龄逻辑。
    generate for (request=0; request<8; request=request+1) begin: requests
        wire [14:0] words [0:31];
        for (index=0; index<32; index=index+1) begin: unpack
            assign words[index]=addresses[(request*32+index)*15+:15];
        end
        reg any_lane;
        reg [4:0] first;
        reg [14:0] first_word;
        reg [31:0] mask;
        integer lane;
        always @* begin
            any_lane=0; first=0; first_word=0; mask=0;
            for (lane=0; lane<32; lane=lane+1)
                if (!any_lane && allocated[request] && remaining[request*32+lane] &&
                    32'(words[lane])%BANKS==BANK) begin
                    any_lane=1; first=5'(lane); first_word=words[lane];
                end
            for (lane=0; lane<32; lane=lane+1)
                if (any_lane && remaining[request*32+lane] &&
                    (5'(lane)==first || (!writes[request] && words[lane]==first_word))) mask[lane]=1;
        end
        assign candidates[request]=any_lane;
        assign first_lanes[request]=first;
        assign lane_masks[request]=mask;
    end endgenerate
    integer slot;
    always @* begin
        found=0; selected_slot=0;
        for (slot=0; slot<8; slot=slot+1)
            if (candidates[slot] && (!found || older[slot*8+32'(selected_slot)])) begin found=1; selected_slot=3'(slot); end
        selected_lane=first_lanes[selected_slot];
        selected_lanes=found ? lane_masks[selected_slot] : 32'b0;
    end
endmodule
