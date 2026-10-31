// One physical 128 KiB TMEM serves eight bounded vectors after hardware page translation.
// 一份物理 128 KiB TMEM 在硬件页转换之后服务八个有界向量。
module tmem_memory #(
    parameter integer BANKS=16, BANK_BITS=$clog2(BANKS), ROW_BITS=15-BANK_BITS
) (
    input wire clk,reset,
    input wire [31:0] response_latency,
    input wire [8:0] column_limit,
    input wire command_valid,command_allocate,
    input wire [5:0] command_cta,
    input wire [31:0] command_base,command_count,
    input wire [4:0] held_base,
    input wire [31:0] held_pages,
    input wire [63:0] retire_ctas,
    output wire command_accepted,
    output wire [4:0] grant_base,
    output wire [31:0] grant_pages,
    output wire [8:0] used_columns,freed_columns,reclaimed_columns,
    input wire [5:0] coordinate_cta,
    input wire [31:0] coordinate_row,coordinate_column,
    output wire [16:0] coordinate_address,
    output wire [1:0] coordinate_error,
    input wire request_valid,request_write,
    input wire [2:0] request_index,
    input wire [5:0] request_cta,
    input wire [31:0] request_mask,
    input wire [479:0] request_words,
    input wire [1023:0] request_values,
    output wire request_ready,
    output wire [2:0] request_slot,
    input wire [7:0] acknowledge,
    input wire [2:0] query_slot,
    output wire [1023:0] query_data,
    output wire [7:0] occupied,completed,
    output wire [BANKS-1:0] bank_accepted,bank_writes,
    output wire [BANKS*6-1:0] write_owners,
    output wire [BANKS*15-1:0] write_words,
    output wire [BANKS*32-1:0] write_values,served_lanes,
    output wire [255:0] blocked_lanes,
    output wire [63:0] busy_ctas,
    output wire clock_enable,
    output reg [3:0] error
);
    reg [7:0] allocated,writes;
    reg [63:0] older;
    reg [5:0] owners [0:7];
    reg [479:0] physical_words [0:7],logical_words [0:7];
    reg [1023:0] values [0:7];
    reg [31:0] todo [0:7],pending [0:7],delay [0:7];
    wire [31:0] granted [0:7],returned [0:7];
    wire [1023:0] results [0:7];
    wire [3839:0] all_addresses;
    wire [255:0] all_remaining;
    wire [BANKS-1:0] bank_returns;
    wire [2:0] return_slots [0:BANKS-1];
    wire [BANKS*32-1:0] return_lanes,return_values;
    wire [BANKS*256-1:0] all_services;
    wire [14:0] word_addresses [0:7][0:31],logical_addresses [0:7][0:31];
    wire [31:0] page_live,missing;
    wire [191:0] page_owners;
    wire [159:0] page_logical;
    wire [479:0] translated;
    wire [3:0] mapping_error;
    reg free_found;
    reg [2:0] free_slot;
    integer at;
    always @* begin
        free_found=0; free_slot=0;
        for (at=0; at<8; at=at+1)
            if (!free_found && (!allocated[at] || acknowledge[at])) begin free_found=1; free_slot=3'(at); end
        error=mapping_error;
        if (error==0 && response_latency==0) error=1;
        else if (error==0 && (acknowledge & ~completed)!=0) error=7;
        else if (error==0 && request_valid) begin
            if (allocated[request_index] && !acknowledge[request_index]) error=8;
            else if (|missing) error=9;
            else if (retire_ctas[request_cta] || (command_valid && !command_allocate && command_cta==request_cta)) error=10;
        end
    end
    wire enabled=!reset && error==0;
    assign request_ready=enabled && free_found;
    assign request_slot=request_valid ? request_index : free_slot;
    wire accepted=request_valid && request_ready;
    assign occupied=allocated;
    assign query_data=completed[query_slot] ? results[query_slot] : 1024'b0;
    assign clock_enable=reset || allocated!=0 || bank_returns!=0 || request_valid || acknowledge!=0 || command_valid || retire_ctas!=0;
    tmem_mapping mapping(.clk(clk),.reset(reset),.enable(enabled),.column_limit(column_limit),
        .command_valid(command_valid),.command_allocate(command_allocate),.command_cta(command_cta),
        .command_base(command_base),.command_count(command_count),.held_base(held_base),.held_pages(held_pages),
        .retire_ctas(retire_ctas),.busy_ctas(busy_ctas),.accepted(command_accepted),.grant_base(grant_base),.grant_pages(grant_pages),
        .page_live(page_live),.page_owners(page_owners),.page_logical(page_logical),.used_columns(used_columns),
        .freed_columns(freed_columns),.reclaimed_columns(reclaimed_columns),.query_cta(coordinate_cta),
        .query_row(coordinate_row),.query_column(coordinate_column),.query_address(coordinate_address),
        .query_error(coordinate_error),.error(mapping_error));
    tmem_translate translation(.page_live(page_live),.mask(request_mask),.page_owners(page_owners),
        .page_logical(page_logical),.owner(request_cta),.logical_words(request_words),.physical_words(translated),.missing(missing));

    genvar bank,slot,lane,cta;
    generate for (bank=0; bank<BANKS; bank=bank+1) begin: banks
        wire found;
        wire [2:0] chosen_slot;
        wire [4:0] chosen_lane;
        wire [31:0] lanes;
        wire [14:0] address=word_addresses[chosen_slot][chosen_lane];
        wire [31:0] value=values[chosen_slot][32'(chosen_lane)*32+:32];
        wire issued=enabled && found;
        tmem_select #(.BANK(bank),.BANKS(BANKS)) select_source(
            .allocated(allocated),.writes(writes),.older(older),.remaining(all_remaining),
            .addresses(all_addresses),.found(found),.selected_slot(chosen_slot),.selected_lane(chosen_lane),.selected_lanes(lanes));
        assign bank_accepted[bank]=issued;
        assign served_lanes[bank*32+:32]=issued ? lanes : 32'b0;
        assign bank_writes[bank]=issued && writes[chosen_slot];
        assign write_owners[bank*6+:6]=owners[chosen_slot];
        assign write_words[bank*15+:15]=logical_addresses[chosen_slot][chosen_lane];
        assign write_values[bank*32+:32]=value;
        tmem_bank #(.ROWS(32768/BANKS),.ROW_BITS(ROW_BITS)) storage(.clk(clk),.reset(reset),.enable(enabled),
            .request_valid(found),.request_write(writes[chosen_slot]),.request_row(ROW_BITS'(address>>BANK_BITS)),
            .request_value(value),.request_lanes(lanes),.request_slot(chosen_slot),
            .response_valid(bank_returns[bank]),.response_slot(return_slots[bank]),
            .response_lanes(return_lanes[bank*32+:32]),.response_value(return_values[bank*32+:32]));
        for (slot=0; slot<8; slot=slot+1) begin: slots
            assign all_services[(slot*BANKS+bank)*32+:32]=issued && chosen_slot==3'(slot) ? lanes : 32'b0;
        end
    end
    for (cta=0; cta<64; cta=cta+1) begin: busy_owners
        wire [7:0] owned;
        for (slot=0; slot<8; slot=slot+1) begin: requests
            assign owned[slot]=allocated[slot] && !acknowledge[slot] && owners[slot]==6'(cta);
        end
        assign busy_ctas[cta]=|owned;
    end
    for (slot=0; slot<8; slot=slot+1) begin: slots
        reg [31:0] finished,services;
        integer b;
        always @* begin
            finished=0; services=0;
            for (b=0; b<BANKS; b=b+1) begin
                if (bank_returns[b] && return_slots[b]==3'(slot)) finished=finished | return_lanes[b*32+:32];
                services=services | all_services[(slot*BANKS+b)*32+:32];
            end
        end
        assign granted[slot]=services;
        assign returned[slot]=finished;
        assign all_remaining[slot*32+:32]=todo[slot];
        assign all_addresses[slot*480+:480]=physical_words[slot];
        assign blocked_lanes[slot*32+:32]=allocated[slot] && enabled ? todo[slot] & ~granted[slot] : 32'b0;
        assign completed[slot]=allocated[slot] && pending[slot]==0 && delay[slot]==0;
        always @(posedge clk) begin
            if (reset) begin allocated[slot]<=0; writes[slot]<=0; owners[slot]<=0; todo[slot]<=0; pending[slot]<=0; delay[slot]<=0; end
            else if (enabled) begin
                if (delay[slot]!=0) delay[slot]<=delay[slot]-1'b1;
                if (allocated[slot]) begin
                    todo[slot]<=todo[slot] & ~granted[slot]; pending[slot]<=pending[slot] & ~returned[slot];
                    if (pending[slot]!=0 && (pending[slot] & ~returned[slot])==0) delay[slot]<=response_latency-1'b1;
                end
                if (acknowledge[slot]) allocated[slot]<=0;
                if (accepted && request_slot==3'(slot)) begin
                    allocated[slot]<=1; writes[slot]<=request_write; owners[slot]<=request_cta;
                    physical_words[slot]<=translated; logical_words[slot]<=request_words; values[slot]<=request_values;
                    todo[slot]<=request_mask; pending[slot]<=request_mask; delay[slot]<=request_mask==0 ? response_latency-1'b1 : 32'b0;
                end
            end
        end
        for (lane=0; lane<32; lane=lane+1) begin: lanes
            assign word_addresses[slot][lane]=physical_words[slot][lane*15+:15];
            assign logical_addresses[slot][lane]=logical_words[slot][lane*15+:15];
            reg [31:0] result;
            wire [6:0] response_bank=7'(32'(word_addresses[slot][lane])%BANKS);
            assign results[slot][lane*32+:32]=result;
            always @(posedge clk) begin
                if (enabled) begin
                    if (returned[slot][lane]) result<=return_values[32'(response_bank)*32+:32];
                    if (accepted && request_slot==3'(slot)) result<=0;
                end
            end
        end
        for (lane=0; lane<8; lane=lane+1) begin: order
            always @(posedge clk) begin
                if (reset) older[slot*8+lane]<=0;
                else if (enabled && accepted) begin
                    if (request_slot==3'(slot)) older[slot*8+lane]<=0;
                    else if (request_slot==3'(lane)) older[slot*8+lane]<=allocated[slot] && !acknowledge[slot];
                end
            end
        end
    end endgenerate
endmodule
