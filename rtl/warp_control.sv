// Sixteen physical token records own reconvergence state for one resident warp.
// 十六个物理令牌记录持有一个驻留 warp 的重聚状态。
module warp_control (
    input wire clk, reset, clear, command_valid,
    input wire [7:0] opcode,
    input wire [5:0] token,
    input wire [3:0] guard,
    input wire [31:0] group_pc, group_mask, mask, target_pc, code_size,
    input wire target_is_sync,
    input wire [1023:0] lane_pc, lane_blocked,
    input wire [95:0] lane_state,
    output reg [1023:0] next_pc, next_blocked,
    output reg [95:0] next_state,
    output reg [31:0] token_states,
    output reg [511:0] token_members, token_arrivals, token_targets,
    output reg [31:0] wake_mask,
    output reg [3:0] error,
    output wire accept, clock_enable
);
    reg [1:0] states [0:15];
    reg [31:0] members [0:15], arrivals [0:15], targets [0:15];
    reg [1:0] updated_states [0:15];
    reg [31:0] updated_members [0:15], updated_arrivals [0:15], updated_targets [0:15];
    reg [15:0] released;
    reg [1023:0] transition_pc, transition_blocked;
    reg [95:0] transition_state;
    integer id, lane, waking, stored;
    wire token_op = opcode == 26 || opcode == 27 || (opcode == 28 && mask != 0);
    wire [3:0] index = token[3:0];
    always @* begin
        error = 0;
        if (command_valid && !reset && !clear) begin
            if (opcode < 24 || opcode > 29) error = 1;
            else if (token_op && token >= 16) error = 2;
            else if ((opcode == 26 || opcode == 27) && guard != 7) error = 3;
            else if (opcode == 26 && states[index] == 1) error = 4;
            else if (opcode == 26 && (target_pc >= code_size || !target_is_sync)) error = 5;
            else if (opcode == 27 && states[index] == 0) error = 6;
            else if (opcode == 27 && targets[index] != group_pc) error = 7;
            else if (opcode == 27 && states[index] == 1 && (group_mask & ~members[index]) != 0) error = 8;
            else if (opcode == 28 && mask != 0 && states[index] == 0) error = 9;
            else if (opcode == 28 && mask != 0 && states[index] == 1 && (mask & ~members[index]) != 0) error = 8;
            else if (opcode == 25 && mask != 0 && target_pc >= code_size) error = 10;
        end
    end
    assign accept = command_valid && !reset && !clear && error == 0;
    assign clock_enable = reset || clear || accept;

    // Decode lane transitions separately from token completion and indexed wakeup.
    // 将 lane 转换译码与令牌完成及按编号唤醒分开。
    always @* begin
        transition_pc = lane_pc; transition_state = lane_state; transition_blocked = lane_blocked;
        if (accept) begin
            for (lane = 0; lane < 32; lane = lane+1) begin
                if (group_mask[lane]) transition_pc[lane*32+:32] = group_pc+1'b1;
                if (opcode == 25 && mask[lane]) transition_pc[lane*32+:32] = target_pc;
                if (opcode == 24 && mask[lane]) transition_state[lane*3+:3] = 5;
                if (opcode == 29 && mask[lane]) transition_state[lane*3+:3] = 2;
                if (opcode == 27 && states[index] == 1 && group_mask[lane]) begin
                    transition_pc[lane*32+:32] = group_pc; transition_state[lane*3+:3] = 1;
                    transition_blocked[lane*32+:32] = {26'b0,token};
                end
            end
        end
    end

    // Produce one atomic token transaction; rejected commands expose unchanged stored state.
    // 产生一个原子的令牌事务；被拒绝的命令暴露不变的存储状态。
    always @* begin
        released = 0;
        for (id = 0; id < 16; id = id+1) begin
            updated_states[id] = reset || clear ? 2'b0 : states[id];
            updated_members[id] = reset || clear ? 32'b0 : members[id];
            updated_arrivals[id] = reset || clear ? 32'b0 : arrivals[id];
            updated_targets[id] = reset || clear ? 32'b0 : targets[id];
        end
        if (accept) begin
            if (opcode == 26) begin
                updated_states[index] = 1; updated_members[index] = group_mask;
                updated_arrivals[index] = 0; updated_targets[index] = target_pc;
            end
            if (opcode == 27 && states[index] == 1) updated_arrivals[index] = arrivals[index] | group_mask;
            for (id = 0; id < 16; id = id+1)
                if (opcode == 24 || (opcode == 28 && mask != 0 && id == 32'(index))) begin
                    updated_members[id] = members[id] & ~mask; updated_arrivals[id] = arrivals[id] & ~mask;
                end
            if (opcode == 24 || (opcode == 28 && mask != 0) || (opcode == 27 && states[index] == 1)) begin
                for (id = 0; id < 16; id = id+1) begin
                    if (updated_states[id] == 1) begin
                        if (updated_members[id] == 0) updated_states[id] = 2;
                        else if (updated_arrivals[id] == updated_members[id]) begin
                            released[id] = 1;
                            updated_states[id] = 0; updated_members[id] = 0; updated_arrivals[id] = 0;
                        end
                    end
                end
            end
        end
        for (id = 0; id < 16; id = id+1) begin
            token_states[id*2+:2] = updated_states[id]; token_members[id*32+:32] = updated_members[id];
            token_arrivals[id*32+:32] = updated_arrivals[id]; token_targets[id*32+:32] = updated_targets[id];
        end
    end
    // Each lane can wait on one token, so a completion-bit lookup replaces a priority scan.
    // 每个 lane 只能等待一个令牌，因此用完成位查询替代优先级扫描。
    always @* begin
        next_pc = transition_pc; next_state = transition_state; next_blocked = transition_blocked; wake_mask = 0;
        for (waking = 0; waking < 32; waking = waking+1)
            if (transition_state[waking*3+:3] == 1 && transition_blocked[waking*32+:32] < 16 &&
                    released[transition_blocked[waking*32+:4]]) begin
                next_pc[waking*32+:32] = updated_targets[transition_blocked[waking*32+:4]]+1'b1;
                next_state[waking*3+:3] = 0; wake_mask[waking] = 1;
            end
    end
    always @(posedge clk) begin
        for (stored = 0; stored < 16; stored = stored+1) begin
            if (reset || clear) begin states[stored] <= 0; members[stored] <= 0; arrivals[stored] <= 0; targets[stored] <= 0; end
            else if (accept) begin
                states[stored] <= updated_states[stored]; members[stored] <= updated_members[stored];
                arrivals[stored] <= updated_arrivals[stored]; targets[stored] <= updated_targets[stored];
            end
        end
    end
endmodule
