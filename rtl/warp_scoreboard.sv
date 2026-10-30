// Bounded same-cycle events implement retirement, operand collection and issue without extra clocks.
// 有界的同拍事件实现退休、操作数收集与发射，不增加额外时钟。
module warp_scoreboard #(
    parameter integer LANES = 32,
    parameter integer PORTS = 16,
    parameter integer READER_BITS = 6
) (
    input wire clk, reset,
    input wire [5:0] query_register,
    input wire [2:0] query_predicate,
    input wire [LANES-1:0] query_mask,
    output wire readable, writable, predicate_ready, clock_enable,
    input wire [17:0] issue_source_registers,
    input wire [3*LANES-1:0] issue_source_masks,
    input wire [5:0] issue_destination,
    input wire [LANES-1:0] issue_write_mask,
    input wire [2:0] issue_predicate, issue_source_predicate,
    input wire [LANES-1:0] issue_predicate_mask, issue_source_predicate_mask,
    output wire [6:0] issue_blocked,
    output wire issue_ready,
    input wire [PORTS-1:0] command_valid,
    input wire [PORTS*3-1:0] command,
    input wire [PORTS*6-1:0] command_register,
    input wire [PORTS*LANES-1:0] command_mask,
    output reg [PORTS-1:0] command_ready, command_error
);
    // Event inputs change only while clk is low; an empty event bus gates all reservation storage.
    // 事件输入只在 clk 为低时改变；空事件总线门控全部预约存储。
    assign clock_enable = reset || (|command_valid);
    wire event_clock = clk && clock_enable;
    wire [64*LANES-1:0] pending_writers, pending_readers;
    wire [8*LANES-1:0] pending_predicates;
    assign pending_writers[63*LANES+:LANES]=0;
    assign pending_readers[63*LANES+:LANES]=0;
    wire [PORTS-1:0] cell_errors [0:62], cell_blocked [0:62];
    genvar reg_index, event_index;
    generate for (reg_index = 0; reg_index < 63; reg_index = reg_index+1) begin: registers
        wire [PORTS-1:0] selected;
        for (event_index = 0; event_index < PORTS; event_index = event_index+1) begin: decode
            assign selected[event_index] = command_valid[event_index] &&
                command_register[event_index*6+:6] == 6'(reg_index) && command[event_index*3+:3] < 4;
        end
        scoreboard_cell #(.LANES(LANES), .PORTS(PORTS), .READER_BITS(READER_BITS)) state (
            .clk(event_clock), .reset(reset), .selected(selected), .commands(command), .masks(command_mask),
            .pending_writers(pending_writers[reg_index*LANES+:LANES]),
            .pending_readers(pending_readers[reg_index*LANES+:LANES]),
            .errors(cell_errors[reg_index]), .blocked(cell_blocked[reg_index])
        );
    end endgenerate

    reg [LANES-1:0] predicates [0:7], next_predicates [0:7];
    reg [PORTS-1:0] predicate_errors;
    reg [2:0] operation, predicate_index;
    reg [5:0] target;
    reg [LANES-1:0] mask;
    integer port, pred, cell_index, reset_pred;

    always @* begin
        predicate_errors = 0; operation = 0; target = 0; predicate_index = 0; mask = 0;
        for (pred = 0; pred < 8; pred = pred+1) next_predicates[pred] = predicates[pred];
        for (port = 0; port < PORTS; port = port+1) begin
            operation = command[port*3+:3]; target = command_register[port*6+:6];
            predicate_index = target[2:0]; mask = command_mask[port*LANES+:LANES];
            if (command_valid[port] && !reset) begin
                if (operation == 4) begin
                    predicate_errors[port] = target >= 7 || (next_predicates[predicate_index] & mask) != 0;
                    if (!predicate_errors[port]) next_predicates[predicate_index] = next_predicates[predicate_index] | mask;
                end else if (operation == 5) begin
                    predicate_errors[port] = target >= 8 || (next_predicates[predicate_index] & mask) != mask;
                    if (!predicate_errors[port]) next_predicates[predicate_index] = next_predicates[predicate_index] & ~mask;
                end else if (operation >= 6) predicate_errors[port] = 1;
            end
        end
    end

    always @* begin
        command_error = predicate_errors; command_ready = {PORTS{!reset}};
        for (cell_index = 0; cell_index < 63; cell_index = cell_index+1) begin
            command_error = command_error | cell_errors[cell_index];
            command_ready = command_ready & ~cell_blocked[cell_index];
        end
        command_ready = command_ready & ~command_error;
    end
    wire [LANES-1:0] query_writers=pending_writers[32'(query_register)*LANES+:LANES];
    wire [LANES-1:0] query_readers=pending_readers[32'(query_register)*LANES+:LANES];
    assign readable = (query_writers & query_mask)==0;
    assign writable = readable && (query_readers & query_mask)==0;
    assign predicate_ready = query_predicate == 7 || (next_predicates[query_predicate] & query_mask) == 0;

    genvar query_pred;
    generate for (query_pred=0; query_pred<8; query_pred=query_pred+1) begin: predicate_wires
        assign pending_predicates[query_pred*LANES+:LANES]=next_predicates[query_pred];
    end endgenerate
    dependency_query #(.LANES(LANES)) issue (
        .writers(pending_writers), .readers(pending_readers), .predicates(pending_predicates),
        .source_registers(issue_source_registers), .source_masks(issue_source_masks),
        .destination(issue_destination), .write_mask(issue_write_mask), .predicate(issue_predicate),
        .source_predicate(issue_source_predicate), .predicate_mask(issue_predicate_mask),
        .source_predicate_mask(issue_source_predicate_mask), .blocked(issue_blocked), .ready(issue_ready)
    );

    always @(posedge event_clock)
        for (reset_pred = 0; reset_pred < 8; reset_pred = reset_pred+1)
            predicates[reset_pred] <= reset ? 0 : next_predicates[reset_pred];
endmodule
