// Exact bounded arithmetic for one transaction; storage publication is controlled separately.
// 单条事务的精确有界算术；存储发布由独立控制器管理。
module transaction_update (
    input wire [2:0] operation,
    input wire [159:0] previous,
    input wire [31:0] amount, completion_phase,
    output reg [159:0] updated,
    output reg [3:0] error,
    output reg advanced
);
    wire [31:0] phase = previous[159:128], expected = previous[127:96];
    wire [31:0] arrivals = previous[95:64], bytes_left = previous[63:32], issued = previous[31:0];
    reg [31:0] next_phase, next_expected, next_arrivals, next_bytes, next_issued;
    always @* begin
        error = 0; advanced = 0;
        next_phase = phase; next_expected = expected; next_arrivals = arrivals;
        next_bytes = bytes_left; next_issued = issued;
        case (operation)
            0: begin
                if (amount == 0 || amount > 1024) error = 3;
                next_phase = 0; next_expected = amount; next_arrivals = amount; next_bytes = 0; next_issued = 0;
            end
            1: begin
                if (amount == 0 || {1'b0,bytes_left}+{1'b0,amount} > 33'h0ffffffff) error = 7;
                else next_bytes = bytes_left+amount;
            end
            2: begin
                if (arrivals == 0) error = 8;
                else next_arrivals = arrivals-1'b1;
            end
            3: begin
                if (amount == 0 || amount > 4096) error = 9;
                else if (issued > bytes_left || amount > bytes_left-issued) error = 10;
                else next_issued = issued+amount;
            end
            4: begin
                if (completion_phase != phase) error = 11;
                else if (issued < amount || bytes_left < amount || amount == 0) error = 12;
                else begin next_bytes = bytes_left-amount; next_issued = issued-amount; end
            end
            default: error = 1;
        endcase
        if (error == 0 && next_arrivals == 0 && next_bytes == 0) begin
            if (next_phase == 32'hffffffff) error = 13;
            else begin next_phase = next_phase+1'b1; next_arrivals = next_expected; advanced = 1; end
        end
        updated = error == 0 ? {next_phase,next_expected,next_arrivals,next_bytes,next_issued} : previous;
    end
endmodule
