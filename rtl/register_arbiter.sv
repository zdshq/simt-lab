// Independent read and write bank ports grant one requester per bank in cyclic priority order.
// 独立读写 bank 端口按循环优先级为每个 bank 授予一个请求者。
module register_arbiter #(
    parameter integer BANKS = 4,
    parameter integer REQUESTERS = 4,
    parameter integer CURSOR_BITS = $clog2(REQUESTERS)
) (
    input wire clk,
    input wire reset,
    input wire [REQUESTERS-1:0] read_valid,
    input wire [REQUESTERS*6-1:0] read_register,
    input wire [REQUESTERS-1:0] write_valid,
    input wire [REQUESTERS*6-1:0] write_register,
    output reg [REQUESTERS-1:0] read_grant,
    output reg [REQUESTERS-1:0] write_grant
);
    reg [CURSOR_BITS-1:0] read_cursor [0:BANKS-1], write_cursor [0:BANKS-1];
    reg [CURSOR_BITS-1:0] read_next [0:BANKS-1], write_next [0:BANKS-1];
    integer bank, offset, reader, writer, reset_bank, zero_requester;
    reg read_taken, write_taken;

    // The constant zero register consumes no physical bank and can satisfy every requester.
    // 常零寄存器不消耗物理 bank，可以满足所有请求者。
    always @* begin
        read_grant = 0; write_grant = 0;
        reader = 0; writer = 0; read_taken = 0; write_taken = 0;
        for (zero_requester = 0; zero_requester < REQUESTERS; zero_requester = zero_requester+1) begin
            if (read_valid[zero_requester] && read_register[zero_requester*6+:6] == 63) read_grant[zero_requester] = 1;
            if (write_valid[zero_requester] && write_register[zero_requester*6+:6] == 63) write_grant[zero_requester] = 1;
        end
        for (bank = 0; bank < BANKS; bank = bank+1) begin
            read_taken = 0; write_taken = 0;
            read_next[bank] = read_cursor[bank]; write_next[bank] = write_cursor[bank];
            for (offset = 0; offset < REQUESTERS; offset = offset+1) begin
                reader = (32'(read_cursor[bank])+offset)%REQUESTERS;
                writer = (32'(write_cursor[bank])+offset)%REQUESTERS;
                if (!reset && !read_taken && read_valid[reader] && read_register[reader*6+:6] != 63 &&
                    32'(read_register[reader*6+:6])%BANKS == bank) begin
                    read_grant[reader] = 1; read_taken = 1; read_next[bank] = CURSOR_BITS'((reader+1)%REQUESTERS);
                end
                if (!reset && !write_taken && write_valid[writer] && write_register[writer*6+:6] != 63 &&
                    32'(write_register[writer*6+:6])%BANKS == bank) begin
                    write_grant[writer] = 1; write_taken = 1; write_next[bank] = CURSOR_BITS'((writer+1)%REQUESTERS);
                end
            end
        end
        if (reset) begin read_grant = 0; write_grant = 0; end
    end

    always @(posedge clk) begin
        for (reset_bank = 0; reset_bank < BANKS; reset_bank = reset_bank+1) begin
            read_cursor[reset_bank] <= reset ? 0 : read_next[reset_bank];
            write_cursor[reset_bank] <= reset ? 0 : write_next[reset_bank];
        end
    end
endmodule
