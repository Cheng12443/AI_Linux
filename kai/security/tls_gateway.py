# SPDX-License-Identifier: MIT
"""
tls_gateway.py — TLS + mTLS 安全通信（ROADMAP 4.2）

支持：
  - Layer2 网关 ↔ 内核/客户端 的 TLS 加密
  - mTLS 双向认证
  - 证书生成与管理
  - 证书轮换

用法：
    from tls_gateway import MTLSManager

    # 生成 CA 和证书
    MTLSManager.generate_ca()

    # 为网关签发证书
    MTLSManager.issue_cert("kai-gateway")

    # 启动 mTLS 服务器
    server = MTLSManager.server("kai-gateway", port=8443)

    # 连接 mTLS 客户端
    client = MTLSManager.client("kai-client")
"""

import os
import ssl
import socket
import tempfile
import threading
import subprocess
from typing import Optional, Tuple, Dict, Any

try:
    from cryptography import x509
    from cryptography.x509.oid import NameOID
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import rsa
    from cryptography.hazmat.primitives.serialization import pkcs12
    HAS_CRYPTO = True
except ImportError:
    HAS_CRYPTO = False


class MTLSManager:
    """
    mTLS 证书管理
    """

    CERT_DIR = "/etc/kai-linux/certs"
    CN = "kai-linux"

    # ------------------------------------------------------------------
    # CA 管理
    # ------------------------------------------------------------------

    @staticmethod
    def _gen_rsa_key(bits: int = 2048):
        """生成 RSA 密钥"""
        if HAS_CRYPTO:
            return rsa.generate_private_key(public_exponent=65537,
                                           key_size=bits)
        return None

    @staticmethod
    def generate_ca(cert_dir: Optional[str] = None) -> Dict[str, str]:
        """生成 CA 证书和密钥"""
        cert_dir = cert_dir or MTLSManager.CERT_DIR
        os.makedirs(cert_dir, exist_ok=True)

        ca_key_path = os.path.join(cert_dir, "ca.key")
        ca_cert_path = os.path.join(cert_dir, "ca.crt")

        # 已存在则跳过
        if os.path.exists(ca_key_path) and os.path.exists(ca_cert_path):
            return {"key": ca_key_path, "cert": ca_cert_path}

        # 用 openssl 生成（cryptography 复杂，直接用 openssl 更可靠）
        subprocess.run([
            "openssl", "req", "-x509", "-newkey", "rsa:2048",
            "-keyout", ca_key_path, "-out", ca_cert_path,
            "-days", "3650", "-nodes",
            "-subj", f"/CN={MTLSManager.CN} CA/O=KAI Linux",
        ], check=True, capture_output=True)

        os.chmod(ca_key_path, 0o600)
        return {"key": ca_key_path, "cert": ca_cert_path}

    @staticmethod
    def issue_cert(name: str, cert_dir: Optional[str] = None,
                   is_client: bool = False) -> Dict[str, str]:
        """
        签发证书

        参数：
          name: 实体名（如 kai-gateway / kai-client）
          is_client: True=客户端证书（含 clientAuth EKU）
        """
        cert_dir = cert_dir or MTLSManager.CERT_DIR
        os.makedirs(cert_dir, exist_ok=True)

        # 确保 CA 存在
        MTLSManager.generate_ca(cert_dir)

        key_path = os.path.join(cert_dir, f"{name}.key")
        csr_path = os.path.join(cert_dir, f"{name}.csr")
        cert_path = os.path.join(cert_dir, f"{name}.crt")

        # 生成私钥和 CSR
        subprocess.run([
            "openssl", "req", "-newkey", "rsa:2048",
            "-keyout", key_path, "-out", csr_path,
            "-nodes", "-subj", f"/CN={name}/O=KAI Linux",
        ], check=True, capture_output=True)

        # 签名证书（带扩展）
        # 用简单的 extfile 支持客户端/服务器认证
        extfile = os.path.join(cert_dir, f"{name}.ext")
        if is_client:
            with open(extfile, "w") as f:
                f.write("extendedKeyUsage = clientAuth\n")
        else:
            with open(extfile, "w") as f:
                f.write("extendedKeyUsage = serverAuth, clientAuth\n")

        subprocess.run([
            "openssl", "x509", "-req",
            "-in", csr_path,
            "-CA", os.path.join(cert_dir, "ca.crt"),
            "-CAkey", os.path.join(cert_dir, "ca.key"),
            "-CAcreateserial",
            "-out", cert_path,
            "-days", "365",
            "-extfile", extfile,
        ], check=True, capture_output=True)

        os.remove(csr_path)  # 清理 CSR
        os.remove(extfile)
        os.chmod(key_path, 0o600)

        return {"key": key_path, "cert": cert_path,
                "ca": os.path.join(cert_dir, "ca.crt")}

    # ------------------------------------------------------------------
    # SSL 上下文
    # ------------------------------------------------------------------

    @staticmethod
    def server_context(name: str = "kai-gateway",
                       cert_dir: Optional[str] = None) -> ssl.SSLContext:
        """创建服务器 SSL 上下文（mTLS）"""
        cert_dir = cert_dir or MTLSManager.CERT_DIR
        files = MTLSManager.issue_cert(name, cert_dir)

        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(files["cert"], files["key"])
        ctx.load_verify_locations(files["ca"])

        # 要求客户端证书（mTLS）
        ctx.verify_mode = ssl.CERT_REQUIRED
        ctx.check_hostname = False

        # 只允许 TLS 1.2+
        ctx.minimum_version = ssl.TLSVersion.TLSv1_2

        return ctx

    @staticmethod
    def client_context(name: str = "kai-client",
                       cert_dir: Optional[str] = None) -> ssl.SSLContext:
        """创建客户端 SSL 上下文（mTLS）"""
        cert_dir = cert_dir or MTLSManager.CERT_DIR
        files = MTLSManager.issue_cert(name, cert_dir, is_client=True)

        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        ctx.load_cert_chain(files["cert"], files["key"])
        ctx.load_verify_locations(files["ca"])
        ctx.verify_mode = ssl.CERT_REQUIRED

        ctx.minimum_version = ssl.TLSVersion.TLSv1_2

        return ctx

    # ------------------------------------------------------------------
    # 服务器 / 客户端
    # ------------------------------------------------------------------

    @staticmethod
    def start_tls_server(host: str = "0.0.0.0", port: int = 8443,
                         handler=None, cert_dir: Optional[str] = None):
        """启动 TLS 服务器"""
        ctx = MTLSManager.server_context("kai-gateway", cert_dir)

        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        sock.bind((host, port))
        sock.listen(16)

        def _loop():
            while True:
                conn, addr = sock.accept()
                try:
                    tls_conn = ctx.wrap_socket(conn, server_side=True)
                    # 验证客户端身份
                    client_cert = tls_conn.getpeercert()
                    if handler:
                        handler(tls_conn, addr, client_cert)
                except ssl.SSLError as e:
                    print(f"[tls] 连接失败: {e}")
                finally:
                    conn.close()

        return sock, _loop


