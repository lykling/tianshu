# ADR-0001：DSL 形式选型

- **状态**：已接受
- **日期**：2026-08-10
- **决策者**：Pride Leong
- **关联**：[00-overview.md](../00-overview.md)

## 背景

天枢的核心是"声明式数据流 + 加载期编译"。开发者用什么形式声明数据流，直接决定了：

- 用户学习成本
- 编译器实现复杂度
- 与现有 cyber 代码的兼容性
- 性能 ceiling（运行时零开销能否实现）

## 候选方案

### 方案 1：C++ 表达式模板（expression templates）

类似 Eigen / Blaze 的矩阵表达式。

```cpp
auto flow = reader<Camera>("/cam") | detect | filter | nms | writer<Det>("/det");
```

**优点**：

- 编译期完成所有工作（零运行时开销天然达成）
- 类型安全
- 表达力强

**缺点**：

- 编译时间爆炸（每多一个算子，模板实例化指数增长）
- 错误信息灾难（STL 经典灾难，C++20 concepts 部分缓解但不够）
- 用户写复杂 flow 时心智负担重（要懂模板元编程）
- 不支持运行时决策（trace / fallback / 状态处理都难做）

### 方案 2：YAML + 代码生成（external DSL）

类似 ROS 2 launch / Kubernetes manifest。

```yaml
flow: perception
steps:
  - reader: /perception/front
  - op: detect
  - op: filter
  - writer: /perception/det
sla: { deadline_ms: 50 }
```

**优点**：

- 用户友好（不需要懂 C++）
- 工具链友好（IDE 补全 / linter / validator）
- 与配置文件同源

**缺点**：

- 算子逻辑仍要 C++ 写，跨语言协调心智重
- 类型检查弱（YAML 不知道 `detect` 输入是 Camera）
- 表达力受限（复杂控制流写不出来）
- 与 cyber 现有 `.dag` 风格太像，区分度低

### 方案 3：C++ fluent builder + auto trace（**已选**）

类似 JAX / PyTorch Dynamo / torch.compile。开发者写**普通 C++ 函数**，用 fluent builder 风格表达数据流，框架在加载期 dry-run 一次（trace）记录所有数据流操作，再编译为静态代码。

```cpp
void perception_flow(Node& node) {
  auto camera = node.reader<CameraMsg>("/perception/front");
  auto lidar  = node.reader<LidarCloud>("/lidar/points");

  node.on_input({camera, lidar}, [&](auto c, auto l) {
    auto det = detect_op(AllLatest::fuse(c, l));
    predict_out.write(predict_op(det));
  })
  .with_sla(SLA{.deadline_ms = 50})
  .with_fallback("perception_flow_lite");
}
REGISTER_TRACEABLE_FLOW("perception_flow", perception_flow);
```

**优点**：

- **现代**：2018+ SOTA（JAX 2020、torch.compile 2022 都是这条路）
- **兼容**：与 cyber 现有代码风格相近（Node / Reader / Writer），用户迁移友好
- **可追溯**：trace 是确定性的，配合纯函数 codegen 可实现可重复构建
- **新颖性强**：AD 中间件领域无先例（最接近的是 Autoware System Designer，但实现路线不同）
- **escape hatch**：trace 失败可回退到命令式执行（robust）

**缺点**：

- trace 实现复杂（要拦截所有数据流操作，类似 Dynamo 的 guard）
- 加载期有一次 dry-run 开销（启动慢几十毫秒）
- 确定性 trace 是新概念，需要文档教育用户

## 决策

**选方案 3**：C++ fluent builder + auto trace。

## 决策依据

1. **零开销可达**：trace + 静态 C++ codegen 产出的代码与手写二进制不可区分
2. **兼容 cyber 用户**：API 风格一致（`Node::CreateReader` ↔ `Node::reader`），迁移成本低
3. **新颖性硬**：JAX-style trace + codegen 是大数据领域 2020+ 的前沿，搬到 AD 中间件是真正的范式跃迁
4. **工程稳健**：有 escape hatch，trace 失败不阻塞
5. **覆盖专利权利要求**：方案 3 的实施例可以支撑独权 + 大部分从权

## 影响

- `tianshu/dsl/` 提供 fluent builder API（Node / Reader / Writer / on_input / with_sla 等）
- `tianshu/trace/` 提供 RAII guard 体系，拦截所有数据流操作
- `tianshu/compiler/` 实现六阶段 pass，其中 Pass 0 是 trace
- DSL 用户文档以"flow 函数"为核心概念，不以"配置文件"为核心
- 用户教育成本：需要解释"trace 是什么 / 为什么有加载期 dry-run"

## 后续可能演进

- 如果 trace 路线在 PoC（Phase 1 H1 验证）失败 → 退到方案 1（表达式模板）+ 运行时解释
- 如果未来需要支持 Python 绑定 → fluent builder 风格天然适配（Python 也能写 fluent builder）
- 如果未来需要图形化编排 → trace AST 可以序列化为图，反向生成 GUI
- 如果未来需要 Rust/Go/Node SDK → DSL fluent builder 风格天然适配这些语言（详见 [adr/0007](./0007-api-spec-multi-language.md)）

## 参考

- JAX：https://jax.readthedocs.io/en/latest/notebooks/quickstart.html
- PyTorch Dynamo：https://pytorch.org/docs/stable/generated/dynamooverview.html
- torch.compile：https://pytorch.org/tutorials/intermediate/torch_compile_tutorial.html
- Autoware System Designer：https://autowarefoundation.github.io/autoware-documentation/main/design/
