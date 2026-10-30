// Eight physical vector slots share 32 single-word banks and retain results until explicit consumption.
// 八个物理向量槽共享 32 个单字 bank，结果保持到显式消费。
module shared_memory (
    input wire clk, reset,
    input wire [3:0] capacity,
    input wire [31:0] response_latency, atomic_latency,
    input wire [511:0] clear_rows,
    input wire request_valid, request_write, request_atomic,
    input wire [2:0] request_index,
    input wire [1:0] request_width,
    input wire [3:0] request_mode,
    input wire [31:0] request_mask,
    input wire [511:0] request_addresses,
    input wire [1023:0] request_values, request_replacements,
    output wire request_ready,
    output wire [2:0] request_slot,
    input wire [7:0] acknowledge,
    input wire [2:0] query_slot,
    output wire [1023:0] query_data,
    output wire [7:0] occupied, completed,
    output wire [31:0] bank_accepted, atomic_started, atomic_completed, bank_writes,
    output wire [287:0] write_rows,
    output wire [127:0] write_masks,
    output wire [1023:0] write_values,
    output wire [1023:0] served_lanes,
    output wire [255:0] blocked_lanes,
    output wire clock_enable,
    output reg [3:0] error
);
    reg [7:0] allocated, writes, atomics;
    reg [63:0] older;
    reg [511:0] addresses [0:7];
    reg [1023:0] values [0:7], replacements [0:7];
    reg [1:0] widths [0:7];
    reg [3:0] modes [0:7];
    reg [31:0] todo [0:7], pending [0:7], delay [0:7];
    wire [31:0] granted [0:7], returned [0:7];
    wire [1023:0] results [0:7];
    wire [3583:0] all_addresses;
    wire [255:0] all_remaining;
    wire [31:0] bank_returns, bank_busy;
    wire [2:0] return_slots [0:31];
    wire [1023:0] return_lanes, return_values;
    wire [8191:0] all_services;
    wire [8:0] address_rows [0:7][0:31];
    wire [1:0] address_offsets [0:7][0:31], request_offsets [0:31];
    reg free_found;
    reg [2:0] free_slot;
    integer at, lane;
    reg [1:0] checked_offset;
    reg [8:0] checked_row;
    always @* begin
        free_found=0; free_slot=0;
        for (at=0; at<8; at=at+1)
            if (!free_found && 4'(at)<capacity && (!allocated[at] || acknowledge[at])) begin free_found=1; free_slot=3'(at); end
        error=0; checked_offset=0; checked_row=0;
        if (capacity==0 || capacity>8 || response_latency==0 || atomic_latency==0) error=1;
        else if ((acknowledge & ~completed)!=0) error=2;
        else if (request_valid && ({1'b0,request_index}>=capacity || (allocated[request_index] && !acknowledge[request_index]) ||
                 request_width==3 || (request_atomic && (!request_write || request_width!=2 || request_mode>9)))) error=3;
        for (lane=0; lane<32; lane=lane+1) begin
            checked_offset=request_offsets[lane];
            if (error==0 && request_valid && request_mask[lane] &&
                ((request_width==1 && checked_offset[0]) || (request_width==2 && checked_offset!=0))) error=4;
        end
        for (at=0; at<8; at=at+1) for (lane=0; lane<32; lane=lane+1) begin
            checked_row=address_rows[at][lane];
            if (error==0 && allocated[at] && pending[at][lane] && clear_rows[checked_row]) error=5;
        end
    end
    wire enabled=!reset && error==0;
    assign request_ready=enabled && free_found;
    assign request_slot=request_valid ? request_index : free_slot;
    wire accepted=request_valid && request_ready;
    assign occupied=allocated;
    assign query_data=completed[query_slot] ? results[query_slot] : 1024'b0;
    assign clock_enable=reset || allocated!=0 || bank_busy!=0 || request_valid || acknowledge!=0 || clear_rows!=0;

    genvar bank, slot, word;
    generate for (bank=0; bank<32; bank=bank+1) begin: banks
        wire found, ready;
        wire [2:0] chosen_slot;
        wire [4:0] chosen_lane;
        wire [31:0] lanes;
        wire [8:0] row=address_rows[chosen_slot][chosen_lane];
        wire [1:0] offset=address_offsets[chosen_slot][chosen_lane];
        wire [31:0] value=values[chosen_slot][32'(chosen_lane)*32+:32];
        wire [3:0] mask=(widths[chosen_slot]==0 ? 4'b0001 : widths[chosen_slot]==1 ? 4'b0011 : 4'b1111)<<offset;
        wire issued=enabled && found && ready;
        shared_select #(.BANK(bank)) select_source(
            .allocated(allocated),.writes(writes),.atomics(atomics),.older(older),.remaining(all_remaining),
            .addresses(all_addresses),.found(found),.selected_slot(chosen_slot),
            .selected_lane(chosen_lane),.selected_lanes(lanes));
        assign bank_accepted[bank]=issued;
        assign atomic_started[bank]=issued && atomics[chosen_slot];
        assign served_lanes[bank*32+:32]=issued ? lanes : 32'b0;
        shared_bank storage(.clk(clk),.reset(reset),.enable(enabled),.clear_rows(clear_rows),
            .request_valid(found),.request_write(writes[chosen_slot]),.request_atomic(atomics[chosen_slot]),
            .request_row(row),.request_bytes(mask),.request_mode(modes[chosen_slot]),
            .request_data(value<<{offset,3'b0}),.request_compare(value),
            .request_replacement(replacements[chosen_slot][32'(chosen_lane)*32+:32]),
            .request_slot(chosen_slot),.request_lanes(lanes),.atomic_latency(atomic_latency),.request_ready(ready),
            .response_valid(bank_returns[bank]),.response_slot(return_slots[bank]),
            .response_lanes(return_lanes[bank*32+:32]),.response_data(return_values[bank*32+:32]),
            .write_event(bank_writes[bank]),.atomic_finish(atomic_completed[bank]),.write_row(write_rows[bank*9+:9]),
            .write_bytes(write_masks[bank*4+:4]),.write_data(write_values[bank*32+:32]),.busy(bank_busy[bank]));
        for (slot=0; slot<8; slot=slot+1) begin: slots
            assign all_services[(slot*32+bank)*32+:32]=issued && chosen_slot==3'(slot) ? lanes : 32'b0;
        end
    end endgenerate

    generate for (slot=0; slot<8; slot=slot+1) begin: slots
        reg [31:0] finished;
        integer b;
        always @* begin
            finished=0;
            for (b=0; b<32; b=b+1) begin
                // The slot identity and mask are transported with each bank response.
                // 槽身份与掩码随每个 bank 响应一起传递。
                if (bank_returns[b] && return_slots[b]==3'(slot)) finished=finished | return_lanes[b*32+:32];
            end
        end
        wire [1023:0] services=all_services[slot*1024+:1024];
        reg [31:0] merged_services;
        integer service_bank;
        always @* begin
            merged_services=0;
            for (service_bank=0; service_bank<32; service_bank=service_bank+1)
                merged_services=merged_services | services[service_bank*32+:32];
        end
        assign granted[slot]=merged_services;
        assign returned[slot]=finished;
        assign all_remaining[slot*32+:32]=todo[slot];
        assign blocked_lanes[slot*32+:32]=allocated[slot] && enabled ? todo[slot] & ~granted[slot] : 32'b0;
        assign completed[slot]=allocated[slot] && pending[slot]==0 && delay[slot]==0;
        always @(posedge clk) begin
            if (reset) begin allocated[slot]<=0; writes[slot]<=0; atomics[slot]<=0; todo[slot]<=0; pending[slot]<=0; delay[slot]<=0; end
            else if (enabled) begin
                if (delay[slot]!=0) delay[slot]<=delay[slot]-1'b1;
                if (allocated[slot]) begin
                    todo[slot]<=todo[slot] & ~granted[slot]; pending[slot]<=pending[slot] & ~returned[slot];
                    if (pending[slot]!=0 && (pending[slot] & ~returned[slot])==0) delay[slot]<=response_latency-1'b1;
                end
                if (acknowledge[slot]) allocated[slot]<=0;
                if (accepted && request_slot==3'(slot)) begin
                    allocated[slot]<=1; writes[slot]<=request_write; atomics[slot]<=request_atomic;
                    addresses[slot]<=request_addresses; values[slot]<=request_values; replacements[slot]<=request_replacements;
                    widths[slot]<=request_width; modes[slot]<=request_mode; todo[slot]<=request_mask; pending[slot]<=request_mask;
                    delay[slot]<=request_mask==0 ? response_latency-1'b1 : 32'b0;
                end
            end
        end
        for (word=0; word<32; word=word+1) begin: lanes
            assign all_addresses[(slot*32+word)*14+:14]=addresses[slot][word*16+2+:14];
            assign address_rows[slot][word]=addresses[slot][word*16+7+:9];
            assign address_offsets[slot][word]=addresses[slot][word*16+:2];
            if (slot==0) assign request_offsets[word]=request_addresses[word*16+:2];
            reg [31:0] result;
            // A lane's address identifies its return bank; extract subwords after selecting that word.
            // lane 地址标识其返回 bank，先选择返回字再提取子字。
            wire [4:0] response_bank=addresses[slot][word*16+2+:5];
            wire [31:0] returned_word=return_values[32'(response_bank)*32+:32]>>{addresses[slot][word*16+:2],3'b0};
            assign results[slot][word*32+:32]=result;
            always @(posedge clk) begin
                if (enabled) begin
                    if (returned[slot][word]) result<=widths[slot]==0 ? returned_word & 32'hff : widths[slot]==1 ? returned_word & 32'hffff : returned_word;
                    if (accepted && request_slot==3'(slot)) result<=0;
                end
            end
        end
        for (word=0; word<8; word=word+1) begin: order
            always @(posedge clk) begin
                if (reset) older[slot*8+word]<=0;
                else if (enabled && accepted) begin
                    if (request_slot==3'(slot)) older[slot*8+word]<=0;
                    else if (request_slot==3'(word)) older[slot*8+word]<=allocated[slot] && !acknowledge[slot];
                end
            end
        end
    end endgenerate
endmodule