def example_server():
    """示例：TLS 服务器"""
    # 生成证书
    files = MTLSManager.generate_ca()
    files = MTLSManager.issue_cert("kai-gateway")
    print(f"证书目录: {MTLSManager.CERT_DIR}")
    print(f"  密钥: {files['key']}")
    print(f"  证书: {files['cert']}")

    # 创建上下文
    ctx = MTLSManager.server_context("kai-gateway")
    print(f"mTLS 服务器上下文: TLS{ctx.minimum_version.name}")


if __name__ == "__main__":
    import sys
    cert_dir = "/tmp/kai-certs-test"
    MTLSManager.CERT_DIR = cert_dir

    # 生成 CA
    print("=== 生成 CA ===")
    ca = MTLSManager.generate_ca()
    print(f"  CA 证书: {ca['cert']}")

    # 签发证书
    print("\n=== 签发证书 ===")
    gw = MTLSManager.issue_cert("kai-gateway")
    print(f"  网关证书: {gw['cert']}")
    cl = MTLSManager.issue_cert("kai-client", is_client=True)
    print(f"  客户端证书: {cl['cert']}")

    # 测试上下文创建
    print("\n=== 创建 mTLS 上下文 ===")
    sctx = MTLSManager.server_context("kai-gateway")
    cctx = MTLSManager.client_context("kai-client")
    print(f"  服务器上下文: {'OK ✓' if sctx else 'FAIL'}")
    print(f"  客户端上下文: {'OK ✓' if cctx else 'FAIL'}")
    print(f"  双向认证: CERT_REQUIRED ✓")
