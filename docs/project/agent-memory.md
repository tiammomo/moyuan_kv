# 项目长期记忆

这个文档记录 `mokv` 仓库里长期稳定、适合持续复用的协作信息。它不是功能设计稿，而是“以后继续改仓库时默认应该记住什么”。

## 项目当前定位

`mokv` 是一个面向大模型工作负载的 KV / 元数据存储项目，当前由三层组成：

- 嵌入式存储内核：`DB`、`MemTable`、`SST`、`Manifest`
- 分布式服务端路径：`Raft + gRPC`
- LLM 场景访问层：`llm::LLMStore`

仓库已经从 `Shuai-KV` 统一更名为 `mokv`，后续新增文档、构建目标、说明文字都应继续使用 `mokv`。

## 当前真实边界

这些信息在后续讨论和设计里应视为“代码当前事实”：

- `mokv_server` 当前仍然主要从当前工作目录读取 `./raft.cfg`
- `MokvConfig` 主要接在库内包装器上，还不是完整的 server 进程配置入口
- `ConversationStore` 当前支持的是按 turn 数裁剪，不是按 token 数裁剪
- `RetrievalStore` 当前存储检索元数据和 `embedding_ref`，不是向量本体
- `UpdateConfig` RPC 当前还没有完成

## 当前验证基线

截至 `2026-04-03`：

- 默认压缩开启
- LZ4 已切换到标准 `liblz4` frame API
- `ctest --test-dir build --output-on-failure` 为 `11/11` 通过

如果后续代码变更导致这些事实失效，应优先更新 `README.md`、`docs/README.md` 和对应专题文档。

## 文档约定

- 所有长期文档统一放在 `docs/`
- `docs/getting-started/` 放项目总览和新读者入口
- `docs/design/` 放架构和模块关系
- `docs/storage/` 放 LSM、缓存、压缩、DB 等实现说明
- `docs/distributed/` 放 Raft 和服务端路径
- `docs/llm/` 放大模型场景封装
- `docs/operations/` 放部署、构建、测试
- `docs/project/` 放项目长期记忆和协作约束
- `docs/reference/` 放工具类和公共组件说明

## 协作约束

- Git 提交信息使用 Conventional Commits：`<type>(<scope>): <subject>`
- 不在提交信息里加入 AI 生成标记
- 不追加 `Signed-off-by` 或 `Co-authored-by`，除非明确要求
- 修改代码时，文档要同步更新，尤其是接口、测试基线、目录结构和运行方式

## 默认工作方式

继续演进这个仓库时，默认优先级应是：

1. 先以代码当前行为为准，不按历史设计稿推断
2. 修改接口或目录后，立即同步 `README.md` 和 `docs/`
3. 只在验证通过后更新“测试基线”类描述
4. 当服务端路径和库内路径不一致时，文档里要明确区分两者
