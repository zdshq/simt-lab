#!/usr/bin/env python3
# Generate both encoding interfaces from a validated ISA description.
# 从经过验证的 ISA 描述生成两种编码接口。
import argparse
import json
from pathlib import Path


# Reject overlapping fields before generating either language interface.
# 在生成任一语言接口之前拒绝重叠字段。
def validate_fields(fields):
    names, used = set(), [0] * 4
    for field in fields:
        name, word, shift, width = (field[k] for k in ('name', 'word', 'shift', 'bits'))
        if name in names or not name.isidentifier() or word not in range(4):
            raise ValueError('invalid or duplicate field: ' + name)
        if width < 1 or shift < 0 or shift + width > 32:
            raise ValueError('invalid field width: ' + name)
        mask = ((1 << width) - 1) << shift
        if used[word] & mask:
            raise ValueError('overlapping field: ' + name)
        if not 0 <= field['default'] < 1 << width:
            raise ValueError('invalid default: ' + name)
        names.add(name)
        used[word] |= mask
    return names


# Validate opcode uniqueness and instruction-specific operand names.
# 检查操作码唯一性和各条指令的操作数字段。
def validate_opcodes(instructions, field_names):
    names, codes = set(), set()
    available = field_names - {'opcode', 'guard', 'control'}
    for op in instructions:
        if op['name'] in names or not op['name'].isidentifier():
            raise ValueError('duplicate or invalid opcode name')
        if op['code'] in codes or not 0 <= op['code'] < 256:
            raise ValueError('duplicate or invalid opcode')
        if len(op['fields']) != len(set(op['fields'])) or set(op['fields']) - available:
            raise ValueError('invalid operands: ' + op['name'])
        names.add(op['name'])
        codes.add(op['code'])


# Both generated backends consume exactly the same validated schema.
# 两种生成后端使用同一份经过验证的描述。
def validate(schema):
    if schema['word_bytes'] != 16 or schema['version'] != 1:
        raise ValueError('unsupported ISA schema')
    names = validate_fields(schema['fields'])
    validate_opcodes(schema['instructions'], names)
    return schema


# Construct the legal bit mask for one instruction.
# 构造一条指令允许使用的位掩码。
def encoding_masks(fields, op):
    masks = [0] * 4
    allowed = set(op['fields']) | {'opcode', 'guard', 'control'}
    for field in fields:
        if field['name'] in allowed:
            masks[field['word']] |= ((1 << field['bits']) - 1) << field['shift']
    return ', '.join(hex(mask) + 'u' for mask in masks)


# Emit data tables instead of a long generated opcode switch.
# 生成数据表，避免冗长的操作码分支函数。
def cpp_tables(schema):
    fields, ops = schema['fields'], schema['instructions']
    return {
        '@ENUM@': '\n'.join(f'    {o["name"]} = {o["code"]},' for o in ops),
        '@FIELDS@': '\n'.join(f'    uint32_t {f["name"]}{{{f["default"]}u}};' for f in fields if f['name'] != 'opcode'),
        '@TABLE@': '\n'.join(f'    {{Op::{o["name"]}, "{o["name"]}", {{{encoding_masks(fields, o)}}}}},' for o in ops),
        '@DECODE@': '\n'.join(f'    i.{f["name"]} = (words[{f["word"]}] >> {f["shift"]}) & {hex((1 << f["bits"]) - 1)}u;'
                             for f in fields if f['name'] != 'opcode'),
    }


# Keep generated source formatting and comments in reviewable templates.
# 将生成源码的格式和注释保存在便于审查的模板中。
def render(template, replacements):
    source = (Path(__file__).parent / 'templates' / template).read_text()
    for token, content in replacements.items():
        source = source.replace(token, content)
    return source


# Produce deterministic output files in the build directory.
# 在构建目录中生成确定性的输出文件。
def generate(schema, destination):
    destination.mkdir(parents=True, exist_ok=True)
    (destination / 'isa.hpp').write_text(render('isa.hpp.in', cpp_tables(schema)))
    (destination / 'simt_isa.py').write_text(render('isa.py.in', {'@SCHEMA@': repr(schema)}))


# Read generation options without performing work at module import time.
# 读取生成选项，避免导入模块时执行生成工作。
def parse_options():
    parser = argparse.ArgumentParser()
    parser.add_argument('--schema', required=True, type=Path)
    parser.add_argument('--out', required=True, type=Path)
    return parser.parse_args()


# Coordinate loading, validation, and generation.
# 组织读取、验证和生成流程。
def run_application():
    options = parse_options()
    schema = validate(json.loads(options.schema.read_text()))
    generate(schema, options.out)


if __name__ == '__main__':
    run_application()
