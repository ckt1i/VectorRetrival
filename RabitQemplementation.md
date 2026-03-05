下面给你一份**面向工程实现的 RabitQ 编码与计算文档**，重点针对：

* **单 bit RabitQ（原始算法）**
* **多 bit RabitQ（扩展版本）**
* **代码实现方式（SIMD / bit packing / distance computation）**
* **如何设计代码结构支持 M-bit 扩展**

这份文档会偏**系统实现 + 数学原理 + 数据结构**，适合直接指导你在 repo 中实现。

文中部分理论来自论文

*  *Practical and Asymptotically Optimal Quantization of High-Dimensional Vectors in Euclidean Space for ANN*

该论文提出 RabitQ 的理论基础，并给出了多 bit 扩展方法。

---

# 1 RabitQ 的核心思想

RabitQ 的目标是：

> **将一个 L 维浮点向量压缩为 L×M bit 的量化向量，同时保持内积估计的无偏性和渐近最优误差界。**

RabitQ 的核心思想：

**利用随机旋转 + 二值/整数编码来估计向量内积。**

ANN 中距离计算：

[
||o - q||^2
]

通过展开：

[
||o-q||^2
=========

||o-c||^2
+
||q-c||^2
---------

2||o-c||\cdot||q-c||\cdot \langle o,q \rangle
]

因此只需要估计

[
\langle o,q \rangle
]

即可。 

RabitQ 的目标就是：

> **快速估计 normalized vectors 的 inner product**

---

# 2 RabitQ 整体流程

整体流程：

```
vector -> normalize -> random rotation -> quantize -> bitcode
```

查询：

```
query -> normalize -> same rotation -> compute approximate inner product
```

流程图：

```
Raw Vector
      |
Normalize (center c)
      |
Random Rotation P
      |
Quantization
      |
Binary / M-bit code
```

---

# 3 单 bit RabitQ（原始算法）

这是 RabitQ 最经典版本。

### 压缩率

```
float vector: L * 32 bits
RabitQ:       L * 1 bit

compression ≈ 32x
```

---

# 3.1 Codebook 构造

原始 RabitQ codebook：

[
C = {-1/\sqrt{L}, +1/\sqrt{L}}^L
]

也就是：

**单位超立方体顶点**

例如：

```
L=4

(+1,+1,+1,+1)/sqrt(4)
(+1,+1,+1,-1)/sqrt(4)
...
```

然后进行随机旋转：

[
C' = P C
]

其中

```
P = random orthogonal matrix
```

论文指出：

> codebook 只需要存储 P 即可 

---

# 3.2 编码过程

输入：

```
o ∈ R^L
```

步骤：

### Step1 normalize

[
o = (o - c) / ||o - c||
]

---

### Step2 random rotation

[
o' = P^{-1} o
]

注意：

```
查询时只需计算 P^{-1}q
```

---

### Step3 sign quantization

每一维：

```
if o'[i] >= 0 → bit = 1
else → bit = 0
```

最终得到

```
x ∈ {0,1}^L
```

---

# 3.3 数据结构

建议存储为：

```
uint64_t code[L/64]
```

例如：

```
L=128

2 uint64_t
```

或

```
uint8_t code[L]
```

---

# 3.4 距离计算

查询时计算：

[
\langle q, \hat{o} \rangle
]

论文推导为：

[
\langle q, \hat{o} \rangle
==========================

## \frac{2}{\sqrt{L}} (q' · x)

\frac{1}{\sqrt{L}} \sum q'
]

其中：

```
q' = P^{-1}q
```

所以只需计算

```
dot(q', x)
```

---

# 3.5 SIMD 实现

核心是：

```
dot(q', x)
```

因为

```
x ∈ {0,1}
```

可转换为

```
x ∈ {-1,1}
```

实现：

```
if bit=1 → +q'
if bit=0 → -q'
```

SIMD 实现：

