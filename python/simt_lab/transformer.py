# Compose a complete small transformer block from memory, SIMT, and matrix instructions.
# 使用内存、SIMT 和矩阵指令组合完整的小型 Transformer block。
from .tensor import Image, pack_values
from .workloads import Workload
from .reference import transformer as scalar_transformer
from .kernels import (start_kernel, stage_inputs, layer_norm, shared_matrix,
                      transpose, softmax, residual, relu, publish)


# Deterministic binary fractions keep host input quantization unambiguous.
# 确定的二进制分数使宿主输入量化没有歧义。
def matrix(rows, columns, salt):
    return [[(((r*columns+c)*7+salt)%17-8)/32 for c in range(columns)] for r in range(rows)]


# Keep host reference tensors separate from device addresses in the staged image.
# 将宿主参考张量与搬运镜像中的设备地址分开。
def inputs():
    image = Image()
    x = [[((r*8+c)*3%11-5)/4 for c in range(8)] for r in range(4)]
    weights = {name:matrix(rows,cols,salt) for name,rows,cols,salt in
               [('q',8,8,1),('k',8,8,3),('v',8,8,5),('o',8,8,7),('up',8,16,9),('down',16,8,11)]}
    tensors = {'x':x,**weights}
    addresses = {}
    for name,tensor in tensors.items():
        addresses[name] = 256+image.append(pack_values((v for row in tensor for v in row),0))
    return image,addresses,x,weights


# Q, K, V and attention output all pass through the same descriptor-driven MMA path.
# Q、K、V 和注意力输出都经过同一条描述符驱动 MMA 路径。
def attention(p, addresses):
    layer_norm(p,addresses['x'],4096)
    for name,destination in [('q',4352),('k',4608),('v',4864)]:
        shared_matrix(p,4096,addresses[name],destination,4,8,8)
    transpose(p,4608,5120)
    shared_matrix(p,4352,5120,5376,4,4,8)
    softmax(p,5376,5632)
    shared_matrix(p,5632,4864,5888,4,8,4)
    shared_matrix(p,5888,addresses['o'],6144,4,8,8)
    residual(p,addresses['x'],6144,6400)


# The feed-forward network includes its own normalization and second residual path.
# 前馈网络包含自身归一化和第二条残差路径。
def feed_forward(p, addresses):
    layer_norm(p,6400,6656)
    shared_matrix(p,6656,addresses['up'],6912,4,16,8)
    relu(p,6912)
    shared_matrix(p,6912,addresses['down'],7168,4,8,16)
    residual(p,6400,7168,7424)


# The reference computes mathematics; the executable computes every intermediate tensor.
# 参考端计算数学结果，可执行程序计算全部中间张量。
def tiny_transformer():
    image,addresses,x,weights = inputs()
    input_size = len(image.data)
    output = image.append(bytes(4*8*4))
    p = start_kernel().emit('TMEM_ALLOC',rd=2,imm=32,guard=0)
    stage_inputs(p,input_size)
    attention(p,addresses)
    feed_forward(p,addresses)
    publish(p,7424,output)
    p.emit('TMEM_FREE',ra=2,imm=32,guard=0).emit('EXIT')
    expected = [value for row in scalar_transformer(x,weights) for value in row]
    return Workload('transformer',p,image,output,expected,1,64,atol=3e-5,rtol=3e-5)
