# SPDX-License-Identifier: MIT
"""
multi_model.py — 多模型并行推理 + 多数投票（ROADMAP 2.1）

支持：
  - 同一请求同时调用多个后端（DeepSeek + Kimi）
  - 多数投票 / 加权平均融合
  - 交叉验证降低幻觉
  - 单模型失败自动降级

用法：
    from multi_model import MultiModelEnsemble
    ens = MultiModelEnsemble()
    result = ens.infer("分析系统负载")
"""

import os
import json
import time
import hashlib
import threading
from typing import Dict, List, Any, Optional, Tuple
from dataclasses import dataclass, field
from concurrent.futures import ThreadPoolExecutor, as_completed

try:
    from ai_linux.client import AIClient
    HAS_SDK = True
except ImportError:
    HAS_SDK = False


@dataclass
class ModelResult:
    backend: str
    model: str
    text: str
    confidence: int
    latency_ms: float
    error: str = ""

    def to_dict(self) -> Dict:
        return {
            "backend": self.backend,
            "model": self.model,
            "text": self.text,
            "confidence": self.confidence,
            "latency_ms": self.latency_ms,
            "error": self.error,
        }


@dataclass
class EnsembleResult:
    text: str
    confidence: int
    agreement: float          # 模型间一致性 0-1
    models_used: int
    results: List[ModelResult] = field(default_factory=list)
    vote_counts: Dict[str, int] = field(default_factory=dict)
    latency_ms: float = 0.0

    def to_dict(self) -> Dict:
        return {
            "text": self.text,
            "confidence": self.confidence,
            "agreement": self.agreement,
            "models_used": self.models_used,
            "results": [r.to_dict() for r in self.results],
            "vote_counts": self.vote_counts,
            "latency_ms": self.latency_ms,
        }


class MultiModelEnsemble:
    """
    多模型集成推理

    策略：
      - vote: 多数投票
      - weighted: 加权平均
      - best: 取置信度最高
    """

    def __init__(self, strategy: str = "weighted"):
        self.strategy = strategy
        self._clients: Dict[str, Any] = {}
        self._init_clients()

    def _init_clients(self):
        """初始化各后端客户端"""
        deepseek_key = os.getenv("DEEPSEEK_API_KEY", "")
        kimi_key = os.getenv("KIMI_API_KEY", "")

        if HAS_SDK:
            if deepseek_key:
                try:
                    self._clients["deepseek"] = AIClient(
                        backend="deepseek", api_key=deepseek_key)
                except Exception:
                    pass
            if kimi_key:
                try:
                    self._clients["kimi"] = AIClient(
                        backend="kimi", api_key=kimi_key)
                except Exception:
                    pass
        else:
            # 无 SDK 时用简单的 API 调用
            self._clients = {}

    def _call_backend(self, backend: str, prompt: str) -> ModelResult:
        """调用单个后端"""
        start = time.time()
        try:
            if HAS_SDK and backend in self._clients:
                client = self._clients[backend]
                resp = client.ask(prompt, use_cache=False)
                return ModelResult(
                    backend=backend,
                    model=resp.model,
                    text=resp.text,
                    confidence=resp.confidence,
                    latency_ms=resp.latency_ms,
                )
            else:
                return ModelResult(
                    backend=backend, model="", text="",
                    confidence=0, latency_ms=(time.time()-start)*1000,
                    error="SDK 未安装或密钥未设置",
                )
        except Exception as e:
            return ModelResult(
                backend=backend, model="", text="",
                confidence=0, latency_ms=(time.time()-start)*1000,
                error=str(e),
            )

    def infer(self, prompt: str,
              system: Optional[str] = None,
              backends: Optional[List[str]] = None) -> EnsembleResult:
        """
        并行推理，融合多个模型结果
        """
        start = time.time()
        targets = backends or list(self._clients.keys()) or ["deepseek", "kimi"]

        # 并行调用
        results: List[ModelResult] = []
        with ThreadPoolExecutor(max_workers=len(targets)) as pool:
            futures = {
                pool.submit(self._call_backend, b, prompt): b
                for b in targets
            }
            for future in as_completed(futures):
                results.append(future.result())

        # 过滤成功结果
        ok_results = [r for r in results if not r.error]
        if not ok_results:
            return EnsembleResult(
                text="所有模型均失败",
                confidence=0,
                agreement=0,
                models_used=0,
                results=results,
                latency_ms=(time.time()-start)*1000,
            )

        # 融合
        if self.strategy == "vote":
            text, conf, votes = self._majority_vote(ok_results)
        elif self.strategy == "best":
            text, conf, votes = self._best_result(ok_results)
        else:  # weighted
            text, conf, votes = self._weighted_average(ok_results)

        # 一致性 = 达成共识的模型数 / 总成功模型数
        agreement = max(votes.values()) / len(ok_results) if ok_results else 0

        return EnsembleResult(
            text=text,
            confidence=conf,
            agreement=round(agreement, 2),
            models_used=len(ok_results),
            results=results,
            vote_counts=votes,
            latency_ms=(time.time()-start)*1000,
        )

    def _majority_vote(self, results: List[ModelResult]) -> Tuple[str, int, Dict]:
        """多数投票（对文本做简单分类）"""
        votes: Dict[str, int] = {}
        # 简化：按文本前 20 字符做"投票桶"
        for r in results:
            key = r.text[:20].strip() or "empty"
            votes[key] = votes.get(key, 0) + 1

        best = max(votes.items(), key=lambda x: x[1])
        # 取该桶中置信度最高的完整文本
        best_text = ""
        best_conf = 0
        for r in results:
            if r.text[:20].strip() == best[0] or r.text[:20].strip() == "":
                if r.confidence > best_conf:
                    best_conf = r.confidence
                    best_text = r.text

        return best_text, best_conf, votes

    def _weighted_average(self, results: List[ModelResult]) -> Tuple[str, int, Dict]:
        """加权平均（置信度加权）"""
        total_weight = sum(r.confidence for r in results) or 1
        # 取置信度最高的作为主文本
        best = max(results, key=lambda r: r.confidence)
        avg_conf = int(sum(r.confidence for r in results) / len(results))
        votes = {r.backend: r.confidence for r in results}
        return best.text, avg_conf, votes

    def _best_result(self, results: List[ModelResult]) -> Tuple[str, int, Dict]:
        """取置信度最高"""
        best = max(results, key=lambda r: r.confidence)
        votes = {r.backend: r.confidence for r in results}
        return best.text, best.confidence, votes

    def cross_validate(self, prompt: str, threshold: int = 70) -> bool:
        """
        交叉验证：检查多个模型是否达成一致
        返回 True 表示结果可信
        """
        result = self.infer(prompt)
        return result.agreement >= 0.6 and result.confidence >= threshold


if __name__ == "__main__":
    ens = MultiModelEnsemble(strategy="weighted")
    result = ens.infer("用一句话总结 Linux 调度器")
    print(json.dumps(result.to_dict(), ensure_ascii=False, indent=2))
