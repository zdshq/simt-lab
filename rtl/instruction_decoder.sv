// Raw machine words drive both execution metadata and bounded operand requests.
// 原始机器字同时驱动执行元数据与有界操作数请求。
module instruction_decoder (
    input wire [127:0] words,
    input wire [31:0] participating,
    output wire [7:0] opcode, rd, ra, rb, rc, aux, mode,
    output wire [3:0] guard,
    output wire [31:0] imm,
    output wire [20:0] control,
    output wire [1:0] encoding_error,
    output wire [2:0] pipe, operand_error,
    output wire boundary, store, destination_valid, predicate_valid, source_predicate_valid,
    output wire [5:0] destination,
    output wire [2:0] predicate, source_predicate,
    output wire [31:0] write_mask,
    output wire [1:0] source_count,
    output wire [17:0] source_registers,
    output wire [95:0] source_masks
);
    instruction_encoding encoding (
        .words(words), .opcode(opcode), .rd(rd), .ra(ra), .rb(rb), .rc(rc), .guard(guard), .aux(aux), .mode(mode),
        .imm(imm), .control(control), .encoding_error(encoding_error)
    );
    instruction_operands operands (
        .opcode(opcode), .rd(rd), .ra(ra), .rb(rb), .rc(rc), .imm(imm), .participating(participating),
        .pipe(pipe), .boundary(boundary), .store(store), .destination_valid(destination_valid),
        .predicate_valid(predicate_valid), .source_predicate_valid(source_predicate_valid), .destination(destination),
        .predicate(predicate), .source_predicate(source_predicate), .write_mask(write_mask), .source_count(source_count),
        .source_registers(source_registers), .source_masks(source_masks), .operand_error(operand_error)
    );
endmodule
