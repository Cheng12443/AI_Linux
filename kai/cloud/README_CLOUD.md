# KAI Linux — 云平台接入指南

## 支持的云平台

| 平台 | 指标服务 | 告警 | 文件 |
|---|---|---|---|
| AWS | CloudWatch | SNS/CloudWatch Alarms | `cloud_integration.py` |
| Azure | Monitor | Alerts | `cloud_integration.py` |
| GCP | Cloud Monitoring | Alerting | `cloud_integration.py` |
| 阿里云 | CloudMonitor | 短信/钉钉 | `cloud_integration.py` |

## 配置

### AWS

```bash
export AWS_ACCESS_KEY_ID=xxx
export AWS_SECRET_ACCESS_KEY=xxx
export AWS_REGION=us-east-1

python3 -c "
from cloud_integration import CloudIntegration
ci = CloudIntegration()
ci.configure('aws')
ci.push_metric('kai_inferences_total', 100)
"
```

### Azure

```bash
export AZURE_CLIENT_ID=xxx
export AZURE_TENANT_ID=xxx
export AZURE_CLIENT_SECRET=xxx
```

### GCP

```bash
export GOOGLE_APPLICATION_CREDENTIALS=/path/to/service-account.json
```

### 阿里云

```bash
export ALIBABA_CLOUD_ACCESS_KEY_ID=xxx
export ALIBABA_CLOUD_ACCESS_KEY_SECRET=xxx
export ALIBABA_CLOUD_REGION=cn-hangzhou
```

## 离线/测试模式

自定义端点无需真实云账号即可测试：

```python
ci.configure("aws", endpoint="http://localhost:8089")
```

## 推送的指标

| 指标 | 说明 |
|---|---|
| kai_inferences_total | 推理总数 |
| kai_inference_errors_total | 推理错误数 |
| kai_inference_latency_avg_ms | 平均延迟 |
| kai_cache_hits_total | 缓存命中 |
| kai_cache_misses_total | 缓存未命中 |
| kai_api_calls_total | API 调用 |

## 告警级别

| 级别 | 说明 |
|---|---|
| info | 信息通知 |
| warning | 需要关注 |
| critical | 立即处理 |

## 与 Prometheus/Alertmanager 集成

Prometheus 抓取 `/proc/kai_metrics` → Alertmanager 规则触发 →
webhook → 云平台告警
