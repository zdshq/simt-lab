// A finite nonblocking sector cache uses write-through, no-write-allocate, and explicit handshakes.
// 有限非阻塞 sector 缓存使用写穿、不写分配和显式握手。
module sector_cache #(
    parameter integer SETS = 128,
    parameter integer WAYS = 4,
    parameter integer SECTORS = 4,
    parameter integer MSHRS = 4,
    parameter integer RESPONSES = 8,
    parameter integer MERGE_LIMIT = 4,
    parameter integer ADDRESS_BITS = 64,
    parameter integer ID_BITS = 16,
    parameter integer MSHR_BITS = $clog2(MSHRS),
    parameter integer LINE_BITS = $clog2(SETS*WAYS),
    parameter integer DATA_BITS = $clog2(SETS*WAYS*SECTORS)
) (
    input wire clk,
    input wire reset,
    input wire req_valid,
    output wire req_ready,
    input wire [ADDRESS_BITS-1:0] req_address,
    input wire req_write,
    input wire [255:0] req_data,
    input wire [31:0] req_mask,
    input wire [ID_BITS-1:0] req_id,
    output wire rsp_valid,
    input wire rsp_ready,
    output wire [ID_BITS-1:0] rsp_id,
    output wire [255:0] rsp_data,
    output reg mem_valid,
    input wire mem_ready,
    output reg [ADDRESS_BITS-1:0] mem_address,
    output reg mem_write,
    output reg [255:0] mem_data,
    output reg [31:0] mem_mask,
    output reg [MSHR_BITS-1:0] mem_id,
    input wire fill_valid,
    output wire fill_ready,
    input wire [MSHR_BITS-1:0] fill_id,
    input wire [255:0] fill_data,
    input wire invalidate_valid,
    input wire invalidate_all,
    input wire [ADDRESS_BITS-1:5] invalidate_first,
    input wire [ADDRESS_BITS-1:5] invalidate_last,
    output wire invalidate_ready,
    output reg invalidate_busy,
    output reg invalidate_done,
    output wire idle,
    output wire event_line_miss, event_read_fill,
    output wire [MSHRS-1:0] active_misses,
    output wire reject_response, reject_mshr, reject_reservation,
    output reg [63:0] hits,
    output reg [63:0] misses,
    output reg [63:0] merges,
    output reg [63:0] evictions
);
    localparam integer LINES = SETS*WAYS;
    localparam integer SECTOR_BITS = $clog2(SECTORS);
    localparam integer TAG_BITS = ADDRESS_BITS-5-SECTOR_BITS;
    localparam integer RANK_BITS = $clog2(WAYS);
    localparam integer RESPONSE_BITS = $clog2(RESPONSES);
    localparam [1:0] LOOKUP = 0, READ_WAIT = 1, READ_RETURN = 2;

    reg [TAG_BITS-1:0] tags [0:LINES-1];
    reg [LINES-1:0] present;
    reg [SECTORS-1:0] valid [0:LINES-1];
    // Rank updates touch all ways together; implement this metadata as flip-flops, not a multiport SRAM.
    // 排名更新同时触及所有 way；该元数据使用触发器实现，而非多端口 SRAM。
    (* mem2reg *) reg [RANK_BITS-1:0] rank [0:LINES-1];
    reg [MSHRS-1:0] occupied, sent, store, has_line;
    reg [ADDRESS_BITS-1:0] address [0:MSHRS-1];
    reg [255:0] payload [0:MSHRS-1];
    reg [31:0] byte_mask [0:MSHRS-1];
    reg [LINE_BITS-1:0] line [0:MSHRS-1];
    reg [RESPONSES-1:0] waiters [0:MSHRS-1];
    reg [RESPONSES-1:0] response_used, response_done;
    reg [ID_BITS-1:0] response_id [0:RESPONSES-1];
    reg [255:0] response_data [0:RESPONSES-1];
    reg [1:0] state;
    reg [RESPONSE_BITS-1:0] read_slot;
    reg [RESPONSE_BITS-1:0] response_head;
    reg [RESPONSE_BITS-1:0] response_cursor;
    reg response_present;
    reg [MSHR_BITS-1:0] memory_cursor;
    integer match_line, chosen_line, same_miss, free_miss, free_response;
    integer ready_response, next_memory, merge_count, locked, best_rank;
    integer lookup_way, lookup_miss, lookup_response, lookup_offset, lookup_candidate;
    integer lookup_index, select_response, select_memory;
    integer update_line, update_miss, update_response, invalidate_sector, invalidate_miss;
    reg [LINE_BITS-1:0] invalidate_cursor;
    reg [ADDRESS_BITS-1:5] first_sector, last_sector;
    reg clear_all, invalidate_conflict;
    wire [TAG_BITS-1:0] tag = req_address[ADDRESS_BITS-1:5+SECTOR_BITS];
    wire [SECTOR_BITS-1:0] sector = req_address[5+:SECTOR_BITS];
    wire cache_hit = match_line >= 0 && valid[match_line][sector];
    wire accept = req_valid && req_ready;
    wire fill_accept = fill_valid && fill_ready;
    wire hit_accept = accept && same_miss < 0 && !req_write && cache_hit;
    wire [DATA_BITS-1:0] read_index = DATA_BITS'(match_line*SECTORS+32'(sector));
    wire [DATA_BITS-1:0] write_index = DATA_BITS'(32'(line[fill_id])*SECTORS+
                                                     32'(address[fill_id][5+:SECTOR_BITS]));
    wire [255:0] ram_data;

    sector_sram #(.WORDS(LINES*SECTORS)) data_array (
        .clk(clk), .read_enable(hit_accept), .read_index(read_index), .read_data(ram_data),
        .write_enable(fill_accept && !store[fill_id]), .write_index(write_index), .write_data(fill_data)
    );

    // Associative lookup excludes every line still owned by an outstanding sector request.
    // 相联查找排除所有仍由在途 sector 请求持有的替换行。
    always @* begin
        match_line = -1; chosen_line = -1; best_rank = -1;
        lookup_index = 32'(tag % TAG_BITS'(SETS))*WAYS;
        locked = 0;
        for (lookup_way = 0; lookup_way < WAYS; lookup_way = lookup_way+1) begin
            if (present[lookup_index+lookup_way] && tags[lookup_index+lookup_way] == tag)
                match_line = lookup_index+lookup_way;
            locked = 0;
            for (lookup_miss = 0; lookup_miss < MSHRS; lookup_miss = lookup_miss+1)
                if (occupied[lookup_miss] && has_line[lookup_miss] &&
                    32'(line[lookup_miss]) == lookup_index+lookup_way) locked = 1;
            if (locked == 0 && (best_rank < 0 || !present[lookup_index+lookup_way] ||
                               32'(rank[lookup_index+lookup_way]) > 32'(best_rank))) begin
                if (best_rank < WAYS) chosen_line = lookup_index+lookup_way;
                best_rank = present[lookup_index+lookup_way] ? 32'(rank[lookup_index+lookup_way]) : WAYS;
            end
        end
    end

    // A merged reader consumes a response credit but does not allocate another memory transaction.
    // 合并的读取消耗响应额度，但不分配新的内存事务。
    always @* begin
        same_miss = -1; free_miss = -1; free_response = -1; merge_count = 0;
        for (lookup_candidate = MSHRS-1; lookup_candidate >= 0; lookup_candidate = lookup_candidate-1) begin
            if (!occupied[lookup_candidate]) free_miss = lookup_candidate;
            if (occupied[lookup_candidate] && address[lookup_candidate] == req_address) same_miss = lookup_candidate;
        end
        for (lookup_response = RESPONSES-1; lookup_response >= 0; lookup_response = lookup_response-1) begin
            if (!response_used[lookup_response]) free_response = lookup_response;
            if (same_miss >= 0 && waiters[same_miss][lookup_response]) merge_count = merge_count+1;
        end
    end

    assign req_ready = !reset && !invalidate_valid && !invalidate_busy && !fill_valid && state == LOOKUP && free_response >= 0 &&
        (same_miss >= 0 ? (!req_write && !store[same_miss] && merge_count < MERGE_LIMIT) :
         ((!req_write && cache_hit) || (free_miss >= 0 && (req_write || match_line >= 0 || chosen_line >= 0))));
    assign fill_ready = !reset && occupied[fill_id] && sent[fill_id];
    assign idle = occupied == 0 && response_used == 0 && state == LOOKUP && !mem_valid && !invalidate_busy;
    assign invalidate_ready = !reset && !invalidate_busy && state == LOOKUP &&
                              (invalidate_all ? idle : !invalidate_conflict);
    assign rsp_valid = response_present;
    assign rsp_id = response_id[response_head];
    assign rsp_data = response_data[response_head];
    assign event_line_miss = accept && !req_write && same_miss < 0 && match_line < 0;
    assign event_read_fill = fill_accept && !store[fill_id];
    assign active_misses = occupied;
    wire lookup_available = !reset && !invalidate_valid && !invalidate_busy && !fill_valid && state == LOOKUP;
    assign reject_response = lookup_available && free_response < 0;
    assign reject_mshr = lookup_available && free_response >= 0 && (same_miss >= 0 || free_miss < 0);
    assign reject_reservation = lookup_available && free_response >= 0 && same_miss < 0 && free_miss >= 0;

    // Range commands may coexist with unrelated fills but cannot race the sectors they invalidate.
    // 范围命令可与无关填充并行，但不能与目标 sector 的填充竞争。
    always @* begin
        invalidate_conflict = 0;
        for (invalidate_miss = 0; invalidate_miss < MSHRS; invalidate_miss = invalidate_miss+1)
            if (occupied[invalidate_miss] && address[invalidate_miss][ADDRESS_BITS-1:5] >= invalidate_first &&
                address[invalidate_miss][ADDRESS_BITS-1:5] <= invalidate_last) invalidate_conflict = 1;
    end

    // A single metadata scan visits one line each clock; data SRAM is never cleared or bypassed.
    // 单个元数据扫描器每拍访问一行，不清零或绕过数据 SRAM。
    task service_invalidation;
        begin
            invalidate_done <= 0;
            if (invalidate_valid && invalidate_ready) begin
                invalidate_busy <= 1; invalidate_cursor <= 0;
                clear_all <= invalidate_all; first_sector <= invalidate_first; last_sector <= invalidate_last;
            end
            if (invalidate_busy) begin
                if (clear_all) present[invalidate_cursor] <= 0;
                for (invalidate_sector = 0; invalidate_sector < SECTORS; invalidate_sector = invalidate_sector+1)
                    if (clear_all || (present[invalidate_cursor] &&
                        {tags[invalidate_cursor], SECTOR_BITS'(invalidate_sector)} >= first_sector &&
                        {tags[invalidate_cursor], SECTOR_BITS'(invalidate_sector)} <= last_sector))
                        valid[invalidate_cursor][invalidate_sector] <= 0;
                if (invalidate_cursor == LINE_BITS'(LINES-1)) begin
                    invalidate_busy <= 0; invalidate_done <= 1;
                end else invalidate_cursor <= invalidate_cursor+1'b1;
            end
        end
    endtask

    // Registered selections hold all response and downstream fields stable under backpressure.
    // 寄存选择在反压期间保持所有响应和下游字段稳定。
    always @* begin
        ready_response = -1; next_memory = -1;
        for (select_response = RESPONSES-1; select_response >= 0; select_response = select_response-1)
            if (response_done[(32'(response_cursor)+select_response)%RESPONSES])
                ready_response = (32'(response_cursor)+select_response)%RESPONSES;
        for (lookup_offset = MSHRS-1; lookup_offset >= 0; lookup_offset = lookup_offset-1) begin
            select_memory = (32'(memory_cursor)+lookup_offset)%MSHRS;
            if (occupied[select_memory] && !sent[select_memory]) next_memory = select_memory;
        end
    end

    // Exact finite LRU ranks avoid unbounded timestamps and retain ordering among unlocked ways.
    // 精确的有限 LRU 排名避免无限时间戳，并保留未锁定 way 的相对顺序。
    task touch_line(input integer selected);
        integer way_at, base_at;
        begin
            base_at = (selected/WAYS)*WAYS;
            for (way_at = 0; way_at < WAYS; way_at = way_at+1)
                if (rank[base_at+way_at] < rank[selected]) rank[base_at+way_at] <= rank[base_at+way_at]+1'b1;
            rank[selected] <= 0;
        end
    endtask

    task allocate_miss;
        integer selected;
        begin
            selected = match_line >= 0 ? match_line : (req_write ? -1 : chosen_line);
            occupied[free_miss] <= 1; sent[free_miss] <= 0; store[free_miss] <= req_write;
            address[free_miss] <= req_address; payload[free_miss] <= req_data; byte_mask[free_miss] <= req_mask;
            has_line[free_miss] <= selected >= 0; line[free_miss] <= LINE_BITS'(selected);
            waiters[free_miss] <= RESPONSES'(1) << free_response;
            if (!req_write) misses <= misses+1'b1;
            if (selected >= 0) begin
                if (match_line < 0) begin
                    if (present[selected]) evictions <= evictions+1'b1;
                    present[selected] <= 1; tags[selected] <= tag; valid[selected] <= 0;
                end else if (req_write) valid[selected][sector] <= 0;
                touch_line(selected);
            end
        end
    endtask

    // Accepted misses own their way and every waiter until the actual fill arrives.
    // 已接受未命中持有其 way 和所有等待者，直到实际填充到达。
    task accept_request;
        begin
            response_used[free_response] <= 1; response_done[free_response] <= 0;
            response_id[free_response] <= req_id;
            if (same_miss >= 0) begin
                waiters[same_miss][free_response] <= 1; merges <= merges+1'b1;
            end else if (hit_accept) begin
                read_slot <= RESPONSE_BITS'(free_response); state <= READ_WAIT;
                touch_line(match_line); hits <= hits+1'b1;
            end else allocate_miss();
        end
    endtask

    task accept_fill;
        integer slot;
        begin
            occupied[fill_id] <= 0;
            if (!store[fill_id]) valid[line[fill_id]][address[fill_id][5+:SECTOR_BITS]] <= 1;
            for (slot = 0; slot < RESPONSES; slot = slot+1)
                if (waiters[fill_id][slot]) begin
                    response_data[slot] <= fill_data; response_done[slot] <= 1;
                end
        end
    endtask

    // Present bits hide old metadata; allocating a new line initializes its tag and sector validity.
    // present 位屏蔽旧元数据；分配新行时初始化其 tag 和 sector 有效位。
    always @(posedge clk) begin
        if (reset) begin
            invalidate_busy <= 0; invalidate_done <= 0; invalidate_cursor <= 0;
            clear_all <= 0; first_sector <= 0; last_sector <= 0;
            present <= 0; occupied <= 0; sent <= 0; store <= 0; has_line <= 0;
            response_used <= 0; response_done <= 0; response_present <= 0; response_head <= 0; response_cursor <= 0;
            state <= LOOKUP; read_slot <= 0; mem_valid <= 0; memory_cursor <= 0;
            mem_address <= 0; mem_write <= 0; mem_data <= 0; mem_mask <= 0; mem_id <= 0;
            hits <= 0; misses <= 0; merges <= 0; evictions <= 0;
            for (update_line = 0; update_line < LINES; update_line = update_line+1) begin
                rank[update_line] <= RANK_BITS'(update_line%WAYS);
            end
            for (update_miss = 0; update_miss < MSHRS; update_miss = update_miss+1) begin
                address[update_miss] <= 0; payload[update_miss] <= 0; byte_mask[update_miss] <= 0;
                line[update_miss] <= 0; waiters[update_miss] <= 0;
            end
            for (update_response = 0; update_response < RESPONSES; update_response = update_response+1) begin
                response_id[update_response] <= 0; response_data[update_response] <= 0;
            end
        end else begin
            service_invalidation();
            if (accept) accept_request();
            if (fill_accept) accept_fill();
            if (state == READ_WAIT) state <= READ_RETURN;
            if (state == READ_RETURN) begin
                response_data[read_slot] <= ram_data; response_done[read_slot] <= 1; state <= LOOKUP;
            end
            service_ports();
        end
    end

    // A sent bit is set only by a downstream handshake, never merely by selection.
    // sent 位只由下游握手设置，不能仅因被选择而设置。
    task service_ports;
        begin
            if (mem_valid && mem_ready) begin
                sent[mem_id] <= 1; mem_valid <= 0; memory_cursor <= mem_id+1'b1;
            end else if (!mem_valid && next_memory >= 0) begin
                mem_valid <= 1; mem_id <= MSHR_BITS'(next_memory);
                mem_address <= address[next_memory]; mem_write <= store[next_memory];
                mem_data <= payload[next_memory]; mem_mask <= byte_mask[next_memory];
            end
            if (response_present && rsp_ready) begin
                response_used[response_head] <= 0; response_done[response_head] <= 0; response_present <= 0;
                response_cursor <= response_head+1'b1;
            end else if (!response_present && ready_response >= 0) begin
                response_present <= 1; response_head <= RESPONSE_BITS'(ready_response);
            end
        end
    endtask
endmodule
