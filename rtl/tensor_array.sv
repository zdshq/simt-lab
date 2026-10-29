// A 4x4 physical PE mesh carries skewed A/B tokens one neighbor per clock.
// 一个物理 4x4 PE 网格将错拍的 A/B 令牌每拍传递到相邻单元。
module tensor_array (
    input wire clk, reset,
    input wire start_valid,
    output wire start_ready,
    input wire [2:0] start_rows, start_columns, start_format,
    input wire start_scaled,
    input wire [511:0] initial_values,
    input wire wave_valid,
    output wire wave_ready,
    input wire [127:0] wave_a, wave_b, wave_scale_a, wave_scale_b,
    input wire [3:0] wave_high_a, wave_high_b, wave_enabled,
    input wire wave_block_end, wave_last,
    output wire result_valid,
    input wire result_ready,
    output wire [511:0] result_values,
    output wire command_error,
    output reg transport_error,
    output reg busy,
    output wire event_wave, event_input_stall,
    output wire [15:0] event_products, event_skips, event_scales, event_accumulates
);
    reg [2:0] rows, columns, format;
    reg scaled, last_accepted;
    reg [1:0] count;
    reg read_pointer, write_pointer;
    reg [3:0] cooldown;
    // This two-entry wide FIFO is a register bank, with explicit muxed reads rather than inferred SRAM.
    // 这个两项宽 FIFO 是寄存器组，使用显式复用读取，不推断为 SRAM。
    (* mem2reg *) reg [127:0] queue_a [0:1], queue_b [0:1], queue_scale_a [0:1], queue_scale_b [0:1];
    (* mem2reg *) reg [3:0] queue_high_a [0:1], queue_high_b [0:1], queue_enabled [0:1];
    reg [1:0] queue_end, queue_last;
    reg [66:0] edge_a [0:15], link_a [0:15];
    reg [63:0] edge_b [0:15], link_b [0:15];
    reg [15:0] edge_valid_a, edge_valid_b, link_valid_a, link_valid_b;
    wire [31:0] decoded_a [0:3], decoded_b [0:3];
    wire [3:0] format_ok_a, format_ok_b;
    wire [15:0] finished, cell_ready, active_cell, incoming_valid_a, incoming_valid_b;
    wire [66:0] incoming_a [0:15];
    wire [63:0] incoming_b [0:15];
    wire valid_start = start_rows >= 1 && start_rows <= 4 && start_columns >= 1 && start_columns <= 4 &&
                       start_format <= 4 && !(start_format == 3 && start_scaled);
    wire invalid_wave = scaled && wave_last && !wave_block_end;
    wire begin_tile = start_valid && start_ready;
    wire push = wave_valid && wave_ready;
    wire inject = busy && count != 0 && cooldown == 0;
    integer lane, step, cell_at;

    assign start_ready = !reset && !busy && valid_start;
    assign wave_ready = !reset && busy && !last_accepted && count < 2 && !invalid_wave;
    assign result_valid = !reset && busy && ((finished & active_cell) == active_cell);
    assign command_error = !reset && ((start_valid && !valid_start) || (wave_valid && invalid_wave && busy));
    // These observation ports expose actual edge events without performing arithmetic in the host adapter.
    // 这些观测端口暴露实际时钟边沿事件，宿主适配器不执行算术。
    assign event_wave = !reset && inject;
    assign event_input_stall = !reset && busy && !last_accepted && count == 0 && cooldown == 0;

    generate for (genvar operand = 0; operand < 4; operand = operand+1) begin: conversion
        tensor_decode decode_a (
            .format(format), .bits(queue_a[read_pointer][operand*32+:32]), .high_nibble(queue_high_a[read_pointer][operand]),
            .value(decoded_a[operand]), .valid_format(format_ok_a[operand])
        );
        tensor_decode decode_b (
            .format(format), .bits(queue_b[read_pointer][operand*32+:32]), .high_nibble(queue_high_b[read_pointer][operand]),
            .value(decoded_b[operand]), .valid_format(format_ok_b[operand])
        );
    end endgenerate

    generate for (genvar r = 0; r < 4; r = r+1) begin: row
        for (genvar c = 0; c < 4; c = c+1) begin: column
            localparam integer CELL = r*4+c;
            assign active_cell[CELL] = r < 32'(rows) && c < 32'(columns);
            assign incoming_a[CELL] = c == 0 ? edge_a[r*4+r] : link_a[CELL-1];
            assign incoming_b[CELL] = r == 0 ? edge_b[c*4+c] : link_b[CELL-4];
            assign incoming_valid_a[CELL] = c == 0 ? edge_valid_a[r*4+r] : link_valid_a[CELL-1];
            assign incoming_valid_b[CELL] = r == 0 ? edge_valid_b[c*4+c] : link_valid_b[CELL-4];
            tensor_pe pe (
                .clk(clk), .reset(reset), .initialize(begin_tile), .initial_value(initial_values[CELL*32+:32]),
                .integer_mode(format == 3), .scaled(scaled),
                .input_valid(incoming_valid_a[CELL] && incoming_valid_b[CELL] && active_cell[CELL] && busy),
                .input_ready(cell_ready[CELL]), .a(incoming_a[CELL][31:0]), .b(incoming_b[CELL][31:0]),
                .scale_a(incoming_a[CELL][63:32]), .scale_b(incoming_b[CELL][63:32]),
                .enabled(incoming_a[CELL][64]), .block_end(incoming_a[CELL][65]), .last(incoming_a[CELL][66]),
                .accumulator(result_values[CELL*32+:32]), .finished(finished[CELL]),
                .event_product(event_products[CELL]), .event_skip(event_skips[CELL]),
                .event_scale(event_scales[CELL]), .event_accumulate(event_accumulates[CELL])
            );
        end
    end endgenerate

    task enqueue;
        begin
            queue_a[write_pointer] <= wave_a; queue_b[write_pointer] <= wave_b;
            queue_scale_a[write_pointer] <= wave_scale_a; queue_scale_b[write_pointer] <= wave_scale_b;
            queue_high_a[write_pointer] <= wave_high_a; queue_high_b[write_pointer] <= wave_high_b;
            queue_enabled[write_pointer] <= wave_enabled;
            queue_end[write_pointer] <= wave_block_end; queue_last[write_pointer] <= wave_last;
            write_pointer <= !write_pointer; last_accepted <= wave_last;
        end
    endtask

    // Separate edge delay registers make row r and column c meet at their matching PE.
    // 独立的边缘延迟寄存器使第 r 行和第 c 列在对应 PE 相遇。
    task move_edges;
        begin
            for (lane = 0; lane < 4; lane = lane+1) begin
                edge_a[lane*4] <= {queue_last[read_pointer], queue_end[read_pointer], queue_enabled[read_pointer][lane],
                                  queue_scale_a[read_pointer][lane*32+:32], decoded_a[lane]};
                edge_b[lane*4] <= {queue_scale_b[read_pointer][lane*32+:32], decoded_b[lane]};
                edge_valid_a[lane*4] <= inject; edge_valid_b[lane*4] <= inject;
                for (step = 1; step < 4; step = step+1) begin
                    edge_a[lane*4+step] <= edge_a[lane*4+step-1];
                    edge_b[lane*4+step] <= edge_b[lane*4+step-1];
                    edge_valid_a[lane*4+step] <= edge_valid_a[lane*4+step-1];
                    edge_valid_b[lane*4+step] <= edge_valid_b[lane*4+step-1];
                end
            end
        end
    endtask

    task move_links;
        begin
            for (cell_at = 0; cell_at < 16; cell_at = cell_at+1) begin
                link_a[cell_at] <= incoming_a[cell_at]; link_b[cell_at] <= incoming_b[cell_at];
                link_valid_a[cell_at] <= incoming_valid_a[cell_at]; link_valid_b[cell_at] <= incoming_valid_b[cell_at];
                if (busy && active_cell[cell_at] && ((incoming_valid_a[cell_at] != incoming_valid_b[cell_at]) ||
                    (incoming_valid_a[cell_at] && !cell_ready[cell_at]))) transport_error <= 1;
            end
            if (inject && (!( &format_ok_a) || !( &format_ok_b))) transport_error <= 1;
        end
    endtask

    task start_tile;
        begin
            busy <= 1; rows <= start_rows; columns <= start_columns; format <= start_format; scaled <= start_scaled;
            count <= 0; read_pointer <= 0; write_pointer <= 0; cooldown <= 0; last_accepted <= 0;
            edge_valid_a <= 0; edge_valid_b <= 0; link_valid_a <= 0; link_valid_b <= 0; transport_error <= 0;
        end
    endtask

    // The finite FIFO and feedback interval prevent an accumulator from being overwritten before retirement.
    // 有限 FIFO 和反馈间隔阻止在退休前覆盖累加器。
    always @(posedge clk) begin
        if (reset) begin
            busy <= 0; rows <= 0; columns <= 0; format <= 0; scaled <= 0;
            count <= 0; read_pointer <= 0; write_pointer <= 0; cooldown <= 0; last_accepted <= 0;
            edge_valid_a <= 0; edge_valid_b <= 0; link_valid_a <= 0; link_valid_b <= 0; transport_error <= 0;
        end else if (begin_tile) start_tile();
        else begin
            move_edges(); move_links();
            if (push) enqueue();
            case ({push, inject})
                2'b10: count <= count+1'b1;
                2'b01: count <= count-1'b1;
                default: begin end
            endcase
            if (cooldown != 0) cooldown <= cooldown-1'b1;
            if (inject) begin read_pointer <= !read_pointer; cooldown <= scaled && queue_end[read_pointer] ? 11 : 3; end
            if (result_valid && result_ready) busy <= 0;
        end
    end
endmodule
