// One physical bank has byte-write SRAM, synchronous reads and an exclusive integer RMW pipeline.
// 一个物理 bank 包含按字节写入的 SRAM、同步读取和独占的整数读改写流水线。
module shared_bank #(
    parameter integer ROWS=512, ROW_BITS=$clog2(ROWS)
) (
    input wire clk, reset, enable,
    input wire [ROWS-1:0] clear_rows,
    input wire request_valid, request_write, request_atomic,
    input wire [ROW_BITS-1:0] request_row,
    input wire [3:0] request_bytes, request_mode,
    input wire [31:0] request_data, request_compare, request_replacement,
    input wire [2:0] request_slot,
    input wire [31:0] request_lanes, atomic_latency,
    output wire request_ready,
    output reg response_valid,
    output reg [2:0] response_slot,
    output reg [31:0] response_lanes,
    output wire [31:0] response_data,
    output wire write_event, atomic_finish,
    output wire [ROW_BITS-1:0] write_row,
    output wire [3:0] write_bytes,
    output wire [31:0] write_data,
    output wire busy
);
    reg [ROWS-1:0] valid;
    reg atomic_busy, read_present, response_zero;
    reg [ROW_BITS-1:0] atomic_row;
    reg [2:0] atomic_slot;
    reg [3:0] atomic_mode;
    reg [31:0] atomic_lanes, operand, replacement, countdown;
    wire [31:0] read_word;
    wire [31:0] old_value=read_present ? read_word : 32'b0;
    reg [31:0] updated;
    always @* begin
        case (atomic_mode)
            0: updated=old_value+operand;
            1: updated=operand;
            2: updated=old_value==operand ? replacement : old_value;
            3: updated=old_value & operand;
            4: updated=old_value | operand;
            5: updated=old_value ^ operand;
            6: updated=old_value<operand ? old_value : operand;
            7: updated=old_value>operand ? old_value : operand;
            8: updated=$signed(old_value)<$signed(operand) ? old_value : operand;
            9: updated=$signed(old_value)>$signed(operand) ? old_value : operand;
            default: updated=old_value;
        endcase
    end
    assign request_ready=!atomic_busy;
    wire accepted=enable && !reset && request_valid && request_ready;
    assign atomic_finish=enable && !reset && atomic_busy && countdown==0;
    assign write_event=atomic_finish || (accepted && request_write && !request_atomic);
    assign write_row=atomic_finish ? atomic_row : request_row;
    assign write_bytes=atomic_finish ? 4'b1111 : request_bytes;
    assign write_data=atomic_finish ? updated : request_data;
    assign response_data=response_zero ? 32'b0 : old_value;
    assign busy=atomic_busy || response_valid;

    // Validity supplies allocation-time zeroes; data arrays have no reset or host initialization.
    // 有效位提供分配时的零值；数据阵列没有复位或宿主初始化。
    genvar byte_index;
    generate for (byte_index=0; byte_index<4; byte_index=byte_index+1) begin: bytes
        reg [7:0] data [0:ROWS-1];
        reg [7:0] read_data;
        assign read_word[byte_index*8+:8]=read_data;
        always @(posedge clk) begin
            if (write_event && (write_bytes[byte_index] || !valid[write_row]))
                data[write_row]<=write_bytes[byte_index] ? write_data[byte_index*8+:8] : 8'b0;
            if (accepted && (!request_write || request_atomic)) read_data<=data[request_row];
        end
    end endgenerate
    integer row;
    always @(posedge clk) begin
        if (reset) begin
            valid<=0; atomic_busy<=0; response_valid<=0; read_present<=0; response_zero<=0;
            response_slot<=0; response_lanes<=0; countdown<=0;
            atomic_row<=0; atomic_slot<=0; atomic_mode<=0; atomic_lanes<=0; operand<=0; replacement<=0;
        end else if (enable) begin
            response_valid<=0;
            for (row=0; row<ROWS; row=row+1) if (clear_rows[row]) valid[row]<=0;
            if (write_event) valid[write_row]<=1;
            if (atomic_busy && countdown!=0) countdown<=countdown-1'b1;
            if (atomic_finish) begin
                atomic_busy<=0; response_valid<=1; response_zero<=0;
                response_slot<=atomic_slot; response_lanes<=atomic_lanes;
            end
            if (accepted) begin
                read_present<=valid[request_row]; response_zero<=request_write && !request_atomic;
                if (request_atomic) begin
                    atomic_busy<=1; atomic_row<=request_row; atomic_slot<=request_slot; atomic_mode<=request_mode;
                    atomic_lanes<=request_lanes; operand<=request_compare; replacement<=request_replacement;
                    countdown<=atomic_latency-1'b1;
                end else begin
                    response_valid<=1; response_slot<=request_slot; response_lanes<=request_lanes;
                end
            end
        end
    end
endmodule