```
sum += mask ? q[i] : -q[i]
```

或者

```
sum += q[i] * (2*bit-1)
```

---

# 3.6 Bit trick

可以用 popcount 计算：

```
dot(q', x)
```

步骤：

```
1 XOR query_sign_mask
2 popcount
```

FastScan 技术就是这样实现的。

---

# 4 多 bit RabitQ（扩展）

原始 RabitQ：

```
1 bit / dimension
```

扩展：

```
M bit / dimension
```

总长度：

```
L * M bits
```

压缩率：

```
32 / M
```

例如：

| M | compression |
| - | ----------- |
| 1 | 32x         |
| 2 | 16x         |
| 4 | 8x          |
| 8 | 4x          |

论文提出该扩展方法。 

---

# 4.1 Codebook 构造

每一维：

```
x[i] ∈ {0 .. 2^M-1}
```

然后

### Step1 shift

```
x = x - mean
```

### Step2 normalize

```
x = x / ||x||
```

### Step3 random rotate

```
Px
```

这样生成 codebook。

---

# 4.2 编码

给定

```
o' = P^{-1} o
```

每一维量化：

```
x[i] = quantize(o'[i])
```

例如：

```
range [-1,1]
```

量化为

```
2^M bins
```

---

# 4.3 存储

存储为

```
uintM_t x[L]
```

例如：

```
M=4 → uint4
```

通常打包：

```
uint8_t code[L]
```

或

```
bit packed
```

---

# 4.4 距离计算

内积估计：

[
\langle q, \hat{o} \rangle
≈
\sum q'[i] * w[x[i]]
]

其中：

```
w[x[i]] = lookup table
```

即

```
LUT[2^M]
```

---

# 5 MSB 优化（关键）

论文提出：

**最高位等价于 1-bit RabitQ**。

也就是说：

```
MSB(x[i])
=
sign bit
```

所以可以：

```
Stage1: MSB distance
Stage2: full M-bit distance
```

好处：

```
先剪枝
再精算
```

这是 RabitQ + IVF 中非常关键优化。 

---

# 6 推荐代码结构

建议模块：

```
quant/
  rabitq/
    rabitq_encoder.h
    rabitq_encoder.cpp
    rabitq_distance.h
    rabitq_distance.cpp
    rabitq_rotation.h
```

---

## Encoder

```
class RabitQEncoder
{
public:

    int dim;
    int bits;

    Matrix rotation;

    encode(vector<float> v)

}
```

---

## Distance

```
class RabitQDistance
{
public:

    compute(query, code)

}
```

---

# 7 SIMD 实现建议

### 单 bit

使用

```
AVX2 popcnt
```

或

```
AVX512 vpternlog
```

---

### 多 bit

使用

```
LUT gather
```

或

```
SIMD dot
```

---

# 8 与 IVF 结合

你方案：

```
IVF + RabitQ + ConANN
```

RabitQ 用于：

```
stage1 approximate distance
```

流程：

```
scan codes
compute approx dist
classify safe-in/out
prefetch
```

---

# 9 推荐默认参数

建议：

```
M = 1
```

先实现单 bit。

再扩展：

```
M = 2
M = 4
```

---

# 10 代码实现建议

建议模板：

```
template<int BITS>
class RabitQ
```

这样可以：

```
RabitQ<1>
RabitQ<2>
RabitQ<4>
```

编译期优化。

---

# 11 与 PQ 的区别

| 方法     | codebook | 计算         |
| ------ | -------- | ---------- |
| PQ     | learned  | LUT        |
| RabitQ | random   | arithmetic |

RabitQ 优点：

```
SIMD friendly
```

---

# 12 实现难点

主要难点：

### 1 random rotation

可以用

```
Hadamard rotation
```

避免矩阵乘。

---

### 2 normalization

```
centroid normalize
```

---

### 3 distance estimator

需要 careful scaling。

---
