// Thirty-two physical eight-column pages are shared by sixty-four CTA identities.
// 三十二个物理八列页由六十四个 CTA 身份共享。
module tmem_mapping (
    input wire clk, reset, enable,
    input wire [8:0] column_limit,
    input wire command_valid, command_allocate,
    input wire [5:0] command_cta,
    input wire [31:0] command_base, command_count,
    input wire [4:0] held_base,
    input wire [31:0] held_pages,
    input wire [63:0] retire_ctas, busy_ctas,
    output wire accepted,
    output wire [4:0] grant_base,
    output wire [31:0] grant_pages,
    output wire [31:0] page_live,
    output wire [191:0] page_owners,
    output wire [159:0] page_logical,
    output wire [8:0] used_columns,
    output wire [8:0] freed_columns, reclaimed_columns,
    input wire [5:0] query_cta,
    input wire [31:0] query_row, query_column,
    output wire [16:0] query_address,
    output wire [1:0] query_error,
    output reg [3:0] error
);
    reg [31:0] live;
    reg [5:0] owners [0:31], lengths [0:31];
    reg [4:0] logical_pages [0:31], heads [0:31];
    wire [31:0] retiring, freeing;
    wire [31:0] logical_owned, query_owned;
    wire [5:0] need=6'(command_count>>3);
    reg [5:0] free_count,used_count,reclaim_count,free_size,selected_count;
    reg [31:0] free_pages,chosen_pages;
    reg [4:0] chosen_base;
    reg found;
    integer page,base,offset;
    reg range_free;
    always @* begin
        free_count=0; used_count=0; reclaim_count=0; free_size=0; selected_count=0; free_pages=0; chosen_pages=0;
        for (page=0; page<32; page=page+1) begin
            used_count=used_count+{5'b0,live[page]}; reclaim_count=reclaim_count+{5'b0,retiring[page]};
            free_size=free_size+{5'b0,freeing[page]}; selected_count=selected_count+{5'b0,held_pages[page]};
            if (9'(page*8)<column_limit && (!live[page] || retiring[page])) begin
                free_pages[page]=1;
                if (free_count<need) chosen_pages[page]=1;
                free_count=free_count+1'b1;
            end
        end
        found=0; chosen_base=0; range_free=0;
        for (base=0; base<32; base=base+1) begin
            range_free=6'(base)+need<=32;
            for (offset=0; offset<32; offset=offset+1)
                if (6'(offset)>=6'(base) && 6'(offset)<6'(base)+need && logical_owned[offset]) range_free=0;
            if (!found && range_free) begin found=1; chosen_base=5'(base); end
        end
    end
    assign grant_base=command_valid && command_allocate ? held_base : chosen_base;
    assign grant_pages=command_valid && command_allocate ? held_pages : chosen_pages;
    assign used_columns={used_count,3'b0};
    assign freed_columns={free_size,3'b0};
    assign reclaimed_columns={reclaim_count,3'b0};
    assign page_live=live;
    wire [31:0] held_range=need==32 ? 32'hffffffff : ((32'b1<<need)-1'b1)<<held_base;
    always @* begin
        error=0;
        if (column_limit==0 || column_limit>256 || column_limit[2:0]!=0 || used_columns>column_limit) error=1;
        else if ((retire_ctas & busy_ctas)!=0) error=5;
        else if (command_valid) begin
            if (retire_ctas[command_cta]) error=5;
            else if (command_allocate) begin
                if (command_count==0 || command_count>256 || command_count[2:0]!=0) error=2;
                else if (!found || free_count<need) error=3;
                else if ({1'b0,held_base}+need>32 || (held_range & logical_owned)!=0 ||
                         selected_count!=need || (held_pages & ~free_pages)!=0) error=6;
            end else if (command_base>=256 || command_base[2:0]!=0 || command_count==0 || command_count>256 ||
                         command_count[2:0]!=0 || free_size!=need) error=4;
            else if (busy_ctas[command_cta]) error=5;
        end
    end
    assign accepted=enable && !reset && command_valid && error==0;
    assign query_address={query_column[7:0],query_row[6:0],2'b0};
    assign query_error=query_row>=128 || query_column>=256 ? 2'd1 : !query_owned[query_column[7:3]] ? 2'd2 : 2'd0;

    genvar slot,column;
    generate for (slot=0; slot<32; slot=slot+1) begin: entries
        assign page_owners[slot*6+:6]=owners[slot];
        assign page_logical[slot*5+:5]=logical_pages[slot];
        assign retiring[slot]=live[slot] && retire_ctas[owners[slot]];
        assign freeing[slot]=live[slot] && owners[slot]==command_cta && heads[slot]==command_base[7:3] &&
            lengths[slot]==need && !command_allocate;
        reg [4:0] rank;
        if (slot==0) begin: first_page
            always @* rank=0;
        end else begin: preceding_pages
            integer previous;
            always @* begin
                rank=0;
                for (previous=0; previous<slot; previous=previous+1) rank=rank+{4'b0,grant_pages[previous]};
            end
        end
        always @(posedge clk) begin
            if (reset) begin live[slot]<=0; owners[slot]<=0; logical_pages[slot]<=0; heads[slot]<=0; lengths[slot]<=0; end
            else if (enable && error==0) begin
                if (retiring[slot] || (accepted && !command_allocate && freeing[slot])) live[slot]<=0;
                if (accepted && command_allocate && grant_pages[slot]) begin
                    live[slot]<=1; owners[slot]<=command_cta; logical_pages[slot]<=grant_base+5'(rank);
                    heads[slot]<=grant_base; lengths[slot]<=need;
                end
            end
        end
    end
    for (column=0; column<32; column=column+1) begin: columns
        wire [31:0] owned,queried;
        for (slot=0; slot<32; slot=slot+1) begin: comparisons
            assign owned[slot]=live[slot] && !retiring[slot] && owners[slot]==command_cta && logical_pages[slot]==5'(column);
            assign queried[slot]=live[slot] && owners[slot]==query_cta && logical_pages[slot]==5'(column);
        end
        assign logical_owned[column]=|owned;
        assign query_owned[column]=|queried;
    end endgenerate
endmodule
