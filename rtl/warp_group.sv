// Select a runnable PC group with balanced min/max/successor trees, without changing storage.
// 用平衡的最小/最大/后继树选择可运行 PC 组，不修改存储。
module warp_group (
    input wire [1023:0] pc,
    input wire [95:0] state,
    input wire [1:0] policy,
    input wire last_valid,
    input wire [31:0] last_pc,
    output wire valid, needs_resume,
    output wire [31:0] selected_pc,
    output reg [31:0] selected_mask
);
    wire [31:0] active, yielded;
    wire [31:0] eligible = |active ? active : yielded;
    genvar lane, node;
    generate for (lane = 0; lane < 32; lane = lane+1) begin: lanes
        assign active[lane] = state[lane*3+:3] == 0;
        assign yielded[lane] = state[lane*3+:3] == 2;
    end
    for (node = 0; node < 63; node = node+1) begin: tree
        wire present, successor;
        wire [31:0] minimum, maximum, following;
        if (node >= 31) begin: leaf
            assign present = eligible[node-31];
            assign successor = eligible[node-31] && pc[(node-31)*32+:32] > last_pc;
            assign minimum = pc[(node-31)*32+:32];
            assign maximum = pc[(node-31)*32+:32];
            assign following = pc[(node-31)*32+:32];
        end else begin: branch
            localparam L = node*2+1, R = node*2+2;
            assign present = tree[L].present || tree[R].present;
            assign successor = tree[L].successor || tree[R].successor;
            assign minimum = !tree[R].present || (tree[L].present && tree[L].minimum < tree[R].minimum) ? tree[L].minimum : tree[R].minimum;
            assign maximum = !tree[R].present || (tree[L].present && tree[L].maximum > tree[R].maximum) ? tree[L].maximum : tree[R].maximum;
            assign following = !tree[R].successor || (tree[L].successor && tree[L].following < tree[R].following) ? tree[L].following : tree[R].following;
        end
    end endgenerate
    assign valid = tree[0].present;
    assign needs_resume = !(|active) && |yielded;
    assign selected_pc = !valid ? 0 : policy == 2 ? tree[0].maximum :
        policy == 0 && last_valid && tree[0].successor ? tree[0].following : tree[0].minimum;
    integer member;
    always @* begin
        selected_mask = 0;
        for (member = 0; member < 32; member = member+1)
            selected_mask[member] = valid && eligible[member] && pc[member*32+:32] == selected_pc;
    end
endmodule
