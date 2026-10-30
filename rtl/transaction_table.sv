// A synchronous record RAM and bounded serial search own all transaction state for one SM.
// 同步记录 RAM 与有界串行搜索持有一个 SM 的全部事务状态。
module transaction_table #(parameter integer SLOTS = 1024) (
    input wire clk, reset,
    input wire command_valid,
    output wire command_ready,
    input wire [2:0] operation,
    input wire [10:0] capacity,
    input wire [31:0] cta, address, amount, completion_phase,
    output wire publication_valid,
    input wire publication_ack,
    output wire response_valid,
    input wire response_ready,
    output reg [3:0] error,
    output reg [31:0] result_cta, result_address, arrival_token,
    output reg [159:0] result_record,
    output reg phase_advanced,
    output wire [2:0] result_operation,
    output reg [10:0] occupancy,
    output wire clock_enable,
    output wire [2:0] observed_state
);
    localparam integer INDEX = $clog2(SLOTS);
    localparam [2:0] IDLE=0, LOAD=1, CHECK=2, PUBLISH=3, RESPONSE=4;
    reg [2:0] state, action;
    reg [10:0] limit;
    reg [31:0] requested_amount, requested_phase;
    reg [INDEX-1:0] cursor, free_slot, target;
    reg free_found, clearing;
    reg [SLOTS-1:0] valid;
    reg [223:0] records [0:SLOTS-1];
    reg [223:0] fetched;
    reg fetched_valid;
    wire last = 11'(cursor)+11'd1 == limit;
    wire owner_match = fetched_valid && fetched[223:192] == result_cta;
    wire record_match = owner_match && fetched[191:160] == result_address;
    wire overlaps = owner_match && {1'b0,fetched[191:160]} < {1'b0,result_address}+33'd32 &&
        {1'b0,result_address} < {1'b0,fetched[191:160]}+33'd32;
    wire [159:0] calculated;
    wire [3:0] calculation_error;
    wire calculation_advanced;
    transaction_update arithmetic(.operation(action), .previous(fetched[159:0]), .amount(requested_amount),
        .completion_phase(requested_phase), .updated(calculated), .error(calculation_error), .advanced(calculation_advanced));
    assign command_ready = state == IDLE && !reset;
    assign publication_valid = state == PUBLISH && !reset;
    assign response_valid = state == RESPONSE && !reset;
    assign result_operation = action;
    assign clock_enable = reset || state != IDLE || command_valid;
    assign observed_state = state;

    // Data RAM has one synchronous read port and one publication write port; validity alone resets.
    // 数据 RAM 有一个同步读端口与一个发布写端口；复位只清除有效位。
    always @(posedge clk) begin
        if (!reset && state == LOAD) fetched <= records[cursor];
        if (!reset && state == PUBLISH && publication_ack)
            records[target] <= {result_cta,result_address,result_record};
    end

    always @(posedge clk) begin
        if (reset) begin
            state <= IDLE; action <= 0; limit <= 0; requested_amount <= 0; requested_phase <= 0;
            cursor <= 0; free_slot <= 0; target <= 0; free_found <= 0; clearing <= 0; valid <= 0;
            fetched_valid <= 0; error <= 0; occupancy <= 0; phase_advanced <= 0;
            result_cta <= 0; result_address <= 0; arrival_token <= 0; result_record <= 0;
        end else case (state)
            IDLE: if (command_valid) begin
                action <= operation; limit <= capacity; result_cta <= cta; result_address <= address;
                requested_amount <= amount; requested_phase <= completion_phase;
                cursor <= 0; free_found <= 0; clearing <= 0; phase_advanced <= 0;
                arrival_token <= 0; result_record <= 0; error <= 0; state <= LOAD;
                if (operation > 6) begin error <= 1; state <= RESPONSE; end
                else if (capacity == 0 || capacity > 11'(SLOTS)) begin error <= 2; state <= RESPONSE; end
                else if (operation == 0 && (amount == 0 || amount > 1024)) begin error <= 3; state <= RESPONSE; end
                else if (operation == 0 && (address[3:0] != 0 || address > 65504)) begin error <= 15; state <= RESPONSE; end
            end
            LOAD: begin fetched_valid <= valid[cursor]; state <= CHECK; end
            CHECK: begin
                if (action == 6) begin
                    if (!clearing && owner_match && fetched[63:0] != 0) begin error <= 14; state <= RESPONSE; end
                    else begin
                        if (clearing && owner_match) begin valid[cursor] <= 0; occupancy <= occupancy-1'b1; end
                        if (!last) begin cursor <= cursor+1'b1; state <= LOAD; end
                        else if (!clearing) begin clearing <= 1; cursor <= 0; state <= LOAD; end
                        else state <= RESPONSE;
                    end
                end else if (action == 0) begin
                    if (overlaps) begin error <= 4; state <= RESPONSE; end
                    else begin
                        if (!fetched_valid && !free_found) begin free_found <= 1; free_slot <= cursor; end
                        if (!last) begin cursor <= cursor+1'b1; state <= LOAD; end
                        else if (!free_found && fetched_valid) begin error <= 5; state <= RESPONSE; end
                        else begin
                            target <= free_found ? free_slot : cursor;
                            result_record <= calculated; state <= PUBLISH;
                        end
                    end
                end else if (record_match) begin
                    target <= cursor; arrival_token <= fetched[159:128];
                    if (action == 5) begin result_record <= fetched[159:0]; state <= RESPONSE; end
                    else if (calculation_error != 0) begin error <= calculation_error; state <= RESPONSE; end
                    else begin result_record <= calculated; phase_advanced <= calculation_advanced; state <= PUBLISH; end
                end else if (!last) begin cursor <= cursor+1'b1; state <= LOAD; end
                else begin error <= 6; state <= RESPONSE; end
            end
            PUBLISH: if (publication_ack) begin
                valid[target] <= 1;
                if (action == 0) occupancy <= occupancy+1'b1;
                state <= RESPONSE;
            end
            RESPONSE: if (response_ready) state <= IDLE;
            default: state <= IDLE;
        endcase
    end
endmodule
