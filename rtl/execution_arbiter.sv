// Eight physical partitions share Tensor/async ports; private arithmetic ports retain interval counters.
// 八个物理分区共享 Tensor/异步端口；私有算术端口保存发射间隔计数。
module execution_arbiter (
    input wire clk, reset,
    input wire [3:0] partition_limit,
    input wire [31:0] integer_interval, floating_interval, special_interval,
    input wire [55:0] requests, accepts,
    input wire [7:0] async_noop,
    input wire async_busy,
    input wire tensor_release,
    input wire [2:0] tensor_release_owner,
    output wire [55:0] grants,
    output wire [55:0] interval_stalls, shared_stalls, arbitration_stalls,
    output wire tensor_busy,
    output reg [2:0] tensor_owner, tensor_cursor, async_cursor,
    output wire [767:0] cooldowns,
    output wire clock_enable,
    output reg [3:0] error
);
    reg held_tensor;
    reg [31:0] remaining [0:23];
    reg [55:0] ready, selected, interval_blocked, shared_blocked;
    reg counters_active, tensor_found, async_found;
    reg [2:0] tensor_choice, async_choice, tensor_at, async_at;
    integer p, pipe, offset;
    assign tensor_busy=held_tensor && !tensor_release;
    always @* begin
        ready=requests; interval_blocked=0; shared_blocked=0; counters_active=0;
        for (pipe=0; pipe<3; pipe=pipe+1) for (p=0; p<8; p=p+1) begin
            if (remaining[pipe*8+p]!=0) begin
                ready[pipe*8+p]=0; interval_blocked[pipe*8+p]=requests[pipe*8+p]; counters_active=1;
            end
        end
        for (p=0; p<8; p=p+1) begin
            if (tensor_busy) begin ready[40+p]=0; shared_blocked[40+p]=requests[40+p]; end
            if (async_busy && !async_noop[p]) begin ready[48+p]=0; shared_blocked[48+p]=requests[48+p]; end
        end
        tensor_found=0; async_found=0; tensor_choice=0; async_choice=0; tensor_at=0; async_at=0;
        for (offset=0; offset<8; offset=offset+1) begin
            tensor_at=tensor_cursor+3'(offset); async_at=async_cursor+3'(offset);
            if (!tensor_found && ready[40+{29'b0,tensor_at}]) begin tensor_found=1; tensor_choice=tensor_at; end
            if (!async_found && ready[48+{29'b0,async_at}] && !async_noop[async_at]) begin async_found=1; async_choice=async_at; end
        end
        selected=ready; selected[47:40]=0; selected[55:48]=ready[55:48] & async_noop;
        if (tensor_found) selected[40+{29'b0,tensor_choice}]=1;
        if (async_found) selected[48+{29'b0,async_choice}]=1;
    end
    integer check_port, check_part;
    always @* begin
        error=0;
        if (partition_limit==0 || partition_limit>8 || integer_interval==0 || floating_interval==0 || special_interval==0) error=1;
        for (check_port=0; check_port<7; check_port=check_port+1)
            for (check_part=0; check_part<8; check_part=check_part+1)
                if (check_part>={28'b0,partition_limit} && requests[check_port*8+check_part]) error=2;
        if ((async_noop & ~requests[55:48])!=0) error=3;
        if (tensor_release && (!held_tensor || tensor_release_owner!=tensor_owner)) error=4;
        if ((accepts & ~selected)!=0) error=5;
    end
    assign grants=(!reset && error==0) ? selected : 56'b0;
    assign interval_stalls=(!reset && error==0) ? interval_blocked : 56'b0;
    assign shared_stalls=(!reset && error==0) ? shared_blocked : 56'b0;
    assign arbitration_stalls=(!reset && error==0) ? ready & ~selected : 56'b0;
    assign clock_enable=reset || counters_active || accepts!=0 || tensor_release;
    integer i;
    always @(posedge clk) begin
        if (reset) begin
            held_tensor<=0; tensor_owner<=0; tensor_cursor<=0; async_cursor<=0;
            for (i=0; i<24; i=i+1) remaining[i]<=0;
        end else if (error==0) begin
            for (i=0; i<24; i=i+1) begin
                if (accepts[i]) begin
                    if (i<8) remaining[i]<=integer_interval-1'b1;
                    else if (i<16) remaining[i]<=floating_interval-1'b1;
                    else remaining[i]<=special_interval-1'b1;
                end else if (remaining[i]!=0) remaining[i]<=remaining[i]-1'b1;
            end
            if (tensor_release) begin held_tensor<=0; tensor_owner<=0; end
            if (tensor_found && accepts[40+{29'b0,tensor_choice}]) begin
                held_tensor<=1; tensor_owner<=tensor_choice; tensor_cursor<=tensor_choice+1'b1;
            end
            if (async_found && accepts[48+{29'b0,async_choice}]) async_cursor<=async_choice+1'b1;
        end
    end
    genvar counter;
    generate for (counter=0; counter<24; counter=counter+1) begin: observation
        assign cooldowns[counter*32+:32]=remaining[counter];
    end endgenerate
endmodule
