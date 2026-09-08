# KAI Linux — 部署指南

## 目录

1. [Docker 部署（推荐）](#docker-部署推荐)
2. [裸机部署](#裸机部署)
3. [虚拟机部署](#虚拟机部署)
4. [Kubernetes 部署](#kubernetes-部署)

---

## Docker 部署（推荐）

### 前提

- Docker 20.10+
- Docker Compose v2

### 步骤

```bash
cd kai/deploy

# 1. 设置密钥
cp .env.example .env
vim .env  # 填入 DEEPSEEK_API_KEY 或 KIMI_API_KEY

# 2. 一键启动所有模块
./docker-up.sh

# 3. 访问
# Web Dashboard → http://localhost:8080
# 性能监控    → http://localhost:9090
# WebSocket   → ws://localhost:9998
```

### 单模块启动

```bash
docker compose up -d kai-web       # 仅 Web
docker compose up -d kai-monitor   # 仅监控
docker compose up -d kai-ws        # 仅 WebSocket
docker compose up -d kai-gateway   # 仅网关
```

---

## 裸机部署

### Ubuntu 24.04

```bash
# 1. 一键安装
curl -fsSL https://your-domain/install.sh | sudo bash
# 或
sudo ./kai/deploy/install.sh --all

# 2. 设置密钥
export DEEPSEEK_API_KEY=sk-xxx

# 3. 启动
kai-tui              # TUI
kai-web --port 8080  # Web
```

### Debian 12

```bash
# 安装依赖
sudo apt install -y build-essential gcc make python3-pip \
  libncurses-dev curl

# 构建 .deb 包
sudo ./kai/deploy/debian_pack.sh
sudo dpkg -i kai-linux_1.0.0_amd64.deb
```

---

## 虚拟机部署

### QEMU/KVM

```bash
# 1. 构建磁盘镜像
sudo ./kai/deploy/build_qemu_image.sh --size 8G --with-ui

# 2. 启动
./kai/deploy/run_vm.sh --kvm --mem 4096 --smp 4
```

### VirtualBox/VMware

```bash
# 1. 构建 ISO
sudo ./kai/deploy/build_iso.sh

# 2. 挂载 ISO 启动
# VirtualBox: 新建虚拟机 → 挂载 ISO
# VMware: 新建虚拟机 → 挂载 ISO
```

登录信息：`kai` / `kai123`（root：`root123`）

---

## Kubernetes 部署

### 前置条件

- Kubernetes 1.20+
- 节点具有 AI 加速能力（可选）

### 步骤

```bash
# 1. 部署 Device Plugin
kubectl apply -f kai/deploy/k8s/device-plugin.yaml

# 2. 创建 API 密钥 Secret
kubectl create secret generic kai-api-keys \
  --from-literal=deepseek=sk-xxx \
  --from-literal=kimi=sk-xxx \
  -n kube-system

# 3. 部署 KAI Linux 服务
kubectl apply -f kai/deploy/k8s/deployment.yaml

# 4. 在 Pod 中请求 AI 资源
# resources:
#   limits:
#     kai-linux.ai/inference: "1"
```

---

## 环境要求汇总

| 组件 | 最低要求 | 推荐 |
|---|---|---|
| 内核 | 6.6+ | 6.12 LTS |
| CPU | 2 核 | 4+ 核 |
| 内存 | 2GB | 4GB+ |
| 磁盘 | 10GB | 50GB |
| 网络 | 外网（API 调用）| — |

---

## 验证部署

```bash
# 1. 检查服务
curl http://localhost:8080/api/status

# 2. 测试推理
ai ask "你好"

# 3. 查看指标
curl http://localhost:9090/api/metrics

# 4. 查看日志
docker compose logs -f
# 或
tail -f /var/log/kai-linux/*.log
```
