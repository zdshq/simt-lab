// Finite command/fragment pools feed one held read offer, one held write offer and one completion offer.
// 有限命令/片段池提供一个保持的读请求、一个保持的写请求与一个完成请求。
module dma_engine #(parameter integer COMMANDS=64, FRAGMENTS=64) (
    input wire clk, reset,
    input wire [6:0] command_limit, fragment_limit,
    input wire [7:0] chunk_bytes,
    input wire command_valid,
    output wire command_ready, command_accept,
    input wire [63:0] command_id,
    input wire [31:0] command_cta, command_source, command_destination, command_barrier, command_phase, command_bytes,
    output wire read_valid, write_valid,
    input wire read_ready, write_ready,
    output wire [5:0] read_slot, write_slot,
    output wire [63:0] read_copy, write_copy,
    output wire [31:0] read_cta, write_cta, read_mask, write_mask,
    output wire read_word, write_word,
    output wire [1055:0] read_addresses, write_addresses,
    output wire [1023:0] write_data,
    input wire read_response_valid, write_response_valid,
    output wire read_response_ready, write_response_ready,
    input wire [5:0] read_response_slot, write_response_slot,
    input wire [1023:0] read_response_data,
    output wire [7:0] read_response_bytes, write_response_bytes,
    output wire [63:0] read_response_copy, write_response_copy,
    output wire completion_valid,
    input wire completion_ready,
    output wire [5:0] completion_slot,
    output wire [63:0] completion_id,
    output wire [31:0] completion_cta, completion_barrier, completion_phase,
    output wire [12:0] completion_bytes,
    input wire release_valid,
    input wire [5:0] release_slot,
    input wire [63:0] release_id,
    output wire release_ready,
    output reg [6:0] command_occupancy, fragment_occupancy,
    output wire fragment_allocated, fragment_stalled, clock_enable,
    output wire [7:0] allocated_bytes,
    output reg [3:0] error
);
    wire [COMMANDS-1:0] command_live, command_claimed;
    wire [63:0] ids [0:COMMANDS-1];
    wire [31:0] ctas [0:COMMANDS-1], sources [0:COMMANDS-1], destinations [0:COMMANDS-1];
    wire [31:0] barriers [0:COMMANDS-1], phases [0:COMMANDS-1];
    wire [12:0] sizes [0:COMMANDS-1], cursors [0:COMMANDS-1];
    wire [6:0] outstanding [0:COMMANDS-1];
    wire [2:0] states [0:FRAGMENTS-1];
    wire [5:0] owners [0:FRAGMENTS-1];
    wire [32:0] fragment_sources [0:FRAGMENTS-1], fragment_destinations [0:FRAGMENTS-1];
    wire [7:0] fragment_sizes [0:FRAGMENTS-1];
    wire [FRAGMENTS-1:0] word_widths;
    wire [1023:0] payloads [0:FRAGMENTS-1];
    reg [COMMANDS-1:0] free_commands, runnable_commands, finished_commands;
    reg [FRAGMENTS-1:0] free_fragments, readable_fragments, writable_fragments;
    reg duplicate_id;
    reg [5:0] cursor, read_cursor, write_cursor, complete_cursor;
    reg read_pending, write_pending, complete_pending;
    reg [5:0] held_read, held_write, held_complete;
    wire config_valid = command_limit!=0 && command_limit<=7'(COMMANDS) && fragment_limit!=0 &&
        fragment_limit<=7'(FRAGMENTS) && chunk_bytes!=0 && chunk_bytes<=128;
    integer c, f;
    always @* begin
        free_commands=0; runnable_commands=0; finished_commands=0; duplicate_id=0;
        for (c=0; c<COMMANDS; c=c+1) begin
            free_commands[c]=!command_live[c] && c<{25'b0,command_limit};
            runnable_commands[c]=command_live[c] && !command_claimed[c] && cursors[c]<sizes[c];
            finished_commands[c]=command_live[c] && !command_claimed[c] && cursors[c]==sizes[c] && outstanding[c]==0;
            if (command_live[c] && ids[c]==command_id) duplicate_id=1;
        end
        free_fragments=0; readable_fragments=0; writable_fragments=0;
        for (f=0; f<FRAGMENTS; f=f+1) begin
            free_fragments[f]=states[f]==0 && f<{25'b0,fragment_limit};
            readable_fragments[f]=states[f]==1; writable_fragments[f]=states[f]==3;
        end
    end
    wire free_command_valid, candidate_valid, free_fragment_valid, read_candidate_valid, write_candidate_valid, complete_candidate_valid;
    wire [5:0] free_command, candidate, free_fragment, read_candidate, write_candidate, complete_candidate;
    dma_select #(.COUNT(COMMANDS)) select_free_command(free_commands,6'b0,free_command_valid,free_command);
    dma_select #(.COUNT(COMMANDS)) select_command(runnable_commands,cursor,candidate_valid,candidate);
    dma_select #(.COUNT(COMMANDS)) select_complete(finished_commands,complete_cursor,complete_candidate_valid,complete_candidate);
    dma_select #(.COUNT(FRAGMENTS)) select_free_fragment(free_fragments,6'b0,free_fragment_valid,free_fragment);
    dma_select #(.COUNT(FRAGMENTS)) select_read(readable_fragments,read_cursor,read_candidate_valid,read_candidate);
    dma_select #(.COUNT(FRAGMENTS)) select_write(writable_fragments,write_cursor,write_candidate_valid,write_candidate);

    wire command_bad = command_valid && (command_bytes==0 || command_bytes>4096 || duplicate_id);
    assign command_ready=!reset && config_valid && free_command_valid && !command_bad;
    assign command_accept=command_valid && command_ready;
    wire [32:0] next_source={1'b0,sources[candidate]}+{20'b0,cursors[candidate]};
    wire [32:0] next_destination={1'b0,destinations[candidate]}+{20'b0,cursors[candidate]};
    wire [12:0] remaining=sizes[candidate]-cursors[candidate];
    wire word_mode=next_source[1:0]==0 && next_destination[1:0]==0 && remaining>=4 && chunk_bytes>=4;
    wire [12:0] limited=remaining<{5'b0,chunk_bytes} ? remaining : {5'b0,chunk_bytes};
    wire [7:0] fragment_bytes=word_mode ? {limited[7:2],2'b0} : limited>32 ? 8'd32 : limited[7:0];
    assign fragment_allocated=!reset && config_valid && candidate_valid && free_fragment_valid;
    assign allocated_bytes=fragment_bytes;
    assign fragment_stalled=!reset && candidate_valid && !free_fragment_valid;

    assign read_valid=read_pending && !reset; assign read_slot=held_read;
    assign write_valid=write_pending && !reset; assign write_slot=held_write;
    assign read_copy=ids[owners[held_read]]; assign write_copy=ids[owners[held_write]];
    assign read_cta=ctas[owners[held_read]]; assign write_cta=ctas[owners[held_write]];
    assign read_word=word_widths[held_read]; assign write_word=word_widths[held_write];
    assign write_data=payloads[held_write];
    assign read_response_ready=!reset && {1'b0,read_response_slot}<7'(FRAGMENTS) && states[read_response_slot]==2;
    assign write_response_ready=!reset && {1'b0,write_response_slot}<7'(FRAGMENTS) && states[write_response_slot]==4;
    assign read_response_bytes=fragment_sizes[read_response_slot]; assign write_response_bytes=fragment_sizes[write_response_slot];
    assign read_response_copy=ids[owners[read_response_slot]]; assign write_response_copy=ids[owners[write_response_slot]];
    wire read_return=read_response_valid && read_response_ready;
    wire write_return=write_response_valid && write_response_ready;
    assign completion_valid=complete_pending && !reset; assign completion_slot=held_complete;
    assign completion_id=ids[held_complete]; assign completion_cta=ctas[held_complete];
    assign completion_barrier=barriers[held_complete]; assign completion_phase=phases[held_complete];
    assign completion_bytes=sizes[held_complete];
    assign release_ready=!reset && {1'b0,release_slot}<7'(COMMANDS) && command_live[release_slot] &&
        command_claimed[release_slot] && ids[release_slot]==release_id;
    wire release_accept=release_valid && release_ready;
    assign clock_enable=reset || command_occupancy!=0 || fragment_occupancy!=0 || command_valid || release_valid ||
        read_response_valid || write_response_valid;
    always @* begin
        error=0;
        if (!reset) begin
            if (!config_valid) error=1;
            else if (command_valid && (command_bytes==0 || command_bytes>4096)) error=2;
            else if (command_valid && duplicate_id) error=3;
            else if (read_response_valid && !read_response_ready) error=4;
            else if (write_response_valid && !write_response_ready) error=5;
            else if (release_valid && !release_ready) error=6;
        end
    end

    // Registered selections keep every offer stable until its consumer accepts it.
    // 寄存的选择使每个请求在消费者接受之前保持稳定。
    always @(posedge clk) begin
        if (reset) begin
            cursor<=0; read_cursor<=0; write_cursor<=0; complete_cursor<=0;
            read_pending<=0; write_pending<=0; complete_pending<=0; held_read<=0; held_write<=0; held_complete<=0;
            command_occupancy<=0; fragment_occupancy<=0;
        end else begin
            if (command_accept || release_accept) command_occupancy<=command_occupancy+{6'b0,command_accept}-{6'b0,release_accept};
            if (fragment_allocated || write_return) fragment_occupancy<=fragment_occupancy+{6'b0,fragment_allocated}-{6'b0,write_return};
            if (fragment_allocated) cursor<=candidate==6'(COMMANDS-1) ? 6'b0 : candidate+1'b1;
            if (!read_pending && read_candidate_valid) begin held_read<=read_candidate; read_pending<=1; end
            else if (read_valid && read_ready) begin read_pending<=0; read_cursor<=held_read==6'(FRAGMENTS-1) ? 6'b0 : held_read+1'b1; end
            if (!write_pending && write_candidate_valid) begin held_write<=write_candidate; write_pending<=1; end
            else if (write_valid && write_ready) begin write_pending<=0; write_cursor<=held_write==6'(FRAGMENTS-1) ? 6'b0 : held_write+1'b1; end
            if (!complete_pending && complete_candidate_valid) begin held_complete<=complete_candidate; complete_pending<=1; end
            else if (completion_valid && completion_ready) begin complete_pending<=0; complete_cursor<=held_complete==6'(COMMANDS-1) ? 6'b0 : held_complete+1'b1; end
        end
    end
    genvar index, lane;
    generate for (index=0; index<COMMANDS; index=index+1) begin: commands
        dma_command entry (
            .clk(clk), .reset(reset), .allocate(command_accept && free_command==6'(index)),
            .fragment_allocate(fragment_allocated && candidate==6'(index)),
            .fragment_retire(write_return && owners[write_response_slot]==6'(index)),
            .claim(completion_valid && completion_ready && held_complete==6'(index)),
            .release_command(release_accept && release_slot==6'(index)),
            .input_id(command_id), .input_cta(command_cta), .input_source(command_source), .input_destination(command_destination),
            .input_barrier(command_barrier), .input_phase(command_phase), .input_bytes(command_bytes[12:0]), .fragment_bytes(fragment_bytes),
            .valid(command_live[index]), .claimed(command_claimed[index]), .id(ids[index]), .cta(ctas[index]), .source(sources[index]),
            .destination(destinations[index]), .barrier(barriers[index]), .phase(phases[index]),
            .bytes(sizes[index]), .cursor(cursors[index]), .outstanding(outstanding[index])
        );
    end
    for (index=0; index<FRAGMENTS; index=index+1) begin: fragments
        dma_fragment entry (
            .clk(clk), .reset(reset), .allocate(fragment_allocated && free_fragment==6'(index)),
            .input_owner(candidate), .input_source(next_source), .input_destination(next_destination), .input_bytes(fragment_bytes),
            .input_word(word_mode), .returned_data(read_response_data),
            .read_accept(read_valid && read_ready && held_read==6'(index)), .read_return(read_return && read_response_slot==6'(index)),
            .write_accept(write_valid && write_ready && held_write==6'(index)), .write_return(write_return && write_response_slot==6'(index)),
            .state(states[index]), .owner(owners[index]), .source(fragment_sources[index]), .destination(fragment_destinations[index]),
            .bytes(fragment_sizes[index]), .word_width(word_widths[index]), .payload(payloads[index])
        );
    end
    for (lane=0; lane<32; lane=lane+1) begin: lanes
        assign read_mask[lane]=(word_widths[held_read] ? lane*4 : lane)<{24'b0,fragment_sizes[held_read]};
        assign write_mask[lane]=(word_widths[held_write] ? lane*4 : lane)<{24'b0,fragment_sizes[held_write]};
        assign read_addresses[lane*33+:33]=fragment_sources[held_read]+(word_widths[held_read] ? 33'(lane*4) : 33'(lane));
        assign write_addresses[lane*33+:33]=fragment_destinations[held_write]+(word_widths[held_write] ? 33'(lane*4) : 33'(lane));
    end endgenerate
endmodule
