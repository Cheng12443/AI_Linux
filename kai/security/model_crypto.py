# SPDX-License-Identifier: MIT
"""
model_crypto.py — 模型加密（AES-256-GCM + 签名验证）（ROADMAP 4.1）

支持：
  - AES-256-GCM 模型加密存储
  - 模型签名验证（Ed25519）
  - 加密模型加载 / 保存
  - 密钥管理（系统密钥环）

用法：
    from model_crypto import ModelCrypto

    # 生成密钥对
    ModelCrypto.init_keys()

    # 加密模型
    ModelCrypto.encrypt_model("model.bin", "model.bin.enc")

    # 加载加密模型
    weights = ModelCrypto.load_model("model.bin.enc")
"""

import os
import json
import base64
import hashlib
import hmac
import time
import secrets
import threading
from typing import Dict, Any, Optional, Tuple

try:
    from cryptography.hazmat.primitives.ciphers.aead import AESGCM
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import ed25519, rsa
    HAS_CRYPTO = True
except ImportError:
    HAS_CRYPTO = False

    # 纯 Python 回退：XOR + SHA256（教学演示用，生产用 cryptography）
    class _AESGCMFallback:
        def __init__(self, key: bytes):
            self.key = hashlib.sha256(key).digest()

        def encrypt(self, nonce: bytes, data: bytes, aad: bytes = b"") -> bytes:
            # 简化：XOR + HMAC
            stream = hashlib.sha256(self.key + nonce).digest()
            while len(stream) < len(data):
                stream += hashlib.sha256(stream + nonce).digest()
            encrypted = bytes(a ^ b for a, b in zip(data, stream))
            tag = hmac.new(self.key, nonce + encrypted + aad,
                          hashlib.sha256).digest()[:16]
            return encrypted + tag

        def decrypt(self, nonce: bytes, data: bytes, aad: bytes = b"") -> bytes:
            encrypted, tag = data[:-16], data[-16:]
            expected = hmac.new(self.key, nonce + encrypted + aad,
                               hashlib.sha256).digest()[:16]
            if not hmac.compare_digest(tag, expected):
                raise ValueError("认证失败：数据被篡改")
            stream = hashlib.sha256(self.key + nonce).digest()
            while len(stream) < len(encrypted):
                stream += hashlib.sha256(stream + nonce).digest()
            return bytes(a ^ b for a, b in zip(encrypted, stream))


