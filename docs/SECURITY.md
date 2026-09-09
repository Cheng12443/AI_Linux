# KAI Linux — 攻击面与安全基线

> 攻击者视角审计：谁在攻击、打哪个面、影响、缓解。
> 状态更新：2026-09-09 安全加固后。

## 攻击面总览

```
                    ┌──────────────────────────────────────────┐
  远程(公网)        │  ai-web:8080   ai-monitor:9090           │
  ────────────────▶ │  ai-ws:9998    layer2-gw:9999(netlink)   │
                    └──────────────┬───────────────────────────┘
                                   │
  本地进程            kai-plugin.sock(/tmp, daemon=root)
  ────────────────▶  /proc/ai_plugins  /proc/kai_metrics  /proc/hello_ai
                    └──────────────────────┬────────────────────┘
  内核面            sys_infer #548（需集成）· eBPF · LKM 加载
```

## 风险登记表（审计 → 处置）

| # | 攻击面 | 威胁/影响 | 处置前 | 处置后 |
|---|---|---|---|---|
| 1 | `kai-plugin` daemon（root） | 任意本地用户连 socket → `plug` 任意命令 → **本地提权** | 无鉴权 | ✅ socket chmod 0600 + `SO_PEERCRED` 校验 uid==0 + 特权命令白名单 |
| 2 | `ai-web`/`ai-monitor`/`ai-ws` | 公网可达：信息泄露 + **白嫖你的 DeepSeek/Kimi 配额（经济）** + WS 任意推理 | 绑 0.0.0.0 无鉴权 | ✅ 默认绑 127.0.0.1；`--bind`/`--host` 显式开放 + 提示前置鉴权 |
| 3 | `/proc/ai_plugins/list` | 任意用户启停内核插件 | mode 0644 | ✅ 0600（仅 root） |
| 4 | `/proc/hello_ai/infer` | 任意用户触发推理测试（DoS） | mode 0222 | ✅ 0600 |
| 5 | `sys_infer` #548 | 未授权推理/配额轰炸 | （设计） | ✅ 内建 CAP_SYS_ADMIN/NICE + 10s/200 限流 + 并发 1024 |
| 6 | 大模型 prompt 注入 | 诱导引擎输出恶意内容 | — | ⚠ 缓解：temperature 低、输出仅回内核侧消费；配额走 API 侧限额 |
| 7 | 密钥 | 泄露→盗刷 | env var | ✅ 日志一律 `_mask_key`；建议 KMS/密钥环 |
| 8 | Web JSON API XSS | 面板注入 | — | ✅ 输出 textContent/escHtml |
| 9 | `model_crypto` 无 cryptography 库时 | 降级 XOR（演示级） | — | ⚠ 明确标注：生产须装 cryptography，或用内核 crypto API |

## 审计方法与复现

```bash
# 静态扫描（规则+AST）
python3 kai/tests/security_scan.py --dir .   # 期望 0/0/0/0

# 语义攻击面（socket/绑定/proc 权限/命令执行点）
# 见上文风险登记；核心修复已内置：

# 1) 服务默认回环：
#    ai-web --port 8080        # 只监听 127.0.0.1
#    ai-monitor --port 9090    # 只监听 127.0.0.1
#    ai-ws                     # 只监听 127.0.0.1
# 2) kai-plugin：
#    kai-plugin serve          # socket 0600 + root-only 特权命令
# 3) /proc：ai_plugins/list、hello_ai/infer 均为 0600
```

## 部署建议（生产）

1. **默认全回环**；对外必须：反向代理 + TLS + 认证（Basic/OAuth/API-key）
2. `kai-plugin serve` 仅以 root 运行；非 root 客户端只读
3. API Key 用密钥环/KMS，环境变量次之；配 DeepSeek/Kimi 账号**消费限额**
4. 集成 `sys_infer` 的内核保持默认 CAP 收紧，按用户/容器给 `CAP_SYS_NICE` 白名单
5. eBPF/LKM 程序签名校验（Secure Boot + module signing）
6. 定期跑 `scripts/quality_gate.sh`
