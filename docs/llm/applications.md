# LLM 应用场景

**对应文件**：`mokv/llm/store.hpp`

这篇文档只描述当前代码里已经存在的 LLM 访问层，不再使用历史上的伪接口示例。

## 当前已经实现的四类场景

### 1. Prompt Cache

对应结构：

```cpp
struct PromptCacheEntry {
    std::string tenant;
    std::string app_id;
    std::string model;
    std::string prompt_hash;
    std::string response;
    int64_t cached_at_ms;
    uint32_t input_tokens;
    uint32_t output_tokens;
};
```

当前接口：

- `PutPromptCache(...)`
- `GetPromptCache(...)`
- `ListPromptCacheEntries(...)`
- `DeletePromptCache(...)`

适合存：

- prompt 模板 hash
- 命中后的响应
- token 统计

当前没有内建 TTL 机制。

### 2. Conversation

对应结构：

- `ConversationDescriptor`
- `ConversationTurn`
- `ConversationSession`
- `ConversationSummary`

当前接口：

- `SaveConversation(...)`
- `GetConversation(...)`
- `AppendConversationTurn(...)`
- `ListConversationSessions(...)`
- `ListConversationTurns(...)`
- `TrimConversationToLastTurns(...)`
- `DeleteConversation(...)`

这里最重要的同步点是：

- 当前裁剪能力是“保留最后 N 个 turn”
- 不是按 token 数裁剪

### 3. Retrieval Metadata

对应结构：

```cpp
struct RetrievalChunk {
    std::string tenant;
    std::string knowledge_base;
    std::string document_id;
    std::string chunk_id;
    std::string content;
    std::string embedding_ref;
    std::vector<std::pair<std::string, std::string>> metadata;
};
```

当前接口：

- `PutRetrievalChunk(...)`
- `GetRetrievalChunk(...)`
- `ListRetrievalChunks(...)`
- `DeleteRetrievalChunk(...)`

需要明确：

- 这里存的是检索元数据
- `embedding_ref` 只是向量引用
- 向量本体仍然应该放在外部向量库

### 4. Runtime Config

对应结构：

```cpp
struct RuntimeConfigRecord {
    std::string tenant;
    std::string app_id;
    std::string profile;
    std::string payload;
    int64_t updated_at_ms;
};
```

当前接口：

- `PutRuntimeConfig(...)`
- `GetRuntimeConfig(...)`
- `ListRuntimeConfigs(...)`
- `DeleteRuntimeConfig(...)`

这里的 `payload` 当前就是一段业务自定义字符串，通常会放 JSON。

## 当前 keyspace 设计

`Keyspace` 把不同场景拆到了固定前缀下：

| 场景 | Key 前缀 |
|------|------|
| Prompt Cache | `llm:prompt-cache:*` |
| Conversation | `llm:conversation:*` |
| Retrieval | `llm:retrieval:*` |
| Runtime Config | `llm:runtime-config:*` |

这也是为什么 `DBKVStore` 需要：

- `ListKeysByPrefix(...)`
- `ListEntriesByPrefix(...)`

## 为什么这层不是直接 `Put/Get`

如果业务自己手写 key，通常会遇到这些问题：

- key 规则不统一
- 转义处理容易出错
- 列举会话或配置时只能自己维护索引
- 结构体序列化分散在业务代码里

当前 `llm/store.hpp` 做了三件统一工作：

1. 统一 keyspace
2. 统一序列化 / 反序列化
3. 统一 prefix scan 的使用方式

## 当前序列化方式

这层不是 JSON-only 实现。

当前主要使用的是：

- 基于 `codec::Escape(...)` 的行文本序列化
- `tenant\tvalue`、`model\tvalue` 这类字段行格式

它的优点是：

- 简单
- 可读
- 不额外引入 JSON 依赖

## 最小示例

### Prompt Cache

```cpp
mokv::DBKVStore store;
mokv::llm::LLMStore llm(store);

mokv::llm::PromptCacheEntry entry;
entry.tenant = "tenant-a";
entry.app_id = "gateway";
entry.model = "gpt-5.4";
entry.prompt_hash = "hash-001";
entry.response = "{\"answer\":\"ok\"}";
entry.cached_at_ms = 1712345678;

llm.PutPromptCache(entry);
auto loaded = llm.GetPromptCache("tenant-a", "gateway", "gpt-5.4", "hash-001");
```

### Conversation

```cpp
mokv::llm::ConversationDescriptor descriptor;
descriptor.tenant = "tenant-a";
descriptor.app_id = "copilot";
descriptor.session_id = "session-01";
descriptor.model = "gpt-5.4-mini";

llm.AppendConversationTurn(descriptor, {"user", "帮我 review 这个 patch", 1});
llm.AppendConversationTurn(descriptor, {"assistant", "先看变更范围。", 2});

auto turns = llm.ListConversationTurns("tenant-a", "copilot", "session-01");
llm.TrimConversationToLastTurns("tenant-a", "copilot", "session-01", 1);
```

### Retrieval Metadata

```cpp
mokv::llm::RetrievalChunk chunk;
chunk.tenant = "tenant-a";
chunk.knowledge_base = "docs";
chunk.document_id = "doc-42";
chunk.chunk_id = "chunk-7";
chunk.content = "mokv keeps metadata in KV and vectors in ANN.";
chunk.embedding_ref = "vec://docs/doc-42/chunk-7";

llm.PutRetrievalChunk(chunk);
```

### Runtime Config

```cpp
mokv::llm::RuntimeConfigRecord record;
record.tenant = "tenant-a";
record.app_id = "router";
record.profile = "production";
record.payload = "{\"primary_model\":\"gpt-5.4\"}";
record.updated_at_ms = 1712345678;

llm.PutRuntimeConfig(record);
auto configs = llm.ListRuntimeConfigs("tenant-a", "router");
```

## 当前测试覆盖

`tests/llm_store_test.cpp` 已经覆盖：

- Prompt Cache round-trip + prefix scan
- Conversation append / list / trim
- Retrieval metadata round-trip
- Runtime config round-trip

截至 `2026-04-03`，这些测试已经通过。

## 当前最适合它的用途

如果你现在就要在仓库里复用一层“面向大模型应用的持久化元数据存储”，这层已经足够直接上手：

- Prompt cache
- session / chat history
- RAG metadata
- runtime routing config

如果你下一步要继续增强，最值得优先补的是：

- TTL / 过期策略
- token-based conversation trimming
- 更结构化的 payload 编码格式
- 和向量库 / 模型服务的上层集成适配