class ModelCrypto:
    """
    模型加密与签名
    """

    MAGIC = b"KAIMD1"          # 文件魔数
    KEY_PATH = "/etc/kai-linux/keys"

    @staticmethod
    def _aesgcm(key: bytes):
        if HAS_CRYPTO:
            return AESGCM(key)
        return _AESGCMFallback(key)

    # ------------------------------------------------------------------
    # 密钥管理
    # ------------------------------------------------------------------

    @staticmethod
    def generate_keys(key_dir: Optional[str] = None) -> Dict[str, str]:
        """生成模型签名密钥对"""
        key_dir = key_dir or ModelCrypto.KEY_PATH
        os.makedirs(key_dir, exist_ok=True)

        if HAS_CRYPTO:
            private_key = ed25519.Ed25519PrivateKey.generate()
            public_key = private_key.public_key()

            priv_pem = private_key.private_bytes(
                serialization.Encoding.PEM,
                serialization.PrivateFormat.PKCS8,
                serialization.NoEncryption(),
            )
            pub_pem = public_key.public_bytes(
                serialization.Encoding.PEM,
                serialization.PublicFormat.SubjectPublicKeyInfo,
            )
        else:
            # 回退：用随机密钥
            priv_pem = secrets.token_bytes(32)
            pub_pem = secrets.token_bytes(32)

        priv_path = os.path.join(key_dir, "model_signing.key")
        pub_path = os.path.join(key_dir, "model_signing.pub")

        with open(priv_path, "wb") as f:
            f.write(priv_pem)
        os.chmod(priv_path, 0o600)
        with open(pub_path, "wb") as f:
            f.write(pub_pem)

        return {"private": priv_path, "public": pub_path}

    @staticmethod
    def load_signing_key(key_dir: Optional[str] = None):
        """加载签名密钥"""
        key_dir = key_dir or ModelCrypto.KEY_PATH
        priv_path = os.path.join(key_dir, "model_signing.key")
        pub_path = os.path.join(key_dir, "model_signing.pub")

        if not os.path.exists(priv_path):
            ModelCrypto.generate_keys(key_dir)

        with open(priv_path, "rb") as f:
            priv_data = f.read()
        with open(pub_path, "rb") as f:
            pub_data = f.read()

        if HAS_CRYPTO:
            try:
                private_key = serialization.load_pem_private_key(priv_data, None)
                public_key = serialization.load_pem_public_key(pub_data)
                return private_key, public_key
            except Exception:
                return priv_data, pub_data
        return priv_data, pub_data

    # ------------------------------------------------------------------
    # 模型签名
    # ------------------------------------------------------------------

    @staticmethod
    def sign_model(filepath: str, key_dir: Optional[str] = None) -> Dict:
        """签名模型文件"""
        with open(filepath, "rb") as f:
            data = f.read()
        return ModelCrypto.sign_bytes(data, key_dir)

    @staticmethod
    def sign_bytes(data: bytes, key_dir: Optional[str] = None) -> Dict:
        """对数据签名"""
        sha = hashlib.sha256(data).hexdigest()
        priv_key, _ = ModelCrypto.load_signing_key(key_dir)

        if HAS_CRYPTO:
            if hasattr(priv_key, "sign"):
                signature = priv_key.sign(data)
                signature_b64 = base64.b64encode(signature).decode()
            else:
                signature_b64 = ""
        else:
            # 回退：HMAC
            sig = hmac.new(priv_key, data, hashlib.sha256).digest()
            signature_b64 = base64.b64encode(sig).decode()

        return {
            "sha256": sha,
            "signature": signature_b64,
            "algorithm": "ed25519" if HAS_CRYPTO else "hmac-sha256",
            "signed_at": time.strftime("%Y-%m-%dT%H:%M:%SZ"),
            "signer": "kai-linux",
        }

    @staticmethod
    def verify_model(filepath: str, signature: Dict,
                     key_dir: Optional[str] = None) -> bool:
        """验证模型签名"""
        with open(filepath, "rb") as f:
            data = f.read()

        # 1. 验证 SHA256
        actual_sha = hashlib.sha256(data).hexdigest()
        if actual_sha != signature.get("sha256"):
            return False

        # 2. 验证签名
        _, pub_key = ModelCrypto.load_signing_key(key_dir)
        sig = base64.b64decode(signature.get("signature", ""))

        if HAS_CRYPTO and hasattr(pub_key, "verify"):
            try:
                pub_key.verify(sig, data)
                return True
            except Exception:
                return False
        else:
            # 回退 HMAC 验证
            priv_key, _ = ModelCrypto.load_signing_key(key_dir)
            expected = hmac.new(priv_key, data, hashlib.sha256).digest()
            return hmac.compare_digest(expected, sig)

    # ------------------------------------------------------------------
    # 模型加密
    # ------------------------------------------------------------------

    @staticmethod
    def encrypt_model(input_path: str, output_path: str,
                      key: Optional[bytes] = None) -> str:
        """加密模型文件（AES-256-GCM）"""
        key = key or hashlib.sha256(
            secrets.token_bytes(32)).digest()  # 生产环境用 KMS

        with open(input_path, "rb") as f:
            data = f.read()

        nonce = os.urandom(12)
        # AAD 需与解密端完全一致：MAGIC + version bytes
        version_bytes = bytes([1, 0])
        aad = b"kai-model" + b"\x00" + version_bytes

        aes = ModelCrypto._aesgcm(key)
        ciphertext = aes.encrypt(nonce, data, aad)

        # 格式: MAGIC + version(2) + nonce(12) + ciphertext
        header = ModelCrypto.MAGIC + version_bytes + nonce

        with open(output_path, "wb") as f:
            f.write(header + ciphertext)

        return output_path

    @staticmethod
    def decrypt_model(filepath: str, key: bytes) -> bytes:
        """解密模型文件"""
        with open(filepath, "rb") as f:
            data = f.read()

        # 验证魔数
        if data[:6] != ModelCrypto.MAGIC:
            raise ValueError("不是有效的加密模型文件")

        version = data[6:8]
        nonce = data[8:20]
        ciphertext = data[20:]

        aad = b"kai-model" + b"\x00" + version

        aes = ModelCrypto._aesgcm(key)
        plaintext = aes.decrypt(nonce, ciphertext, aad)

        return plaintext

    @staticmethod
    def load_model(filepath: str, key: Optional[bytes] = None) -> bytes:
        """
        加载模型（自动检测是否加密）

        返回模型原始字节
        """
        # 读取前 6 字节检测
        with open(filepath, "rb") as f:
            magic = f.read(6)

        if magic == ModelCrypto.MAGIC:
            # 加密模型，需要密钥
            if key is None:
                key = os.environ.get("KAI_MODEL_KEY")
                if key:
                    key = base64.b64decode(key)
            if key is None:
                raise ValueError(
                    "加密模型需要密钥：设置 KAI_MODEL_KEY 环境变量")
            return ModelCrypto.decrypt_model(filepath, key)
        else:
            # 明文模型
            with open(filepath, "rb") as f:
                return f.read()

    # ------------------------------------------------------------------
    # 便捷：加密 + 签名
    # ------------------------------------------------------------------

    @staticmethod
    def encrypt_bytes(data: bytes, key: bytes) -> bytes:
        """对字节数据加密（便捷）"""
        nonce = os.urandom(12)
        version = bytes([1, 0])
        aad = b"kai-model" + b"\x00" + version
        aes = ModelCrypto._aesgcm(key)
        ct = aes.encrypt(nonce, data, aad)
        return ModelCrypto.MAGIC + version + nonce + ct

    @staticmethod
    def decrypt_bytes(payload: bytes, key: bytes) -> bytes:
        """解密字节数据（便捷）"""
        if payload[:6] != ModelCrypto.MAGIC:
            raise ValueError("不是有效的加密数据")
        version = payload[6:8]
        nonce = payload[8:20]
        ct = payload[20:]
        aad = b"kai-model" + b"\x00" + version
        return ModelCrypto._aesgcm(key).decrypt(nonce, ct, aad)

    @staticmethod
    def encrypt_and_sign(input_path: str, output_path: str,
                         key: Optional[bytes] = None) -> Dict:
        """加密并签名模型"""
        key = key or hashlib.sha256(secrets.token_bytes(32)).digest()
        ModelCrypto.encrypt_model(input_path, output_path, key)
        signature = ModelCrypto.sign_model(output_path)
        signature["encrypted"] = True
        signature["algorithm"] = "AES-256-GCM+" + signature["algorithm"]
        return signature


if __name__ == "__main__":
    import tempfile

    # 生成测试文件
    with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as f:
        f.write(os.urandom(2048))
        test_file = f.name

    # 测试签名
    sig = ModelCrypto.sign_model(test_file)
    print(f"签名: {sig['algorithm']} SHA={sig['sha256'][:16]}...")
    ok = ModelCrypto.verify_model(test_file, sig)
    print(f"验证: {'通过 ✓' if ok else '失败 ✗'}")

    # 测试加密
    key = hashlib.sha256(b"test-key").digest()
    enc_file = test_file + ".enc"
    ModelCrypto.encrypt_model(test_file, enc_file, key)
    data = ModelCrypto.decrypt_model(enc_file, key)
    print(f"加密: {'往返一致 ✓' if data == open(test_file,'rb').read() else '失败 ✗'}")

    os.unlink(test_file)
    os.unlink(enc_file)
