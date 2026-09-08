# KAI Linux — 虚拟机镜像构建指南

## 关于 iOS 镜像的重要说明

**iOS 系统镜像无法制作、也无法在虚拟机中运行。** 原因：

1. iOS 是 Apple 的闭源专有操作系统
2. iOS 仅运行在 Apple 自家芯片（A 系列 / M 系列）上
3. Apple 不发布面向虚拟机的 iOS 镜像

任何声称"可在虚拟机安装的 iOS 镜像"都是虚假或非法的。

---

## 替代方案：KAI Linux 虚拟机镜像

KAI Linux 是完整的 AI 增强 Linux 系统，可在以下虚拟机中运行：

| 虚拟机 | 镜像格式 | 支持 |
|---|---|---|
| QEMU / KVM | qcow2 / ISO | ✅ 完整 |
| VirtualBox | ISO / VDI | ✅ 完整 |
| VMware | ISO / VMDK | ✅ 完整 |
| Docker | 容器镜像 | ✅ 最轻量 |

---

## 方案一：QEMU 磁盘镜像（推荐）

在**有完整 Linux 环境的机器**上运行：

```bash
# 1. 安装依赖
sudo apt install qemu-img debootstrap parted

# 2. 构建镜像（8GB，含 KAI Linux 全套代码 + 预装依赖）
sudo ./build_qemu_image.sh --size 8G --with-ui

# 3. 启动虚拟机
./run_vm.sh --kvm --mem 4096 --smp 4
```

---

## 方案二：可启动 ISO

```bash
# 1. 安装依赖
sudo apt install debootstrap genisoimage xorriso syslinux-utils

# 2. 构建 ISO
sudo ./build_iso.sh

# 3. 在 VirtualBox/VMware 中挂载 ISO 启动
```

---

## 方案三：Docker（最快，无需虚拟机）

```bash
cd kai/deploy
docker-compose up -d

# 或直接构建
docker build -f Dockerfile -t kai-linux ../../
docker run -it --rm \
  -e DEEPSEEK_API_KEY=sk-xxx \
  -p 8080:8080 -p 9090:9090 \
  kai-linux web
```

---

## 登录信息

| 项 | 值 |
|---|---|
| 用户名 | `kai` |
| 密码 | `kai123` |
| root 密码 | `root123` |
| 源码位置 | `/opt/kai-linux` |

---

## 启动后

```bash
# TUI 界面
kai-tui

# Web Dashboard（访问 http://localhost:8080）
kai-web --port 8080

# 性能监控（访问 http://localhost:9090）
kai-monitor --port 9090

# 设置 API Key
export DEEPSEEK_API_KEY=sk-xxx
# 或
export KIMI_API_KEY=sk-xxx
```

---

## 端口映射

| 服务 | 容器内端口 | 宿主机端口 |
|---|---|---|
| Web Dashboard | 8080 | 8080 |
| Monitor | 9090 | 9090 |
| WebSocket | 9998 | 9998 |
| SSH | 22 | 2222 |
