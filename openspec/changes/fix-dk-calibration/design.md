## Context

ConANN 三分类依赖全局阈值 `d_k`（top-K 参考距离），在 build 时通过 `ConANN::CalibrateDistanceThreshold` 校准。当前实现从 database 向量自身采样 pseudo-query，假设 query 与 database 同分布。

COCO-1k benchmark 暴露了跨模态场景的问题：text→image 距离分布与 image→image 显著不同，导致 d_k=0.786 远小于实际 text→image 最近邻距离（~1.0-1.4），100% 向量被误判为 SafeOut。

## Goals / Non-Goals

**Goals:**
- 支持 query/database 不同分布场景下的 d_k 校准
- 保持现有 Build() API 签名不变，零影响 12+ 处调用点
- benchmark 输出正确的 ConANN 分类结果

**Non-Goals:**
- 运行时动态更新 d_k（仍为 build-time 静态值）
- 修改 ε_ip 校准逻辑（ε_ip 来自 database 内部编码误差，不受 query 分布影响）

## Decisions

### 1. 通过 Config 传入 query 样本（而非改 Build 签名）

```cpp
struct IvfBuilderConfig {
    // ... existing fields ...

    /// Optional query vectors for d_k calibration.
    /// If provided, d_k is calibrated from query→database distances.
    /// If nullptr, fallback to database self-sampling (existing behavior).
    /// Pointer must remain valid for the duration of Build().
    const float* calibration_queries = nullptr;
    uint32_t num_calibration_queries = 0;
};
```

**理由**：Config 传参是最小侵入方案。`Build()` 签名不变，现有调用点全部兼容（config 默认 nullptr 自动 fallback）。与现有 `calibration_samples`、`calibration_topk` 等 config 字段风格一致。

**替代方案**：改 Build() 参数 → 需修改 12+ 处调用；setter 方法 → 增加 stateful API 复杂度。

### 2. CalibrateDistanceThreshold 新增重载

```cpp
// 新: query/database 分离
static float CalibrateDistanceThreshold(
    const float* queries, uint32_t Q,
    const float* database, uint32_t N,
    Dim dim, uint32_t num_samples,
    uint32_t top_k, float percentile, uint64_t seed);

// 旧: 保留为 wrapper，内部调用新重载 (queries = database)
static float CalibrateDistanceThreshold(
    const float* vectors, uint32_t N, Dim dim,
    uint32_t num_samples, uint32_t top_k,
    float percentile, uint64_t seed);
```

**理由**：新重载不需要处理 self-distance（query 和 database 天然分离），逻辑更干净。旧签名保留为 wrapper 保证兼容。

### 3. IvfBuilder::CalibrateDk 适配逻辑

```
if (config_.calibration_queries != nullptr && config_.num_calibration_queries > 0):
    CalibrateDistanceThreshold(queries, Q, database, N, ...)
else:
    CalibrateDistanceThreshold(database, N, ...)  // 现有行为
```

## Risks / Trade-offs

- **[裸指针生命周期]** → Config 注释标注 "pointer must remain valid for the duration of Build()"，与现有 `Build(const float* vectors, ...)` 风格一致
- **[query 样本量不足]** → 如果 `num_calibration_queries < calibration_samples`，自动 clamp 到可用数量。极端情况（1-2 条 query）d_k 统计意义不足，但不会 crash
- **[同模态用户无感知]** → 默认 nullptr fallback，同模态场景零改动零风险
