// Decode all ISA v1 operand roles, preserving full source fields until range checks finish.
// 解码 ISA v1 全部操作数角色，完成范围检查前保留完整源字段。
module instruction_operands (
    input wire [7:0] opcode, rd, ra, rb, rc,
    input wire [31:0] imm, participating,
    output reg [2:0] pipe,
    output reg boundary, store, destination_valid, predicate_valid, source_predicate_valid,
    output wire [5:0] destination,
    output wire [2:0] predicate, source_predicate,
    output reg [31:0] write_mask,
    output reg [1:0] source_count,
    output reg [17:0] source_registers,
    output reg [95:0] source_masks,
    output reg [2:0] operand_error
);
    reg [2:0] used;
    reg [31:0] masks [0:2];
    wire [7:0] registers [0:2];
    assign registers[0]=ra; assign registers[1]=rb; assign registers[2]=rc;
    assign destination=rd[5:0]; assign predicate=rd[2:0]; assign source_predicate=ra[2:0];
    always @* begin
        pipe=0; boundary=0; store=0; destination_valid=0; predicate_valid=0; source_predicate_valid=0;
        used=0; write_mask=participating; masks[0]=participating; masks[1]=participating; masks[2]=participating;
        if (opcode>=1 && opcode<=23) begin
            predicate_valid=opcode==14 || opcode==15; destination_valid=!predicate_valid;
            if (opcode!=1 && opcode!=3) used[0]=1;
            if (opcode>=4 && opcode<=18) used[1]=1;
            if (opcode==7 || opcode==18) used[2]=1;
            if (opcode>=15) pipe=1;
            if (opcode>=19 && opcode<=21) pipe=2;
        end else if (opcode>=24 && opcode<=33) begin
            pipe=4; boundary=1;
            if (opcode>=30) begin
                predicate_valid=opcode==30; destination_valid=!predicate_valid;
                if (opcode!=33) write_mask=participating & imm;
                if (opcode==31) source_predicate_valid=1;
                if (opcode==32) begin used=3'b011; masks[0]=participating | imm; end
            end
        end else if (opcode>=34 && opcode<=42) begin
            pipe=3; used[0]=1; store=opcode==35 || opcode==37 || opcode==39 || opcode==42;
            if (store) used[1]=1; else destination_valid=1;
        end else if (opcode==43) begin used[0]=1; destination_valid=1;
        end else if (opcode==44 || opcode==45) begin
            pipe=3; store=1; used=3'b111; destination_valid=opcode==44;
        end else if (opcode>=46 && opcode<=48) begin pipe=4; boundary=1;
        end else if (opcode>=49 && opcode<=53) begin
            pipe=5; boundary=1; destination_valid=opcode==49 || opcode==51;
            if (opcode!=49) used[0]=1;
            if (opcode==51 || opcode==52) used[1]=1;
            if (opcode==52) used[2]=1;
        end else if (opcode>=54 && opcode<=58) begin
            pipe=6; boundary=1; used[0]=1; destination_valid=opcode==56;
            if (opcode==57 || opcode==58) used[1]=1;
            if (opcode==58) used[2]=1;
        end
    end

    // Three fixed source slots are compacted in operand order; equal registers merge lane masks.
    // 三个固定源槽按操作数顺序压紧；相同寄存器合并 lane 掩码。
    reg [5:0] compact_registers [0:2];
    reg [31:0] compact_masks [0:2];
    reg found, invalid_source;
    integer source, previous, output_slot;
    always @* begin
        source_count=0; source_registers=0; source_masks=0; found=0; invalid_source=0; operand_error=0;
        for (output_slot=0; output_slot<3; output_slot=output_slot+1) begin compact_registers[output_slot]=0; compact_masks[output_slot]=0; end
        for (source=0; source<3; source=source+1) begin
            found=0;
            if (used[source] && masks[source]!=0 && registers[source]!=63) begin
                if (registers[source]>=64) invalid_source=1;
                for (previous=0; previous<3; previous=previous+1)
                    if (previous<{30'b0,source_count} && compact_registers[previous]==registers[source][5:0]) begin
                        compact_masks[previous]=compact_masks[previous] | masks[source]; found=1;
                    end
                if (!found) begin
                    compact_registers[source_count]=registers[source][5:0]; compact_masks[source_count]=masks[source];
                    source_count=source_count+1'b1;
                end
            end
        end
        for (output_slot=0; output_slot<3; output_slot=output_slot+1) begin
            source_registers[output_slot*6+:6]=compact_registers[output_slot]; source_masks[output_slot*32+:32]=compact_masks[output_slot];
        end
        if (opcode>58) operand_error=1;
        else if (invalid_source || (destination_valid && rd>=64)) operand_error=2;
        else if (predicate_valid && rd>=7) operand_error=3;
        else if (source_predicate_valid && ra>=8) operand_error=3;
    end
endmodule
