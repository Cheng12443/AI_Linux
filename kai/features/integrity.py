# SPDX-License-Identifier: MIT
"""
integrity.py — 模型完整性检查 + 签名验证（ROADMAP 4.1）

支持：
  - SHA256 哈希校验
  - 模型清单（manifest）验证
  - 签名验证（HMAC-SHA256）
  - 完整性报告

用法：
    from integrity import IntegrityChecker
    ic = IntegrityChecker()
    ic.compute_hash("model.bin")
    ic.verify("model.bin", expected_hash)
"""

import os
import hashlib
import hmac
import json
from typing import Dict, Optional, List
from dataclasses import dataclass


@dataclass
class IntegrityReport:
    file: str
    sha256: str
    size: int
    verified: bool
    signature_valid: bool = False
    error: str = ""


class IntegrityChecker:
    """
    模型完整性检查器
    """

    def __init__(self, secret: Optional[str] = None):
        self.secret = secret or os.getenv("KAI_INTEGRITY_SECRET", "")

    def compute_hash(self, filepath: str, chunk_size: int = 8192) -> str:
        """计算文件 SHA256"""
        h = hashlib.sha256()
        with open(filepath, "rb") as f:
            while True:
                chunk = f.read(chunk_size)
                if not chunk:
                    break
                h.update(chunk)
        return h.hexdigest()

    def compute_hash_bytes(self, data: bytes) -> str:
        """计算字节数据 SHA256"""
        return hashlib.sha256(data).hexdigest()

    def verify(self, filepath: str, expected_hash: str) -> IntegrityReport:
        """验证文件哈希"""
        try:
            actual = self.compute_hash(filepath)
            size = os.path.getsize(filepath)
            return IntegrityReport(
                file=filepath,
                sha256=actual,
                size=size,
                verified=actual.lower() == expected_hash.lower(),
            )
        except Exception as e:
            return IntegrityReport(
                file=filepath, sha256="", size=0,
                verified=False, error=str(e),
            )

    def sign(self, filepath: str) -> str:
        """生成 HMAC 签名"""
        if not self.secret:
            return ""
        h = self.compute_hash(filepath)
        return hmac.new(self.secret.encode(), h.encode(), hashlib.sha256).hexdigest()

    def sign_hash(self, file_hash: str) -> str:
        """对哈希签名"""
        if not self.secret:
            return ""
        return hmac.new(self.secret.encode(), file_hash.encode(),
                        hashlib.sha256).hexdigest()

    def verify_signature(self, filepath: str, signature: str) -> bool:
        """验证签名"""
        expected = self.sign(filepath)
        return hmac.compare_digest(expected, signature)

    def verify_manifest(self, manifest_path: str,
                        base_dir: str) -> List[IntegrityReport]:
        """验证模型清单（manifest）"""
        try:
            with open(manifest_path) as f:
                manifest = json.load(f)
        except Exception as e:
            return [IntegrityReport(file=manifest_path, sha256="", size=0,
                                    verified=False, error=f"读取清单失败: {e}")]

        reports = []
        for item in manifest.get("files", []):
            filepath = os.path.join(base_dir, item["path"])
            if not os.path.exists(filepath):
                reports.append(IntegrityReport(
                    file=filepath, sha256="", size=0,
                    verified=False, error="文件不存在"))
                continue

            report = self.verify(filepath, item["sha256"])
            if item.get("signature"):
                report.signature_valid = self.verify_signature(
                    filepath, item["signature"])
            reports.append(report)

        return reports

    def generate_manifest(self, base_dir: str, patterns: List[str],
                          output_path: str):
        """生成清单文件"""
        files = []
        for pattern in patterns:
            import glob
            for f in glob.glob(os.path.join(base_dir, pattern)):
                if os.path.isfile(f):
                    h = self.compute_hash(f)
                    files.append({
                        "path": os.path.relpath(f, base_dir),
                        "sha256": h,
                        "signature": self.sign_hash(h),
                    })

        manifest = {
            "version": "1.0",
            "generated_at": __import__("time").strftime("%Y-%m-%dT%H:%M:%S"),
            "files": files,
        }
        with open(output_path, "w") as f:
            json.dump(manifest, f, indent=2, ensure_ascii=False)
        return manifest


if __name__ == "__main__":
    ic = IntegrityChecker(secret="demo-secret")
    h = ic.compute_hash_bytes(b"hello kai")
    print(f"SHA256: {h}")
    sig = ic.sign_hash(h)
    print(f"签名: {sig}")
    print(f"验证签名: {ic.verify_signature_from_hash(h, sig)}")
