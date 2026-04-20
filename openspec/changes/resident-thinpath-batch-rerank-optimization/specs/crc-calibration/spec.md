## ADDED Requirements

### Requirement: CRC calibration 必须作为离线或 prepare 阶段能力存在
CRC calibration MUST 被视为离线构建或 benchmark prepare 阶段能力，不得被在线 E2E benchmark 查询口径隐式包含。在线查询阶段 SHALL 只消费 calibration 结果，而不是重新生成 calibration 结果。

#### Scenario: 在线阶段仅消费 calibration 结果
- **WHEN** 在线 E2E benchmark 开始执行 query round
- **THEN** 系统必须已经具备可直接使用的 CRC calibration 结果

### Requirement: CRC calibration 工件必须可被 benchmark 显式发现和加载
系统 MUST 为在线 benchmark 提供可定位、可校验的 CRC calibration 工件或等价参数存储方式，使 benchmark 能显式判断工件是否存在并据此决定是否可以进入在线查询阶段。

#### Scenario: 工件可见性明确
- **WHEN** benchmark 检查索引目录或相关元数据
- **THEN** 系统必须能够明确判断 CRC calibration 工件是否齐备
