FROM ubuntu:24.04

LABEL maintainer="KAI Linux Team"
LABEL description="KAI Linux — AI-powered Linux kernel system (all modules)"
LABEL version="2.0.0"

# 基础依赖
RUN apt-get update && apt-get install -y \
    build-essential gcc make \
    libncurses-dev libelf-dev libssl-dev liburing-dev bc kmod \
    clang llvm libbpf-dev \
    linux-tools-common linux-tools-generic \
    python3 python3-pip python3-venv \
    curl wget git vim \
    htop iotop sysstat \
    iproute2 net-tools iputils-ping dnsutils tcpdump \
    supervisor procps \
    && rm -rf /var/lib/apt/lists/*

# Python 依赖
RUN pip3 install --break-system-packages \
    websockets pyyaml requests

# 创建工作目录
WORKDIR /opt/kai-linux

# 复制源码（构建上下文为 ai-linux 根目录）
COPY . /opt/kai-linux/

# 编译 UI（ai-tui 需要 ncurses；ai 需要 curl）
RUN cd ui && \
    make ai 2>/dev/null || true && \
    make ai-tui 2>/dev/null || true && \
    make ai-web 2>/dev/null || true && \
    make ai-monitor 2>/dev/null || true && \
    echo "UI 编译完成"

# 编译 Layer2 网关
RUN cd layer2 && \
    mkdir -p build && \
    gcc -O2 -o build/ai_layer2_gateway src/ai_layer2_gateway.c -lpthread 2>/dev/null || \
    echo "Layer2 网关编译失败（继续构建）"

# 创建符号链接
RUN ln -sf /opt/kai-linux/ui/build/ai /usr/local/bin/ai && \
    ln -sf /opt/kai-linux/ui/build/ai-tui /usr/local/bin/kai-tui && \
    ln -sf /opt/kai-linux/ui/build/ai-web /usr/local/bin/kai-web && \
    ln -sf /opt/kai-linux/ui/build/ai-monitor /usr/local/bin/kai-monitor && \
    ln -sf /opt/kai-linux/layer2/build/ai_layer2_gateway /usr/local/bin/kai-gateway && \
    ln -sf /opt/kai-linux/sdk/python/demo.py /usr/local/bin/kai-demo && \
    ln -sf /opt/kai-linux/sdk/python/ai_ws_server.py /usr/local/bin/kai-ws && \
    ln -sf /opt/kai-linux/tools/perf_profiler.py /usr/local/bin/kai-perf && \
    chmod +x /usr/local/bin/kai-* 2>/dev/null || true

# 创建数据目录
RUN mkdir -p /var/lib/kai-linux/models \
    /var/log/kai-linux \
    /etc/kai-linux \
    /run/kai-linux

# 默认配置
RUN cp /opt/kai-linux/layer2/config/layer2.yaml /etc/kai-linux/ 2>/dev/null || true

# 环境变量
ENV DEEPSEEK_API_KEY=""
ENV KIMI_API_KEY=""
ENV KAI_BACKEND=deepseek
ENV KAI_MODE=async
ENV PYTHONUNBUFFERED=1

# 端口
EXPOSE 8080 9090 9998 9999

# 启动脚本
COPY kai/deploy/entrypoint.sh /entrypoint.sh
COPY kai/deploy/supervisord.conf /etc/supervisor/conf.d/kai.conf
RUN chmod +x /entrypoint.sh

# 健康检查
HEALTHCHECK --interval=30s --timeout=10s --start-period=10s --retries=3 \
    CMD curl -sf http://localhost:8080/api/status || curl -sf http://localhost:9090/ || exit 1

ENTRYPOINT ["/entrypoint.sh"]
CMD ["all"]
