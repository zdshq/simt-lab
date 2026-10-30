// Three source ports and independent destination/predicate ports observe forwarded reservation masks.
// 三个源端口和独立的目标/谓词端口读取旁路后的预约掩码。
module dependency_query #(
    parameter integer LANES = 32
) (
    input wire [64*LANES-1:0] writers, readers,
    input wire [8*LANES-1:0] predicates,
    input wire [17:0] source_registers,
    input wire [3*LANES-1:0] source_masks,
    input wire [5:0] destination,
    input wire [LANES-1:0] write_mask,
    input wire [2:0] predicate, source_predicate,
    input wire [LANES-1:0] predicate_mask, source_predicate_mask,
    output wire [6:0] blocked,
    output wire ready
);
    genvar source;
    generate for (source=0; source<3; source=source+1) begin: sources
        wire [5:0] reg_index=source_registers[source*6+:6];
        wire [LANES-1:0] pending=writers[32'(reg_index)*LANES+:LANES];
        assign blocked[source]=reg_index!=63 && (pending & source_masks[source*LANES+:LANES])!=0;
    end endgenerate
    wire [LANES-1:0] write_pending=writers[32'(destination)*LANES+:LANES];
    wire [LANES-1:0] read_pending=readers[32'(destination)*LANES+:LANES];
    wire [LANES-1:0] predicate_pending=predicates[32'(predicate)*LANES+:LANES];
    wire [LANES-1:0] source_pending=predicates[32'(source_predicate)*LANES+:LANES];
    assign blocked[3]=destination!=63 && (write_pending & write_mask)!=0;
    assign blocked[4]=destination!=63 && (read_pending & write_mask)!=0;
    assign blocked[5]=predicate!=7 && (predicate_pending & predicate_mask)!=0;
    assign blocked[6]=source_predicate!=7 && (source_pending & source_predicate_mask)!=0;
    assign ready=!(|blocked);
endmodule
