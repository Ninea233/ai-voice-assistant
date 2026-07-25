---
name: daily_briefing
description: 综合天气、时间、新闻生成今日简报（LLM 判断触发）
trigger_mode: llm_only
category: utility
priority: 5
tools:
  - mcp.get_weather
  - mcp.get_time
  - mcp.get_news
---

# 今日简报生成

## 步骤

1. **获取天气**：调用 `mcp.get_weather` 获取用户所在城市（默认杭州）的实时天气
2. **获取时间**：调用 `mcp.get_time` 获取当前北京时间
3. **获取新闻**：调用 `mcp.get_news` 获取今日热点新闻（可选）
4. **综合生成**：基于以上数据生成简洁自然的今日简报回复（不超过 80 字）

## 回复模板

```
早上好/下午好/晚上好！今天是{日期}，{城市}当前{天气状况}，温度{温度}，{风力}。{新闻摘要}。
```

## 注意事项

- 根据当前时间选择合适的问候语（早上好/下午好/晚上好）
- 如果某个 MCP 工具调用失败，跳过对应部分，不要编造数据
- 新闻部分可选，如果获取失败则不提及
- 保持语气友好自然
