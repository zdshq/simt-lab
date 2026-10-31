// The CP owns queue pointers, sixteen decoded packets, dependency masks and engine occupancy.
// CP 持有队列指针、十六个已解码包、依赖掩码和引擎占用状态。
module command_processor (
    input wire clk, reset,
    input wire [12:0] capacity,
    input wire mmio_valid, mmio_write,
    input wire [2:0] mmio_address,
    input wire [31:0] mmio_wdata,
    output reg [31:0] mmio_rdata,
    input wire fetch_accept,
    input wire [31:0] fetch_handle,
    input wire [1:0] fetch_kind,
    input wire [15:0] fetch_wait,
    output wire fetch_valid,
    output wire [3:0] fetch_slot,
    output wire [31:0] fetch_index,
    input wire [31:0] lookup_handle,
    output wire [15:0] lookup_mask,
    input wire [15:0] external_pending, external_failed,
    input wire node_valid, node_cancel,
    input wire [1:0] node_kind,
    input wire [31:0] node_handle,
    output wire dispatch_valid, dispatch_node, dispatch_cancel,
    output wire [3:0] dispatch_slot,
    input wire [15:0] graph_pending,
    output wire graph_valid,
    output wire [3:0] graph_slot,
    input wire [15:0] complete_mask, failed_mask,
    input wire compute_release, copy_release,
    output reg compute_busy, copy_busy,
    output reg [15:0] slot_valid,
    output reg [15:0] slot_running,
    output reg [3:0] error
);
    localparam [1:0] COMPUTE=0, COPY=1, NO_ENGINE=2;
    localparam [2:0] REG_PUT=0, REG_GET=1, REG_NOTIFIED=2, REG_STATUS=3, REG_CAPACITY=4;
    reg [31:0] put, get, notified;
    reg [15:0] waits [0:15];
    reg [15:0] failed;
    reg [1:0] kinds [0:15];
    wire [511:0] handles;
    wire [15:0] eligible, cancelling, slot_hits;
    wire doorbell=mmio_valid && mmio_write && mmio_address==REG_PUT;
    wire [31:0] published=doorbell ? mmio_wdata : put;
    wire slot_found, oldest_graph_found;
    wire [3:0] oldest_slot;
    reg free_found;
    reg [3:0] free_slot;
    reg [4:0] occupancy, completions;
    integer at;

    // Empty slots are found combinationally; a release becomes visible after its clock edge.
    // 组合查找空槽；释放在时钟边沿之后才可见。
    always @* begin
        free_found=0; free_slot=0; occupancy=0; completions=0;
        for (at=0; at<16; at=at+1) begin
            if (!slot_valid[at] && !free_found) begin free_found=1; free_slot=4'(at); end
            occupancy=occupancy+{4'b0,slot_valid[at]};
            completions=completions+{4'b0,complete_mask[at]};
        end
    end
    assign fetch_valid=get!=published && free_found;
    assign fetch_slot=free_slot;
    assign fetch_index=get;
    assign lookup_mask=slot_hits;

    cp_oldest select_slot(.candidates(eligible),.handles(handles),.found(slot_found),.slot(oldest_slot));
    // A packet fetched this clock is the youngest packet, so it competes only when no decoded slot is eligible.
    // 本拍取到的包是最年轻的包，因此只有没有合格解码槽时才参与竞争。
    wire fetch_engine_free=fetch_kind==NO_ENGINE || (fetch_kind==COMPUTE ? !compute_busy : !copy_busy);
    wire fetch_cancel=fetch_accept && external_failed[fetch_slot];
    wire fetch_ready=fetch_cancel || (fetch_accept && fetch_wait==0 && !external_pending[fetch_slot] && fetch_engine_free);
    wire packet_found=slot_found || fetch_ready;
    wire [31:0] packet_handle=slot_found ? handles[32'(oldest_slot)*32+:32] : fetch_handle;
    wire [3:0] packet_slot=slot_found ? oldest_slot : fetch_slot;
    assign dispatch_node=node_valid && (!packet_found || node_handle<packet_handle);
    assign dispatch_valid=node_valid || packet_found;
    assign dispatch_slot=packet_slot;
    assign dispatch_cancel=dispatch_node ? node_cancel : slot_found ? cancelling[oldest_slot] : fetch_cancel;
    wire [1:0] dispatch_kind=dispatch_node ? node_kind : slot_found ? kinds[oldest_slot] : fetch_kind;
    wire claims=dispatch_valid && !dispatch_cancel;
    wire claim_compute=claims && dispatch_kind==COMPUTE;
    wire claim_copy=claims && dispatch_kind==COPY;
    wire [15:0] slot_started=dispatch_valid && !dispatch_node ? 16'b1<<packet_slot : 16'b0;
    wire [15:0] slot_fetching=fetch_accept ? 16'b1<<fetch_slot : 16'b0;
    cp_oldest select_graph(.candidates(graph_pending),.handles(handles),.found(oldest_graph_found),.slot(graph_slot));
    assign graph_valid=oldest_graph_found;

    // Every input batch is validated before any state changes, so a rejected batch has no partial effect.
    // 所有输入批次先检查再改变状态，因此被拒绝的批次不会产生部分效果。
    always @* begin
        error=0;
        if (capacity==0 || capacity>4096) error=1;
        else if (doorbell && (mmio_wdata-put>32'(capacity) || mmio_wdata-get>32'(capacity))) error=2;
        else if (mmio_valid && mmio_write && mmio_address!=REG_PUT) error=2;
        else if (fetch_accept && (!fetch_valid || fetch_kind>NO_ENGINE || (fetch_wait & ~slot_valid)!=0)) error=3;
        else if ((complete_mask & ~slot_valid & ~slot_fetching)!=0 || (failed_mask & ~complete_mask)!=0) error=4;
        else if ((complete_mask & ~slot_running & ~slot_started)!=0) error=4;
        else if ((compute_release && !compute_busy && !claim_compute) || (copy_release && !copy_busy && !claim_copy)) error=5;
        else if (node_valid && (node_kind>NO_ENGINE || (!node_cancel &&
                 ((node_kind==COMPUTE && compute_busy) || (node_kind==COPY && copy_busy))))) error=6;
        else if ((graph_pending & ~slot_running)!=0) error=7;
    end
    wire enabled=!reset && error==0;

    always @* begin
        case (mmio_address)
            REG_PUT: mmio_rdata=published;
            REG_GET: mmio_rdata=get;
            REG_NOTIFIED: mmio_rdata=notified;
            REG_STATUS: mmio_rdata={copy_busy,compute_busy,25'b0,occupancy};
            REG_CAPACITY: mmio_rdata={19'b0,capacity};
            default: mmio_rdata=0;
        endcase
    end

    genvar slot;
    generate for (slot=0; slot<16; slot=slot+1) begin: slots
        reg [31:0] handle;
        wire engine_free=kinds[slot]==NO_ENGINE || (kinds[slot]==COMPUTE ? !compute_busy : !copy_busy);
        wire queued=slot_valid[slot] && !slot_running[slot];
        wire dependencies_done=waits[slot]==0 && !external_pending[slot];
        assign handles[slot*32+:32]=handle;
        assign slot_hits[slot]=slot_valid[slot] && handle==lookup_handle;
        assign cancelling[slot]=failed[slot] || external_failed[slot];
        assign eligible[slot]=queued && (cancelling[slot] || (dependencies_done && engine_free));
        wire fetched=fetch_accept && fetch_slot==4'(slot);
        always @(posedge clk) begin
            if (reset) begin slot_valid[slot]<=0; slot_running[slot]<=0; failed[slot]<=0; waits[slot]<=0; kinds[slot]<=0; handle<=0; end
            else if (enabled) begin
                waits[slot]<=waits[slot] & ~complete_mask;
                if ((waits[slot] & failed_mask)!=0) failed[slot]<=1;
                if (slot_started[slot]) slot_running[slot]<=1;
                if (complete_mask[slot]) begin slot_valid[slot]<=0; slot_running[slot]<=0; failed[slot]<=0; waits[slot]<=0; end
                if (fetched) begin
                    slot_valid[slot]<=!complete_mask[slot]; slot_running[slot]<=slot_started[slot] && !complete_mask[slot];
                    handle<=fetch_handle; kinds[slot]<=fetch_kind;
                    waits[slot]<=fetch_wait & ~complete_mask; failed[slot]<=(fetch_wait & failed_mask)!=0;
                end
            end
        end
    end endgenerate

    always @(posedge clk) begin
        if (reset) begin put<=0; get<=0; notified<=0; compute_busy<=0; copy_busy<=0; end
        else if (enabled) begin
            if (doorbell) put<=mmio_wdata;
            if (fetch_accept) get<=get+1'b1;
            notified<=notified+32'(completions);
            compute_busy<=(compute_busy || claim_compute) && !compute_release;
            copy_busy<=(copy_busy || claim_copy) && !copy_release;
        end
    end
endmodule
