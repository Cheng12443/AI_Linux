# KAI Linux — 故障排查手册

## 目录

1. [常见问题](#常见问题)
2. [内核模块问题](#内核模块问题)
3. [Layer2 网关问题](#layer2-网关问题)
4. [API 连接问题](#api-连接问题)
5. [性能问题](#性能问题)
6. [Web/UI 问题](#webui-问题)

---

## 常见问题

### Q: 未设置 API 密钥

**症状**：推理返回空或错误提示"未设置 API 密钥"

**解决**：
```bash
export DEEPSEEK_API_KEY=sk-xxx
# 或
export KIMI_API_KEY=sk-xxx
```

验证：
```bash
echo $DEEPSEEK_API_KEY  # 确认非空
```

---

### Q: 推理超时

**症状**：`ai ask` 长时间无响应，最终超时

**排查步骤**：
1. 检查网络连接：`curl https://api.deepseek.com`
2. 检查 API Key 是否有效
3. 增加超时：`ai --timeout 60000 ask "..."`

---

## 内核模块问题

### Q: insmod 失败 "Operation not permitted"

**解决**：
```bash
# 检查 Secure Boot
mokutil --sb-state

# 如果 Secure Boot 开启，需要签名模块
sudo mokutil --import MOK.der
```

### Q: 模块版本不匹配 "Invalid module format"

**原因**：模块编译时内核版本与运行内核不一致

**解决**：
```bash
# 使用匹配的内核头文件重新编译
make modules KERNEL_SRC=/usr/src/linux-headers-$(uname -r)
```

### Q: 模块加载后系统崩溃

**排查**：
```bash
# 检查 dmesg
dmesg | tail -50

# 使用 kmemleak 检测内存泄漏
echo scan > /sys/kernel/debug/kmemleak
cat /sys/kernel/debug/kmemleak
```

---

## Layer2 网关问题

### Q: 网关无法连接内核

**排查**：
```bash
# 检查内核模块是否加载
lsmod | grep kai

# 检查 netlink socket
ss -x | grep kai

# 检查网关日志
tail -f /var/log/kai-linux/gateway.log
```

### Q: 决策始终走降级路径

**症状**：日志显示大量 "fallback" 调用

**原因**：API 不可用，降级到本地规则引擎

**排查**：
```bash
# 检查 API 连通性
curl -s https://api.deepseek.com/v1/models \
  -H "Authorization: Bearer $DEEPSEEK_API_KEY"

# 检查 API Key 余额
```

---

## API 连接问题

### Q: DeepSeek 401 错误

**原因**：API Key 无效或已过期

**解决**：
1. 到 platform.deepseek.com 检查 Key
2. 更新环境变量
3. 重启网关

### Q: Kimi K3 429 错误

**原因**：请求频率超限

**解决**：
1. 降低请求频率
2. 启用缓存（`cache_enabled: true`）
3. 增加批处理（减少请求数）

---

## 性能问题

### Q: 推理延迟高

**排查**：
```bash
# 运行性能分析
python3 tools/perf_profiler.py

# 查看延迟统计
cat /proc/kai_metrics | grep latency
```

**优化**：
1. 启用缓存（减少重复推理）
2. 启用批处理（合并请求）
3. 使用本地模型（避免网络延迟）

### Q: 内存使用过高

**排查**：
```bash
# 查看内存
cat /proc/kai_metrics | grep mem
```

**解决**：
1. 减小模型缓存（`cache_ttl_ms`）
2. 卸载不用的模型
3. 启用模型量化（INT8）

---

## Web/UI 问题

### Q: Web Dashboard 无法访问

**排查**：
```bash
# 检查端口
ss -tlnp | grep 8080

# 检查防火墙
ufw status

# 检查服务日志
tail -f /var/log/kai-linux/web.log
```

### Q: TUI 显示乱码

**解决**：
```bash
# 检查终端编码
echo $LANG  # 应为 UTF-8

# 设置 UTF-8
export LANG=en_US.UTF-8
```

### Q: WebSocket 连接断开

**原因**：网络不稳定或心跳超时

**解决**：
```bash
# 客户端添加重连逻辑
# 或降低心跳间隔
```

---

## 快速诊断命令

```bash
# 一键诊断
echo "=== 内核模块 ===" && lsmod | grep kai
echo "=== 进程 ===" && ps aux | grep kai
echo "=== 端口 ===" && ss -tlnp | grep -E "8080|9090|9998"
echo "=== 指标 ===" && cat /proc/kai_metrics 2>/dev/null
echo "=== 日志 ===" && tail -20 /var/log/kai-linux/*.log 2>/dev/null
```
