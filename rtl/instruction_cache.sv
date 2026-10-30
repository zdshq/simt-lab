// A blocking 4 KiB instruction cache has one synchronous lookup and four 128-bit refill beats.
// 阻塞式 4 KiB 指令缓存使用一次同步查询和四个 128 位补充拍。
module instruction_cache (
    input wire clk, reset,
    input wire query_valid,
    input wire [31:0] query_pc,
    input wire [32:0] program_words,
    output wire response_valid, address_error,
    output wire [127:0] response_words,
    output wire miss_valid,
    output wire [31:0] miss_pc,
    input wire miss_ready,
    input wire fill_valid,
    input wire [127:0] fill_words,
    output wire fill_ready, busy,
    output wire read_event, hit_event, miss_event, fill_event
);
    localparam [2:0] IDLE=0, LOOKUP=1, REQUEST=2, FILL=3, HOLD=4;
    reg [127:0] data [0:255];
    reg [23:0] tags [0:63];
    reg [63:0] valid_lines;
    reg [2:0] state;
    reg [31:0] pending_pc;
    reg [127:0] word_value;
    reg [23:0] tag_value;
    reg valid_value;
    reg response_seen;
    reg [1:0] beat;
    wire hit=valid_value && tag_value==pending_pc[31:8];
    wire available=state==HOLD || (state==LOOKUP && hit);
    assign address_error=!reset && query_valid && {1'b0,query_pc}>=program_words;
    assign response_valid=!reset && query_valid && !address_error && available && query_pc==pending_pc;
    assign response_words=word_value;
    assign read_event=!reset && query_valid && !address_error &&
        (state==IDLE || (available && query_pc!=pending_pc));
    assign hit_event=!reset && state==LOOKUP && hit;
    assign miss_event=!reset && state==LOOKUP && !hit;
    assign miss_valid=!reset && state==REQUEST;
    assign miss_pc={pending_pc[31:2],2'b0};
    assign fill_ready=!reset && state==FILL;
    assign fill_event=fill_ready && fill_valid;
    assign busy=state==LOOKUP || state==REQUEST || state==FILL || (state==HOLD && !response_seen);

    // Data/tag arrays have no reset; validity prevents uninitialized storage from becoming an instruction.
    // 数据/tag 阵列不复位；有效位阻止未初始化存储成为指令。
    always @(posedge clk) begin
        if (read_event) begin
            word_value<=data[query_pc[7:0]]; tag_value<=tags[query_pc[7:2]];
        end
        if (fill_event) begin
            data[{pending_pc[7:2],beat}]<=fill_words;
            if (beat==pending_pc[1:0]) word_value<=fill_words;
            if (beat==3) tags[pending_pc[7:2]]<=pending_pc[31:8];
        end
    end

    always @(posedge clk) begin
        if (reset) begin state<=IDLE; valid_lines<=0; pending_pc<=0; valid_value<=0; beat<=0; response_seen<=0; end
        else if (read_event) begin
            state<=LOOKUP; pending_pc<=query_pc; valid_value<=valid_lines[query_pc[7:2]]; response_seen<=0;
        end else begin
            if (response_valid) response_seen<=1;
            case (state)
            LOOKUP: state<=hit ? HOLD : REQUEST;
            REQUEST: if (miss_ready) begin state<=FILL; beat<=0; end
            FILL: if (fill_event) begin
                beat<=beat+1'b1;
                if (beat==3) begin state<=HOLD; valid_lines[pending_pc[7:2]]<=1; end
            end
            HOLD: if (!query_valid) state<=IDLE;
            default: begin end
            endcase
        end
    end
endmodule
