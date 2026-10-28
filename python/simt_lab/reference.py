# Plain scalar mathematical references do not import the ISA or instruction builders.
# 普通标量数学参考不导入 ISA 或指令构造器。
import math


# Matrix dimensions are taken from ordinary nested Python lists.
# 从普通 Python 嵌套列表取得矩阵维度。
def matmul(a, b):
    return [[sum(x*y for x,y in zip(row,column)) for column in zip(*b)] for row in a]


# Center and normalize each row using the same documented epsilon.
# 使用规范中的 epsilon 对每行进行中心化与归一化。
def layer_norm(rows):
    output = []
    for row in rows:
        mean = sum(row)/len(row)
        variance = sum((value-mean)**2 for value in row)/len(row)
        output.append([(value-mean)/math.sqrt(variance+1e-5) for value in row])
    return output


# Stable scalar softmax uses natural exp, independently of the device's exp2 sequence.
# 稳定的标量 softmax 使用自然指数，独立于设备的 exp2 指令序列。
def attention_probabilities(scores, width):
    output = []
    for row in scores:
        scaled = [value/math.sqrt(width) for value in row]
        exponentials = [math.exp(value-max(scaled)) for value in scaled]
        output.append([value/sum(exponentials) for value in exponentials])
    return output


# Add corresponding entries in two equally shaped tensors.
# 将两个形状相同的张量逐项相加。
def add(a, b):
    return [[x+y for x,y in zip(ra,rb)] for ra,rb in zip(a,b)]


# Evaluate a single-head pre-norm block with a ReLU feed-forward network.
# 计算一个使用 ReLU 前馈网络的单头 pre-norm block。
def transformer(x, weights):
    normalized = layer_norm(x)
    q, k, v = [matmul(normalized,weights[name]) for name in ('q','k','v')]
    scores = matmul(q,list(map(list,zip(*k))))
    probabilities = attention_probabilities(scores,len(x[0]))
    context = matmul(probabilities,v)
    hidden = add(x,matmul(context,weights['o']))
    up = matmul(layer_norm(hidden),weights['up'])
    activated = [[max(0,value) for value in row] for row in up]
    return add(hidden,matmul(activated,weights['down']))
