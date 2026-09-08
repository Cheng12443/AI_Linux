#!/bin/bash
# ============================================================================
# KAI Linux — 内核模块签名脚本（ROADMAP 5.1 Secure Boot）
#
# 对内核模块进行签名，以兼容 Secure Boot。
#
# 用法：
#   ./kernel_sign.sh                      # 签名 build/ 下所有 .ko
#   ./kernel_sign.sh --module xxx.ko      # 签名单个模块
#   ./kernel_sign.sh --enroll             # 注册 MOK 密钥到 UEFI
#
# 依赖：
#   openssl mokutil sbsign（或 sign-file）
# ============================================================================

set -euo pipefail

GREEN='\033[0;32m'; YELLOW='\033[0;33m'; RED='\033[0;31m'; NC='\033[0m'
log()  { echo -e "${GREEN}[sign]${NC} $*"; }
warn() { echo -e "${YELLOW}[sign]${NC} $*"; }
die()  { echo -e "${RED}[sign]${NC} $*" >&2; exit 1; }

KEY_DIR="/etc/kai-linux/keys"
MODULE=""
ENROLL=0

while [[ $# -gt 0 ]]; do
  case $1 in
    --module) MODULE="$2"; shift 2 ;;
    --enroll) ENROLL=1; shift ;;
    --key-dir) KEY_DIR="$2"; shift 2 ;;
    *) shift ;;
  esac
done

# ============================================================================
# 生成密钥（如果不存在）
# ============================================================================
generate_keys() {
  if [ -f "$KEY_DIR/MOK.priv" ]; then
    log "密钥已存在: $KEY_DIR"
    return
  fi

  log "生成签名密钥..."
  mkdir -p "$KEY_DIR"
  cd "$KEY_DIR"

  # 生成 RSA 密钥对
  openssl req -new -x509 -newkey rsa:2048 \
    -keyout MOK.priv -outform DER -out MOK.der \
    -days 3650 -nodes \
    -subj "/CN=KAI Linux Module Signing/"

  # 生成 X509 证书
  openssl req -new -x509 -key MOK.priv -outform DER -out MOK.der \
    -days 3650 -nodes -subj "/CN=KAI Linux/"

  chmod 600 MOK.priv
  log "密钥已生成: $KEY_DIR/MOK.{priv,der}"
}

# ============================================================================
# 签名模块
# ============================================================================
sign_module() {
  local mod="$1"

  if [ ! -f "$mod" ]; then
    warn "模块不存在: $mod"
    return 1
  fi

  # 尝试使用 sign-file（内核自带）
  if [ -f "/usr/src/linux-headers-$(uname -r)/scripts/sign-file" ]; then
    log "签名 (sign-file): $mod"
    /usr/src/linux-headers-$(uname -r)/scripts/sign-file \
      sha256 "$KEY_DIR/MOK.priv" "$KEY_DIR/MOK.der" "$mod"
    return 0
  fi

  # 回退到 sbsign（需 sbsigntool）
  if command -v sbsign >/dev/null 2>&1; then
    log "签名 (sbsign): $mod"
    sbsign --key "$KEY_DIR/MOK.priv" --cert "$KEY_DIR/MOK.der" \
      --output "$mod.signed" "$mod"
    mv "$mod.signed" "$mod"
    return 0
  fi

  die "未找到 sign-file 或 sbsign，请安装 linux-headers 或 sbsigntool"
}

# ============================================================================
# 注册 MOK
# ============================================================================
enroll_mok() {
  if ! command -v mokutil >/dev/null 2>&1; then
    die "未找到 mokutil，请安装 mokutil"
  fi

  log "注册 MOK 密钥到 UEFI..."
  log "系统将重启并要求你在 UEFI 中确认密钥"
  mokutil --import "$KEY_DIR/MOK.der"
  log "请重启系统并在 UEFI MOK 管理界面中确认密钥"
}

# ============================================================================
# 主流程
# ============================================================================
log "=========================================="
log "  KAI Linux 内核模块签名"
log "=========================================="

generate_keys

if [ "$ENROLL" -eq 1 ]; then
  enroll_mok
  exit 0
fi

if [ -n "$MODULE" ]; then
  sign_module "$MODULE"
else
  # 签名所有 .ko
  log "签名所有内核模块..."
  for mod in build/*.ko kai/build/*.ko; do
    [ -f "$mod" ] && sign_module "$mod"
  done
fi

log "完成！"
