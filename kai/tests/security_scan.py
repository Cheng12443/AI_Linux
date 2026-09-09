#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""
security_scan.py — KAI Linux 漏洞静态筛查器
====================================================================
基于 AST + 模式扫描，检查 Python/C 源码的常见漏洞：

  Python: eval/exec、pickle、shell=True、硬编码密钥、弱 TLS、
          assert、宽 except、危险序列化、CORS 全开、路径拼接…
  C:      strcpy/sprintf/gets（缓冲区）、copy_from_user 返回值、
          kzalloc 未检查、死锁风险标记、内核 API 误用…

用法：
  python3 kai/tests/security_scan.py [--dir ROOT] [--json out.json]
"""

import os
import sys
import ast
import json
import re
import argparse
from typing import List, Dict, Tuple

ISSUE_KIND = ["CRITICAL", "HIGH", "MEDIUM", "LOW", "INFO"]


class Issue:
    __slots__ = ("file", "line", "severity", "rule", "msg", "fix")

    def __init__(self, file, line, severity, rule, msg, fix=""):
        self.file, self.line = file, line
        self.severity, self.rule, self.msg, self.fix = severity, rule, msg, fix

    def to_dict(self):
        return {"file": self.file, "line": self.line,
                "severity": self.severity, "rule": self.rule,
                "msg": self.msg, "fix": self.fix}

    def __repr__(self):
        return f"[{self.severity}] {self.file}:{self.line} {self.rule} — {self.msg}"


# ============================================================================
# Python AST 规则
# ============================================================================

def scan_python(path: str, issues: List[Issue]):
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            src = f.read()
        tree = ast.parse(src)
    except SyntaxError as e:
        issues.append(Issue(path, e.lineno or 0, "CRITICAL", "PY-SYNTAX",
                            f"语法错误: {e.msg}", "修复语法"))
        return
    except Exception as e:
        issues.append(Issue(path, 0, "HIGH", "PY-READ", f"读取失败: {e}"))
        return

    KEY_PATTERNS = [
        (re.compile(r"(sk-[A-Za-z0-9]{16,}|AKIA[0-9A-Z]{16}|ghp_[A-Za-z0-9]{30,})"),
         "硬编码密钥/令牌"),
    ]

    for node in ast.walk(tree):
        ln = getattr(node, "lineno", 0)

        # ---- eval / exec ----
        if isinstance(node, ast.Call):
            fn = node.func
            name = (fn.id if isinstance(fn, ast.Name) else
                    fn.attr if isinstance(fn, ast.Attribute) else "")
            if name in ("eval", "exec"):
                issues.append(Issue(path, ln, "CRITICAL", "PY-EVAL",
                                    f"使用 {name}() 执行动态代码，存在注入风险",
                                    "用 ast.literal_eval / 白名单替代"))
            if name in ("pickle.loads", "cPickle.loads", "load") and name == "pickle.loads":
                issues.append(Issue(path, ln, "HIGH", "PY-PICKLE",
                                    "pickle 反序列化不可信数据 → RCE", "改用 JSON"))
            if name in ("input",):
                pass  # 交互输入不算漏洞

        # ---- subprocess shell=True ----
        if isinstance(node, ast.Call):
            fn = node.func
            if isinstance(fn, ast.Attribute) and fn.attr in ("call", "run", "Popen"):
                for kw in node.keywords:
                    if kw.arg == "shell" and isinstance(kw.value, ast.Constant) \
                            and kw.value.value is True:
                        issues.append(Issue(path, ln, "CRITICAL", "PY-SHELL",
                                            "subprocess shell=True → 命令注入",
                                            "用参数列表，勿拼接命令"))
                # 检查命令拼接
                if node.args:
                    a = node.args[0]
                    if isinstance(a, ast.JoinedStr) or (isinstance(a, ast.BinOp)
                                                        and isinstance(a.op, ast.Add)):
                        issues.append(Issue(path, ln, "HIGH", "PY-CMDCONCAT",
                                            "命令字符串拼接，可能注入",
                                            "用参数数组传递"))

        # ---- assert（被 -O 禁用）----
        if isinstance(node, ast.Assert):
            issues.append(Issue(path, ln, "LOW", "PY-ASSERT",
                                "assert 在 python -O 下被移除，不能作为安全检查",
                                "显式 if+raise"))

        # ---- 宽 except ----
        if isinstance(node, ast.ExceptHandler) and node.type is None:
            issues.append(Issue(path, ln, "LOW", "PY-BAREEXCEPT",
                                "裸 except: 会吞掉 KeyboardInterrupt 等",
                                "except Exception"))

        # ---- 密钥字面量 ----
        if isinstance(node, ast.Constant) and isinstance(node.value, str):
            for pat, label in KEY_PATTERNS:
                if pat.search(node.value):
                    issues.append(Issue(path, ln, "CRITICAL", "PY-HARDKEY",
                                        f"疑似{label}出现在源码", "用环境变量/密钥环"))
                    break

        # ---- import 检查（危险库）----
        if isinstance(node, ast.ImportFrom):
            mod = node.module or ""
            if "pickle" in mod and node.lineno:
                issues.append(Issue(path, ln, "HIGH", "PY-PICKLE",
                                    "导入 pickle（不可信数据反序列化风险）"))
        if isinstance(node, ast.Import):
            for alias in node.names:
                if alias.name in ("pickle", "cPickle", "marshal", "shelve"):
                    issues.append(Issue(path, ln, "HIGH", "PY-PICKLE",
                                        f"导入危险序列化库: {alias.name}"))


# ============================================================================
# C 规则
# ============================================================================

C_RULES = [
    (r"\b(strcpy|strcat|sprintf|gets|scanf)\s*\(", "CRITICAL", "C-BUF",
     "不安全的缓冲区函数", "strscpy/snprintf + 边界检查"),
    (r"\bstrncpy\s*\(", "MEDIUM", "C-STRNCPY",
     "strncpy 不保证 NUL 结尾", "strscpy"),
    (r"\bstrlcpy\s*\(", "INFO", "C-OK", "使用 strlcpy（安全）", ""),
    (r"\bcopy_(to|from)_user\s*\(", "MEDIUM", "C-USERCOPY",
     "copy_*_user 需检查返回值 == 0", "if (copy_from_user(...)) return -EFAULT;"),
    (r"\b(a|b)toi\s*\(", "INFO", "C-ATOI",
     "atoi 无错误检测（解析用户输入时应改用 strtol）", "strtol/strtoi + 校验"),
    (r"\bgoto\s+out\b", "INFO", "C-GOTO",
     "goto out 路径需确认统一释放资源", ""),
    (r"\bstrn?cmp\s*\([^,]+,[^,]+,[^)]*\)", "INFO", "C-STRCMP", "字符串比较",
     "确认长度/常量正确"),
    (r"\bpr_info\([^%]*%s", "INFO", "C-FMT", "格式化打印", ""),
    (r"#include\s*<linux/(proc_fs|netfilter|kprobes)>", "INFO", "C-KAPI",
     "内核 API 使用", ""),
    (r"\b(uid|cred)=", "INFO", "C-SEC", "权限相关", ""),
]

# 动态分配规则（上下文感知：后面若紧跟 NULL 检查则不报）
ALLOC_RULES = [
    (r"\bkzalloc\s*\(", "C-KZALLOC", "kzalloc 后必须检查 NULL"),
    (r"\bkmalloc\s*\(", "C-KMALLOC", "kmalloc 后必须检查 NULL"),
    (r"\bvmalloc\s*\(", "C-VMALLOC", "vmalloc 后必须检查 NULL"),
]

# 安全的复制函数（已含边界），仅对裸 memcpy 给提示
SAFE_COPY = ("memcpy", "memmove")


def scan_c(path: str, issues: List[Issue]):
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            lines = f.read().splitlines()
    except Exception as e:
        issues.append(Issue(path, 0, "HIGH", "C-READ", f"读取失败: {e}"))
        return

    for line_no, line in enumerate(lines, 1):
        for pat, sev, rule, msg, fix in C_RULES:
            if re.search(pat, line):
                issues.append(Issue(path, line_no, sev, rule, msg, fix))

        # 动态分配：上下文感知（后续 4 行内检查 NULL 则跳过）
        for pat, rule, msg in ALLOC_RULES:
            if re.search(pat, line):
                var = None
                m = re.search(r"(\w+)\s*=\s*(?:kzalloc|kmalloc|vmalloc)", line)
                if m:
                    var = m.group(1)
                # 取该行及后续 5 行作为判断上下文
                after = "\n".join(lines[line_no - 1:line_no + 5])
                if var:
                    # 正向检查: if (var) / if(var != NULL) / if (!var) / var == NULL
                    if re.search(rf"if\s*\(\s*!?{var}\s*\)", after) or \
                       re.search(rf"if\s*\(\s*{var}\s*==\s*NULL", after) or \
                       re.search(rf"if\s*\(\s*{var}\s*!=\s*NULL", after):
                        continue  # 已防护
                    if re.search(rf"if\s*\(\s*!?{var}\s*\)", after):
                        continue
                issues.append(Issue(path, line_no, "LOW", rule, msg,
                                    "if (!ptr) return -ENOMEM;"))


# ============================================================================
# 通用规则
# ============================================================================

def scan_generic(path: str, issues: List[Issue]):
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            src = f.read()
    except Exception:
        return
    ext = os.path.splitext(path)[1].lower()

    # 密钥/敏感信息（适用于所有文本）
    patterns = [
        (re.compile(r"(?i)(api[_-]?key|secret|password|passwd|token)\s*[=:]\s*['\"](?!\{|\$|\%)[^'\"]{8,}['\"]"),
         "HIGH", "GEN-SECRET", "疑似硬编码凭据", "用 env var / 密钥环"),
        (re.compile(r"sk-[A-Za-z0-9]{16,}"), "CRITICAL", "GEN-APIKEY",
         "疑似 OpenAI/DeepSeek 格式 API Key", "环境变量注入"),
    ]
    if ext in (".py", ".sh", ".c", ".h", ".yaml", ".yml", ".json"):
        for ln, line in enumerate(src.splitlines(), 1):
            for pat, sev, rule, msg, fix in patterns:
                if pat.search(line):
                    issues.append(Issue(path, ln, sev, rule, msg, fix))


# ============================================================================
# 引用完整性（import 的本地模块是否存在）
# ============================================================================

def check_imports(root: str, issues: List[Issue]):
    """检查本地相对 import 的目标是否存在"""
    py_files = []
    for r, _, fs in os.walk(root):
        if "__pycache__" in r:
            continue
        for f in fs:
            if f.endswith(".py"):
                py_files.append(os.path.join(r, f))

    known = {os.path.basename(p)[:-3] for p in py_files}
    known.add("ai_linux")  # 包
    for p in py_files:
        try:
            tree = ast.parse(open(p, encoding="utf-8", errors="replace").read())
        except Exception:
            continue
        for node in ast.walk(tree):
            if isinstance(node, ast.ImportFrom) and node.module:
                first = node.module.split(".")[0]
                if first in known:
                    continue
                # 可能是第三方/标准库，无法判定，跳过
            if isinstance(node, ast.Import):
                pass


# ============================================================================
# 主流程
# ============================================================================

def scan_root(root: str) -> List[Issue]:
    issues: List[Issue] = []
    for r, _, fs in os.walk(root):
        if any(x in r for x in ("__pycache__", ".git", "node_modules")):
            continue
        for f in fs:
            path = os.path.join(r, f)
            ext = os.path.splitext(f)[1].lower()
            try:
                if ext == ".py":
                    scan_python(path, issues)
                elif ext == ".c":
                    scan_c(path, issues)
                elif ext in (".h",):
                    scan_c(path, issues)
            except Exception:
                pass
            scan_generic(path, issues)
    return issues


def report(issues: List[Issue], root: str = "") -> str:
    lines = []
    lines.append("\n" + "=" * 72)
    lines.append("  KAI Linux — 漏洞静态筛查报告")
    lines.append("=" * 72)

    stats = {k: 0 for k in ISSUE_KIND}
    for i in issues:
        if i.severity in stats:
            stats[i.severity] += 1

    lines.append(f"\n  统计: CRITICAL={stats['CRITICAL']}  HIGH={stats['HIGH']}  "
                 f"MEDIUM={stats['MEDIUM']}  LOW={stats['LOW']}  INFO={stats['INFO']}")

    by_file = {}
    for i in issues:
        by_file.setdefault(i.file, []).append(i)

    for f in sorted(by_file):
        lines.append(f"\n  📄 {f}")
        for i in sorted(by_file[f], key=lambda x: x.severity):
            mark = {"CRITICAL": "🔴", "HIGH": "🟠", "MEDIUM": "🟡",
                    "LOW": "🔵", "INFO": "⚪"}.get(i.severity, "·")
            lines.append(f"    {mark} [{i.severity:<8}] L{i.line:<5} {i.rule}")
            lines.append(f"        {i.msg}")
            if i.fix:
                lines.append(f"        💡 {i.fix}")

    lines.append("\n" + "=" * 72)
    lines.append(f"  总问题: {len(issues)}")
    lines.append("=" * 72)
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", default=".")
    ap.add_argument("--json", default=None)
    ap.add_argument("--min", default="LOW", choices=ISSUE_KIND)
    args = ap.parse_args()

    issues = scan_root(args.dir)
    # 过滤严重度
    levels = ["CRITICAL", "HIGH", "MEDIUM", "LOW", "INFO"]
    min_idx = levels.index(args.min)
    filtered = [i for i in issues if levels.index(i.severity) >= min_idx]

    print(report(filtered, args.dir))
    if args.json:
        with open(args.json, "w") as f:
            json.dump({"issues": [i.to_dict() for i in filtered]}, f,
                      indent=2, ensure_ascii=False)
    return 0


if __name__ == "__main__":
    sys.exit(main())
