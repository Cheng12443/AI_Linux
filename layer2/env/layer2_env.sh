# AI Linux Layer2 环境配置
# Layer2 网关的环境变量文件

# ===========================================================================
# API 密钥（必填）
# ===========================================================================

# DeepSeek API Key
# 获取地址：https://platform.deepseek.com/api_keys
export DEEPSEEK_API_KEY="${DEEPSEEK_API_KEY:-}"

# Kimi K3 (Moonshot) API Key
# 获取地址：https://platform.moonshot.cn/console/api-keys
export KIMI_API_KEY="${KIMI_API_KEY:-}"

# ===========================================================================
# 默认后端
# ===========================================================================

# 可选值: deepseek | kimi
export AI_BACKEND="${AI_BACKEND:-deepseek}"

# ===========================================================================
# Skill 路由权重
# ===========================================================================

# DeepSeek 权重（调度类任务）
export SKILL_WEIGHT_DEEPSEEK_SCHED="${SKILL_WEIGHT_DEEPSEEK_SCHED:-70}"
export SKILL_WEIGHT_KIMI_SCHED="${SKILL_WEIGHT_KIMI_SCHED:-30}"

# DeepSeek 权重（IO 类任务）
export SKILL_WEIGHT_DEEPSEEK_IO="${SKILL_WEIGHT_DEEPSEEK_IO:-50}"
export SKILL_WEIGHT_KIMI_IO="${SKILL_WEIGHT_KIMI_IO:-50}"

# DeepSeek 权重（安全类任务）
export SKILL_WEIGHT_DEEPSEEK_SEC="${SKILL_WEIGHT_DEEPSEEK_SEC:-40}"
export SKILL_WEIGHT_KIMI_SEC="${SKILL_WEIGHT_KIMI_SEC:-60}"

# DeepSeek 权重（内存类任务）
export SKILL_WEIGHT_DEEPSEEK_MEM="${SKILL_WEIGHT_DEEPSEEK_MEM:-65}"
export SKILL_WEIGHT_KIMI_MEM="${SKILL_WEIGHT_KIMI_MEM:-35}"

# DeepSeek 权重（代码类任务）
export SKILL_WEIGHT_DEEPSEEK_CODE="${SKILL_WEIGHT_DEEPSEEK_CODE:-80}"
export SKILL_WEIGHT_KIMI_CODE="${SKILL_WEIGHT_KIMI_CODE:-20}"

# DeepSeek 权重（编排类任务）
export SKILL_WEIGHT_DEEPSEEK_ORCH="${SKILL_WEIGHT_DEEPSEEK_ORCH:-30}"
export SKILL_WEIGHT_KIMI_ORCH="${SKILL_WEIGHT_KIMI_ORCH:-70}"

# ===========================================================================
# 运行模式
# ===========================================================================

# 运行模式: async | sync | override
export AI_MODE="${AI_MODE:-async}"

# 同步模式超时（毫秒）
export AI_SYNC_TIMEOUT_MS="${AI_SYNC_TIMEOUT_MS:-1000}"

# ===========================================================================
# API 参数
# ===========================================================================

export AI_API_TIMEOUT_MS="${AI_API_TIMEOUT_MS:-5000}"
export AI_API_MAX_RETRIES="${AI_API_MAX_RETRIES:-3}"
export AI_API_TEMPERATURE="${AI_API_TEMPERATURE:-0.1}"
export AI_API_MAX_TOKENS="${AI_API_MAX_TOKENS:-256}"

# ===========================================================================
# 路由阈值
# ===========================================================================

# 置信度阈值 (0-10000)
export AI_THRESHOLD_SCHED="${AI_THRESHOLD_SCHED:-7000}"
export AI_THRESHOLD_IO="${AI_THRESHOLD_IO:-7000}"
export AI_THRESHOLD_SEC="${AI_THRESHOLD_SEC:-8000}"

# ===========================================================================
# 缓存
# ===========================================================================

# 决策缓存开关
export AI_CACHE_ENABLED="${AI_CACHE_ENABLED:-1}"

# 缓存 TTL（毫秒）
export AI_CACHE_TTL_MS="${AI_CACHE_TTL_MS:-5000}"

# ===========================================================================
# MCP 配置
# ===========================================================================

# MCP 协议版本
export MCP_VERSION="${MCP_VERSION:-2024-11-05}"

# MCP 工具调用超时（毫秒）
export MCP_TOOL_TIMEOUT_MS="${MCP_TOOL_TIMEOUT_MS:-3000}"

# 启用 MCP 模式
export MCP_ENABLED="${MCP_ENABLED:-1}"

# ===========================================================================
# 日志
# ===========================================================================

export AI_LOG_LEVEL="${AI_LOG_LEVEL:-info}"  # debug | info | warn | error
export AI_LOG_FILE="${AI_LOG_FILE:-/var/log/ai-layer2.log}"

# ===========================================================================
# 共享内存
# ===========================================================================

export AI_SHMEM_ENABLED="${AI_SHMEM_ENABLED:-1}"
export AI_SHMEM_SIZE_MB="${AI_SHMEM_SIZE_MB:-4}"

# ===========================================================================
# 网络
# ===========================================================================

# Layer2 监听地址（用于内核 netlink 连接）
export AI_LISTEN_ADDR="${AI_LISTEN_ADDR:-127.0.0.1}"
export AI_LISTEN_PORT="${AI_LISTEN_PORT:-9999}"

# ===========================================================================
# DeepSeek API 端点
# ===========================================================================
export DEEPSEEK_BASE_URL="${DEEPSEEK_BASE_URL:-https://api.deepseek.com}"
export DEEPSEEK_CHAT_PATH="${DEEPSEEK_CHAT_PATH:-/v1/chat/completions}"

# ===========================================================================
# Kimi K3 API 端点
# ===========================================================================
export KIMI_BASE_URL="${KIMI_BASE_URL:-https://api.moonshot.cn}"
export KIMI_CHAT_PATH="${KIMI_CHAT_PATH:-/v1/chat/completions}"

# ===========================================================================
# 代理（可选）
# ===========================================================================
export HTTP_PROXY="${HTTP_PROXY:-}"
export HTTPS_PROXY="${HTTPS_PROXY:-}"
