# SPDX-License-Identifier: MIT
"""
prompt_engine.py — 提示词模板引擎（ROADMAP 2.2）

支持：
  - 可配置的提示词模板
  - 变量插值
  - 模板继承
  - 系统提示词 + 用户提示词
  - 模板版本管理

用法：
    from prompt_engine import PromptEngine
    pe = PromptEngine()
    pe.register_template("sched", "你是调度专家。分析 {pid} 的调度状态。")
    prompt = pe.render("sched", pid=1234)
"""

import re
import os
from typing import Dict, Any, Optional
from dataclasses import dataclass, field


@dataclass
class Template:
    name: str
    system: str = ""
    user: str = ""
    description: str = ""
    version: str = "1.0"
    variables: Dict[str, str] = field(default_factory=dict)
    inherits: Optional[str] = None

    def to_dict(self) -> Dict:
        return {
            "name": self.name,
            "system": self.system,
            "user": self.user,
            "description": self.description,
            "version": self.version,
            "variables": self.variables,
            "inherits": self.inherits,
        }


class PromptEngine:
    """
    提示词模板引擎
    """

    def __init__(self):
        self._templates: Dict[str, Template] = {}
        self._register_builtins()

    def _register_builtins(self):
        """注册内置模板"""
        self.register_template(Template(
            name="scheduling",
            system="你是一个运行在 Linux 内核中的 AI 调度专家。"
                   "决策选项：promote（升权）/ demote（降权）/ migrate（迁移）/ batch（批处理）/ keep（保持）",
            user="分析进程 {pid} 的调度状态。\n"
                 "CPU 利用率 {cpu_util}%，上下文切换 {nvcsw} 次。\n"
                 "返回 JSON：{{\"decision\":\"...\",\"confidence\":0-100,\"reason\":\"...\"}}",
            description="调度分析模板",
            variables={"pid": "进程ID", "cpu_util": "CPU利用率", "nvcsw": "上下文切换数"},
        ))

        self.register_template(Template(
            name="security",
            system="你是一个 Linux 内核安全专家。宁可误报不可漏报。",
            user="检测进程 {comm} 是否恶意。\n命令参数：{argv}\n"
                 "返回 JSON：{{\"action\":\"allow|block|alert\",\"threat_level\":\"low|medium|high|critical\"}}",
            description="安全检测模板",
            variables={"comm": "进程名", "argv": "命令行参数"},
        ))

        self.register_template(Template(
            name="io_network",
            system="你是一个 Linux 网络分析专家。",
            user="分析连接 {src_ip}:{src_port} → {dst_ip}:{dst_port} ({protocol})。\n"
                 "返回 JSON：{{\"action\":\"pass|drop|redirect|alert\",\"confidence\":0-100}}",
            description="网络分析模板",
            variables={"src_ip": "源IP", "src_port": "源端口", "dst_ip": "目标IP", "dst_port": "目标端口", "protocol": "协议"},
        ))

        self.register_template(Template(
            name="memory",
            system="你是一个 Linux 内存管理专家。",
            user="分析内存使用：总 {total_gb}GB，已用 {used_gb}GB，Swap {swap_gb}GB。\n"
                 "给出优化建议。",
            description="内存分析模板",
            variables={"total_gb": "总内存", "used_gb": "已用", "swap_gb": "Swap"},
        ))

        self.register_template(Template(
            name="code",
            system="你是一个 Linux 系统程序员。只输出代码，不要解释。",
            user="用 {language} 实现：{requirement}",
            description="代码生成模板",
            variables={"language": "语言", "requirement": "需求"},
        ))

    def register_template(self, tpl: Template):
        """注册模板"""
        self._templates[tpl.name] = tpl

    def get_template(self, name: str) -> Optional[Template]:
        return self._templates.get(name)

    def render(self, name: str, **kwargs) -> str:
        """
        渲染模板

        参数：
          name: 模板名
          **kwargs: 变量值

        返回完整提示词（system + user 拼接）
        """
        tpl = self._templates.get(name)
        if not tpl:
            return kwargs.get("prompt", "")

        # 处理继承
        system = tpl.system
        user = tpl.user
        if tpl.inherits and tpl.inherits in self._templates:
            parent = self._templates[tpl.inherits]
            system = parent.system + "\n" + system

        # 变量插值
        try:
            system = system.format(**kwargs)
        except KeyError:
            pass  # 未提供的变量保持原样

        try:
            user = user.format(**kwargs)
        except KeyError:
            pass

        # 拼接
        if system and user:
            return f"{system}\n\n{user}"
        return user or system

    def render_system(self, name: str, **kwargs) -> str:
        """只渲染系统提示词"""
        tpl = self._templates.get(name)
        if not tpl:
            return ""
        try:
            return tpl.system.format(**kwargs)
        except KeyError:
            return tpl.system

    def render_user(self, name: str, **kwargs) -> str:
        """只渲染用户提示词"""
        tpl = self._templates.get(name)
        if not tpl:
            return ""
        try:
            return tpl.user.format(**kwargs)
        except KeyError:
            return tpl.user

    def list_templates(self) -> Dict[str, str]:
        """列出所有模板"""
        return {k: v.description for k, v in self._templates.items()}


if __name__ == "__main__":
    pe = PromptEngine()

    print("可用模板:")
    for name, desc in pe.list_templates().items():
        print(f"  {name}: {desc}")

    print("\n渲染调度模板:")
    prompt = pe.render("scheduling", pid=1234, cpu_util=75, nvcsw=120)
    print(prompt)
