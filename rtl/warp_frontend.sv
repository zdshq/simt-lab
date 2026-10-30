// Persistent lane storage feeds control and group selection directly; external ports serve unfinished barrier engines.
// 持久 lane 存储直接连接控制与线程组选择；外部端口服务尚未硬件化的屏障引擎。
module warp_frontend (
    input wire clk, reset, initialize,
    input wire [31:0] initial_mask,
    input wire [1:0] policy,
    input wire [63:0] write_state_mask, write_blocked_mask, write_phase_mask,
    input wire [191:0] write_state,
    input wire [2047:0] write_blocked, write_phase,
    input wire retire_advance,
    input wire [31:0] retire_pc, retire_mask,
    input wire command_valid,
    input wire [7:0] opcode,
    input wire [5:0] token,
    input wire [3:0] guard,
    input wire [31:0] group_pc, group_mask, mask, target_pc, code_size,
    input wire target_is_sync,
    input wire resume_idle, issue_valid, issue_advance,
    input wire [31:0] issue_pc, issue_mask,
    input wire [31:0] cta_block_mask,
    input wire [3:0] cta_block_id,
    input wire [15:0] cta_release_mask,
    input wire [31:0] async_wait_mask, async_wait_address, async_query_phase,
    input wire [1023:0] async_wait_phases,
    input wire async_release_valid,
    input wire [31:0] async_release_address, async_release_phase,
    output wire group_valid, needs_resume,
    output wire [31:0] selected_pc, selected_mask,
    output reg [1023:0] observed_pc, observed_blocked, observed_phase,
    output reg [95:0] observed_state,
    output wire [31:0] observed_valid_mask, observed_last_pc,
    output wire observed_last_valid,
    output wire [31:0] token_states,
    output wire [511:0] token_members, token_arrivals, token_targets,
    output wire [31:0] wake_mask,
    output reg [31:0] resumed_mask,
    output reg [31:0] cta_wake_mask,
    output reg [31:0] async_future_mask, async_block_mask, async_wake_mask,
    output wire [3:0] error,
    output wire accept, control_clock_enable, clock_enable
);
    reg [1023:0] stored_pc, stored_blocked, stored_phase;
    reg [95:0] stored_state;
    reg [31:0] stored_valid_mask, stored_last_pc;
    reg stored_last_valid;
    reg [1023:0] before_pc, before_blocked, before_phase;
    reg [95:0] before_state;
    wire [1023:0] control_pc, control_blocked;
    wire [95:0] control_state;
    reg [1023:0] after_pc, after_blocked, after_phase;
    reg [95:0] after_state;
    reg [1023:0] cta_blocked;
    reg [95:0] cta_state, scheduled_state;
    integer first_lane, second_lane, final_lane, blocking, waking;

    // Initialization forwards into first-cycle issue; pre-control writes reflect earlier asynchronous completions.
    // 初始化旁路到首拍发射；控制前写入反映较早的异步完成。
    always @* begin
        before_pc = stored_pc; before_state = stored_state;
        before_blocked = stored_blocked; before_phase = stored_phase;
        for (first_lane = 0; first_lane < 32; first_lane = first_lane+1) begin
            if (initialize || reset) begin
                before_pc[first_lane*32+:32] = 0;
                before_state[first_lane*3+:3] = !reset && initial_mask[first_lane] ? 3'd0 : 3'd5;
                before_blocked[first_lane*32+:32] = 0; before_phase[first_lane*32+:32] = 0;
            end
            if (write_state_mask[first_lane]) before_state[first_lane*3+:3] = write_state[first_lane*3+:3];
            if (write_blocked_mask[first_lane]) before_blocked[first_lane*32+:32] = write_blocked[first_lane*32+:32];
            if (write_phase_mask[first_lane]) before_phase[first_lane*32+:32] = write_phase[first_lane*32+:32];
        end
    end
    warp_control control (
        .clk(clk), .reset(reset), .clear(initialize), .command_valid(command_valid),
        .opcode(opcode), .token(token), .guard(guard), .group_pc(group_pc), .group_mask(group_mask), .mask(mask),
        .target_pc(target_pc), .code_size(code_size), .target_is_sync(target_is_sync),
        .lane_pc(before_pc), .lane_blocked(before_blocked), .lane_state(before_state),
        .next_pc(control_pc), .next_blocked(control_blocked), .next_state(control_state),
        .token_states(token_states), .token_members(token_members), .token_arrivals(token_arrivals), .token_targets(token_targets),
        .wake_mask(wake_mask), .error(error), .accept(accept), .clock_enable(control_clock_enable)
    );

    // Boundary continuations and post-control barrier wakeups precede group selection.
    // 边界指令后续 PC 与控制后的屏障唤醒先于线程组选择。
    always @* begin
        after_pc = control_pc; after_state = control_state;
        after_blocked = control_blocked; after_phase = before_phase;
        for (second_lane = 0; second_lane < 32; second_lane = second_lane+1) begin
            if (retire_advance && retire_mask[second_lane]) after_pc[second_lane*32+:32] = retire_pc+1'b1;
            if (write_state_mask[32+second_lane]) after_state[second_lane*3+:3] = write_state[96+second_lane*3+:3];
            if (write_blocked_mask[32+second_lane]) after_blocked[second_lane*32+:32] = write_blocked[1024+second_lane*32+:32];
            if (write_phase_mask[32+second_lane]) after_phase[second_lane*32+:32] = write_phase[1024+second_lane*32+:32];
        end
    end
    // A raw CTA release bus wakes matching stored waiters; the host does not calculate lane wake masks.
    // 原始 CTA 释放总线唤醒匹配的已存等待者，宿主不计算 lane 唤醒掩码。
    always @* begin
        cta_state = after_state; cta_blocked = after_blocked;
        for (blocking = 0; blocking < 32; blocking = blocking+1)
            if (cta_block_mask[blocking]) begin
                cta_state[blocking*3+:3] = 3;
                cta_blocked[blocking*32+:32] = {28'b0,cta_block_id};
            end
    end
    always @* begin
        scheduled_state = cta_state; cta_wake_mask = 0;
        for (waking = 0; waking < 32; waking = waking+1)
            if (cta_state[waking*3+:3] == 3 && cta_blocked[waking*32+:32] < 16 &&
                    cta_release_mask[cta_blocked[waking*32+:4]]) begin
                scheduled_state[waking*3+:3] = 0; cta_wake_mask[waking] = 1;
            end
    end
    reg [95:0] async_state, runnable_state;
    reg [1023:0] async_blocked, async_phase;
    integer validating, waiting, releasing;
    // Raw phase tokens are compared locally; a completion cannot wake a CTA or reconvergence waiter.
    // 本地比较原始阶段令牌；事务完成不能唤醒 CTA 或重聚等待者。
    always @* begin
        async_future_mask = 0;
        for (validating = 0; validating < 32; validating = validating+1)
            if (async_wait_mask[validating]) async_future_mask[validating] = async_wait_phases[validating*32+:32] > async_query_phase;
    end
    always @* begin
        async_state = scheduled_state; async_blocked = cta_blocked; async_phase = after_phase;
        async_block_mask = 0;
        for (waiting = 0; waiting < 32; waiting = waiting+1) if (async_wait_mask[waiting] && async_future_mask == 0) begin
            if (async_wait_phases[waiting*32+:32] == async_query_phase) begin
                async_state[waiting*3+:3] = 4; async_blocked[waiting*32+:32] = async_wait_address;
                async_phase[waiting*32+:32] = async_wait_phases[waiting*32+:32]; async_block_mask[waiting] = 1;
            end
        end
    end
    always @* begin
        runnable_state = async_state; async_wake_mask = 0;
        for (releasing = 0; releasing < 32; releasing = releasing+1)
            if (async_release_valid && async_state[releasing*3+:3] == 4 &&
                    async_blocked[releasing*32+:32] == async_release_address &&
                    async_phase[releasing*32+:32] < async_release_phase) begin
                runnable_state[releasing*3+:3] = 0; async_wake_mask[releasing] = 1;
            end
    end
    warp_group selector (
        .pc(after_pc), .state(runnable_state), .policy(policy),
        .last_valid(stored_last_valid && !initialize && !reset), .last_pc(stored_last_pc),
        .valid(group_valid), .needs_resume(needs_resume), .selected_pc(selected_pc), .selected_mask(selected_mask)
    );

    // Successful issue resumes yielded peers, updates the round-robin cursor, and advances only nonboundary PCs.
    // 成功发射恢复让出的同伴，更新轮转游标，并且只推进非边界指令 PC。
    always @* begin
        observed_pc = after_pc; observed_state = runnable_state;
        observed_blocked = async_blocked; observed_phase = async_phase; resumed_mask = 0;
        for (final_lane = 0; final_lane < 32; final_lane = final_lane+1) begin
            if ((resume_idle || issue_valid) && runnable_state[final_lane*3+:3] == 2) begin
                observed_state[final_lane*3+:3] = 0; resumed_mask[final_lane] = 1;
            end
            if (issue_valid && issue_advance && issue_mask[final_lane]) observed_pc[final_lane*32+:32] = issue_pc+1'b1;
        end
    end
    assign observed_valid_mask = reset ? 0 : initialize ? initial_mask : stored_valid_mask;
    assign observed_last_valid = !reset && (issue_valid || (!initialize && stored_last_valid));
    assign observed_last_pc = reset ? 0 : issue_valid ? issue_pc : initialize ? 0 : stored_last_pc;
    assign clock_enable = control_clock_enable || retire_advance || issue_valid || |resumed_mask ||
        |write_state_mask || |write_blocked_mask || |write_phase_mask || |cta_block_mask || |cta_wake_mask ||
        |async_block_mask || |async_wake_mask;
    always @(posedge clk) begin
        if (reset) begin
            stored_pc <= 0; stored_state <= {32{3'd5}}; stored_blocked <= 0; stored_phase <= 0;
            stored_valid_mask <= 0; stored_last_pc <= 0; stored_last_valid <= 0;
        end else if (clock_enable) begin
            stored_pc <= observed_pc; stored_state <= observed_state;
            stored_blocked <= observed_blocked; stored_phase <= observed_phase;
            stored_valid_mask <= observed_valid_mask;
            stored_last_pc <= observed_last_pc; stored_last_valid <= observed_last_valid;
        end
    end
endmodule
