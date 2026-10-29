// One warp's per-lane writer bits and bounded reader counts retain RAW, WAW, and WAR hazards.
// 单 warp 的逐 lane 写者位和有界读者计数保留 RAW、WAW 和 WAR 冒险。
module lane_scoreboard #(
    parameter integer LANES = 32,
    parameter integer READER_BITS = 3
) (
    input wire clk,
    input wire reset,
    input wire [5:0] query_register,
    input wire [2:0] query_predicate,
    input wire [LANES-1:0] query_mask,
    output reg readable,
    output reg writable,
    output wire predicate_ready,
    input wire command_valid,
    output reg command_ready,
    output reg command_error,
    input wire [2:0] command,
    input wire [5:0] command_register,
    input wire [LANES-1:0] command_mask
);
    localparam [2:0] RESERVE_READ = 0, COLLECT_READ = 1, RESERVE_WRITE = 2,
                     RETIRE_WRITE = 3, RESERVE_PREDICATE = 4, RETIRE_PREDICATE = 5;
    reg [LANES-1:0] writers [0:63];
    reg [LANES-1:0] predicates [0:7];
    reg [READER_BITS-1:0] readers [0:64*LANES-1];
    integer query_lane, check_lane, update_lane, reset_register, reset_reader;
    wire zero_register = command_register == 63;
    wire [2:0] predicate_index = command_register[2:0];

    always @* begin
        readable = query_register == 63 || (writers[query_register] & query_mask) == 0;
        writable = readable;
        for (query_lane = 0; query_lane < LANES; query_lane = query_lane+1)
            if (query_register != 63 && query_mask[query_lane] && readers[32'(query_register)*LANES+query_lane] != 0)
                writable = 0;
    end
    assign predicate_ready = query_predicate == 7 || (predicates[query_predicate] & query_mask) == 0;

    // Invalid transitions are rejected atomically; reader saturation applies backpressure without wrapping.
    // 非法状态转换被原子拒绝；读者计数饱和时施加反压，不允许回绕。
    always @* begin
        command_ready = !reset; command_error = 0;
        case (command)
            RESERVE_READ: if (!zero_register) begin
                if ((writers[command_register] & command_mask) != 0) command_error = 1;
                for (check_lane = 0; check_lane < LANES; check_lane = check_lane+1)
                    if (command_mask[check_lane] && (&readers[32'(command_register)*LANES+check_lane])) command_ready = 0;
            end
            COLLECT_READ: if (!zero_register)
                for (check_lane = 0; check_lane < LANES; check_lane = check_lane+1)
                    if (command_mask[check_lane] && readers[32'(command_register)*LANES+check_lane] == 0) command_error = 1;
            RESERVE_WRITE: if (!zero_register && (writers[command_register] & command_mask) != 0) command_error = 1;
            RETIRE_WRITE: if (!zero_register && (writers[command_register] & command_mask) != command_mask) command_error = 1;
            RESERVE_PREDICATE: if (command_register >= 7 || (predicates[predicate_index] & command_mask) != 0) command_error = 1;
            RETIRE_PREDICATE: if (command_register >= 8 || (predicates[predicate_index] & command_mask) != command_mask) command_error = 1;
            default: command_error = 1;
        endcase
        if (!command_valid || reset) command_error = 0;
        if (command_error) command_ready = 0;
    end

    task update_readers;
        integer lane;
        begin
            for (lane = 0; lane < LANES; lane = lane+1)
                if (command_mask[lane]) begin
                    if (command == RESERVE_READ) readers[32'(command_register)*LANES+lane] <= readers[32'(command_register)*LANES+lane]+1'b1;
                    else readers[32'(command_register)*LANES+lane] <= readers[32'(command_register)*LANES+lane]-1'b1;
                end
        end
    endtask

    // Reservation checks mirror the C++ primitive; an instruction scheduler must check writable before RMW issue.
    // 预约检查对应 C++ 原语；指令调度器必须在读改写发射前检查 writable。
    task update_reservation;
        begin
            case (command)
                RESERVE_READ, COLLECT_READ: if (!zero_register) update_readers();
                RESERVE_WRITE: if (!zero_register) writers[command_register] <= writers[command_register] | command_mask;
                RETIRE_WRITE: if (!zero_register) writers[command_register] <= writers[command_register] & ~command_mask;
                RESERVE_PREDICATE: predicates[predicate_index] <= predicates[predicate_index] | command_mask;
                RETIRE_PREDICATE: predicates[predicate_index] <= predicates[predicate_index] & ~command_mask;
                default: begin end
            endcase
        end
    endtask

    always @(posedge clk) begin
        if (reset) begin
            for (reset_register = 0; reset_register < 64; reset_register = reset_register+1) writers[reset_register] <= 0;
            for (update_lane = 0; update_lane < 8; update_lane = update_lane+1) predicates[update_lane] <= 0;
            for (reset_reader = 0; reset_reader < 64*LANES; reset_reader = reset_reader+1) readers[reset_reader] <= 0;
        end else if (command_valid && command_ready) update_reservation();
    end
endmodule
