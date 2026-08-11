# ADR-0017：许可证（Apache License 2.0）

- **状态**：已接受
- **日期**：2026-08-10
- **决策者**：Pride Leong
- **关联**：[adr/0005](./0005-lightweight-multiplatform.md) · [adr/0013](./0013-cross-machine-transport.md) · [adr/0016](./0016-config-format.md)

---

## 决策

天枢采用 **Apache License 2.0**。

## 候选方案对比

| 许可证 | 商业友好 | 专利保护 | 与 Zenoh 兼容 | 与 Protobuf 兼容 | 与 FlatBuffers 兼容 | 与 nlohmann/json 兼容 |
|---|---|---|---|---|---|---|
| **Apache 2.0** | ✅ | ✅ | ✅（Zenoh 双许可含 Apache-2.0） | ✅ | ✅ | ✅ |
| MIT | ✅ | ⚠️ 弱 | ✅ | ✅ | ✅ | ✅ |
| BSD-3 | ✅ | ⚠️ 弱 | ✅ | ✅ | ✅ | ✅ |
| GPL-3.0 | ❌ copyleft | ✅ | ⚠️ | ✅ | ✅ | ✅ |
| AGPL-3.0 | ❌（网络 copyleft） | ✅ | ❌ | ✅ | ✅ | ✅ |
| LGPL-3.0 | ⚠️（动态链接 OK） | ✅ | ✅ | ✅ | ✅ | ✅ |

## 决策依据

1. **专利保护**：Apache-2.0 含明确专利授权条款，对天枢的工程实现（DSL/trace/codegen/SLA 等技术）提供法律保护
2. **与生态兼容**：所有候选第三方依赖（Zenoh / Protobuf / FlatBuffers / nlohmann/json / yaml-cpp / toml++ / GoogleTest 等）都允许与 Apache-2.0 链接
3. **商业友好**：Apache-2.0 允许商业闭源衍生品（仅需 attribution + NOTICE），适合内部商用 + 开源双轨
4. **工业先例**：主流 C++ 框架（Apache Thrift、Google Protobuf、Envoy、gRPC、Apollo CyberRT）均用 Apache-2.0
5. **合规简单**：单一许可证，无 copyleft 复杂性

## 影响范围

### 仓库根目录新增

```
tianshu/
├── LICENSE                # Apache License 2.0 全文
├── NOTICE                 # 天枢 + 第三方 attribution
└── ...
```

### LICENSE 文件

放置 Apache License 2.0 标准全文（https://www.apache.org/licenses/LICENSE-2.0.txt）。

### NOTICE 文件

```
TIANSHU
Copyright 2026 The TIANSHU Team. All Rights Reserved.

This product includes software developed by the TIANSHU project
(https://github.com/lykling/tianshu).

This product includes software developed by:
- Eclipse Zenoh (https://zenoh.io) — Apache License 2.0
- Google Protocol Buffers (https://github.com/protocolbuffers/protobuf) — BSD-3
- Google FlatBuffers (https://github.com/google/flatbuffers) — Apache License 2.0
- nlohmann/json (https://github.com/nlohmann/json) — MIT
- yaml-cpp (https://github.com/jbeder/yaml-cpp) — MIT
- toml++ (https://github.com/marzer/tomlplusplus) — MIT
- GoogleTest (https://github.com/google/googletest) — BSD-3
- GoogleBenchmark (https://github.com/google/benchmark) — Apache License 2.0
```

### 源码头部版权

所有源码文件必须有英文版权头（与 [ADR-0009](./0009-doc-code-language.md) 协同）：

```cpp
// Copyright 2026 The TIANSHU Team. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
```

### 文档头部

每份文档（双语版本）顶部加：

```markdown
> Copyright 2026 The TIANSHU Team. All Rights Reserved.
> Licensed under the Apache License, Version 2.0.
```

### README 修订

`README.md` 顶部 license 徽章改为：

```
[![license](https://img.shields.io/badge/license-Apache--2.0-blue)](LICENSE)
```

`README.en.md` 同步修订。

### CI 守护

| ID | 描述 |
|---|---|
| INFRA-CI-13（新） | `license-check`：所有源文件必须有版权头（CI lint） |
| INFRA-CI-14（新） | `NOTICE-check`：新增第三方依赖必须更新 NOTICE |

## 与现有 ADR 协同

| ADR | 协同点 |
|---|---|
| [adr-0005 依赖治理](./0005-lightweight-multiplatform.md) | `ALLOWED_DEPS.txt` 含每依赖的许可证，禁止 GPL/AGPL |
| [adr-0009 双语+英文 commit](./0009-doc-code-language.md) | 版权头英文 |
| [adr-0013 跨机](./0013-cross-machine-transport.md) | Zenoh Apache-2.0 子许可与天枢兼容 |
| [adr-0016 配置格式](./0016-config-format.md) | toml++/yaml-cpp/nlohmann/json 都兼容 |

## 风险与缓解

| 风险 | 缓解 |
|---|---|
| 第三方依赖引入 GPL（污染） | CI 守护：`license-check` 扫描所有依赖许可证，发现 GPL 即 fail |
| 贡献者协议缺失 | 加 CLA（Contributor License Agreement）模板，所有贡献者签署 |
| 商标问题（"天枢" / "TIANSHU"） | 注册商标（如需商业化） |
| 未来许可证变更（如改 MIT） | Apache-2.0 转 MIT 单向可（Apache 专利条款需要明确放弃） |

## 后续可能演进

- 如果未来商业化需要 dual license → 商业版额外条款
- 如果未来基金会托管（CNCF / Eclipse / Apache 软件基金会）→ 转移版权到基金会
- 如果未来出现 strong copyleft 需求（小概率） → 评估 GPL

## 参考

- Apache License 2.0 全文：https://www.apache.org/licenses/LICENSE-2.0
- Apache 许可证分发指南：https://www.apache.org/foundation/license-faq.html
- 选择许可证工具：https://choosealicense.com/
- SPDX 标识符：Apache-2.0
