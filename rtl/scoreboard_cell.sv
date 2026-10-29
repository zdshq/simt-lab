// Per-register events follow pipeline phases; each kind has one vector event per clock.
// 逐寄存器事件遵循流水线阶段，每种事件每拍只有一个向量输入。
module scoreboard_cell #(
    parameter integer LANES = 32,
    parameter integer PORTS = 16,
    parameter integer READER_BITS = 6
) (
    input wire clk, reset,
    input wire [PORTS-1:0] selected,
    input wire [PORTS*3-1:0] commands,
    input wire [PORTS*LANES-1:0] masks,
    input wire [LANES-1:0] query_mask,
    output reg readable, writable,
    output reg [PORTS-1:0] errors, blocked
);
    reg [LANES-1:0] writers, next_writers, retired_writers;
    reg [READER_BITS-1:0] readers [0:LANES-1], next_readers [0:LANES-1], collected_readers [0:LANES-1];
    reg [LANES-1:0] event_masks [0:3];
    reg [3:0] present;
    integer positions [0:3];
    integer port, lane, kind, reset_lane, last_phase, current_phase;
    reg [2:0] operation;
    reg retire_error, collect_error, read_error, write_error, read_blocked;

    // Retire old writers, collect old readers, reserve new readers, then reserve the issuing writer.
    // 依次释放旧写者、收集旧读者、预约新读者，最后预约发射指令的写者。
    function integer phase(input [2:0] op);
        case (op)
            3: phase = 0;
            1: phase = 1;
            0: phase = 2;
            default: phase = 3;
        endcase
    endfunction

    always @* begin
        errors = 0; blocked = 0; present = 0; last_phase = -1; current_phase = 0; operation = 0;
        for (kind = 0; kind < 4; kind = kind+1) begin event_masks[kind] = 0; positions[kind] = 0; end
        for (port = 0; port < PORTS; port = port+1) begin
            operation = commands[port*3+:3]; current_phase = phase(operation);
            if (selected[port] && !reset) begin
                if (operation >= 4 || present[operation[1:0]] || current_phase < last_phase) errors[port] = 1;
                else begin
                    present[operation[1:0]] = 1; positions[operation[1:0]] = port;
                    event_masks[operation[1:0]] = masks[port*LANES+:LANES]; last_phase = current_phase;
                end
            end
        end
        retire_error = (writers & event_masks[3]) != event_masks[3];
        retired_writers = writers & ~(retire_error ? {LANES{1'b0}} : event_masks[3]);
        collect_error = 0;
        for (lane = 0; lane < LANES; lane = lane+1)
            if (event_masks[1][lane] && readers[lane] == 0) collect_error = 1;
        read_error = (retired_writers & event_masks[0]) != 0; read_blocked = 0;
        for (lane = 0; lane < LANES; lane = lane+1) begin
            collected_readers[lane] = readers[lane] - READER_BITS'(event_masks[1][lane] && !collect_error);
            if (event_masks[0][lane] && (&collected_readers[lane])) read_blocked = 1;
        end
        write_error = (retired_writers & event_masks[2]) != 0;
        next_writers = retired_writers | (write_error ? {LANES{1'b0}} : event_masks[2]);
        for (lane = 0; lane < LANES; lane = lane+1)
            next_readers[lane] = collected_readers[lane] + READER_BITS'(event_masks[0][lane] && !read_error && !read_blocked);
        if (present[3]) errors[positions[3]] = retire_error;
        if (present[1]) errors[positions[1]] = collect_error;
        if (present[0]) begin errors[positions[0]] = read_error; blocked[positions[0]] = read_blocked; end
        if (present[2]) errors[positions[2]] = write_error;
        readable = (next_writers & query_mask) == 0; writable = readable;
        for (lane = 0; lane < LANES; lane = lane+1)
            if (query_mask[lane] && next_readers[lane] != 0) writable = 0;
    end

    always @(posedge clk) begin
        writers <= reset ? 0 : next_writers;
        for (reset_lane = 0; reset_lane < LANES; reset_lane = reset_lane+1)
            readers[reset_lane] <= reset ? 0 : next_readers[reset_lane];
    end
endmodule
