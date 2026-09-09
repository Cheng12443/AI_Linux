// KAI Linux VS Code 扩展主文件
// 功能：系统状态、AI 问答、决策监控、硬件后端
const vscode = require('vscode');
const http = require('http');

/** @param {vscode.ExtensionContext} context */
function activate(context) {
    console.log('KAI Linux 扩展已激活');

    // 配置
    let endpoint = vscode.workspace.getConfiguration('kai').get('endpoint');

    // ---- kai.status：查看系统状态 ----
    const statusCmd = vscode.commands.registerCommand('kai.status', async () => {
        try {
            const data = await httpGet(endpoint + '/api/status');
            const s = JSON.parse(data);

            const panel = vscode.window.createOutputChannel('KAI Status');
            panel.clear();
            panel.appendLine('=== KAI Linux 系统状态 ===');
            panel.appendLine(`CPU: ${s.cpu ? s.cpu.usage + '%' : 'N/A'}`);
            panel.appendLine(`内存: ${s.memory ? s.memory.usage + '%' : 'N/A'}`);
            panel.appendLine(`负载: ${JSON.stringify(s.loadavg)}`);
            panel.appendLine(`运行时间: ${s.uptime}s`);
            panel.appendLine(`后端: ${s.backend}`);
            panel.show();
        } catch (e) {
            vscode.window.showErrorMessage('KAI 服务未连接: ' + e.message);
        }
    });

    // ---- kai.ask：AI 问答 ----
    const askCmd = vscode.commands.registerCommand('kai.ask', async () => {
        const question = await vscode.window.showInputBox({
            prompt: '问 KAI Linux AI',
            placeHolder: '例如：分析当前系统负载'
        });
        if (!question) return;

        vscode.window.withProgress({
            location: vscode.ProgressLocation.Notification,
            title: 'KAI 推理中...'
        }, async () => {
            try {
                const res = await httpPost(endpoint + '/api/ask', {
                    prompt: question
                });
                const data = JSON.parse(res);
                const panel = vscode.window.createOutputChannel('KAI Answer');
                panel.clear();
                panel.appendLine('Q: ' + question);
                panel.appendLine('');
                panel.appendLine((data.content || data.error || '无响应'));
                panel.show();
            } catch (e) {
                vscode.window.showErrorMessage('请求失败: ' + e.message);
            }
        });
    });

    // ---- kai.monitor：打开浏览器监控 ----
    const monitorCmd = vscode.commands.registerCommand('kai.monitor', async () => {
        vscode.env.openExternal(vscode.Uri.parse('http://localhost:9090'));
    });

    // ---- kai.inferFile：推理选中代码 ----
    const inferCmd = vscode.commands.registerCommand('kai.inferFile', async () => {
        const editor = vscode.window.activeTextEditor;
        if (!editor) return;
        const selection = editor.document.getText(editor.selection);
        if (!selection) {
            vscode.window.showInformationMessage('请先选中代码');
            return;
        }
        vscode.window.showInformationMessage(`已选中 ${selection.length} 字符，发送推理…`);
        const res = await httpPost(endpoint + '/api/ask', {
            prompt: `分析以下代码（注意安全性和正确性）：\n\n${selection.slice(0, 3000)}`
        });
        const data = JSON.parse(res);
        const panel = vscode.window.createOutputChannel('KAI Code Review');
        panel.clear();
        panel.appendLine(data.content || data.error || '');
        panel.show();
    });

    // 注册树视图（决策监控）
    const treeProvider = new KaiTreeProvider(endpoint);
    vscode.window.registerTreeDataProvider('kaiDecisions', treeProvider);
    vscode.window.registerTreeDataProvider('kaiBackends', new KaiBackendProvider(endpoint));

    context.subscriptions.push(statusCmd, askCmd, monitorCmd, inferCmd);
    console.log('KAI Linux 命令已注册');
}

function deactivate() {}

// ---- HTTP 辅助 ----
function httpGet(url) {
    return new Promise((resolve, reject) => {
        http.get(url, res => {
            let data = '';
            res.on('data', c => data += c);
            res.on('end', () => resolve(data));
        }).on('error', reject);
    });
}

function httpPost(url, body) {
    return new Promise((resolve, reject) => {
        const u = new URL(url);
        const payload = JSON.stringify(body);
        const req = http.request({
            hostname: u.hostname,
            port: u.port,
            path: u.pathname,
            method: 'POST',
            headers: {
                'Content-Type': 'application/json',
                'Content-Length': Buffer.byteLength(payload)
            }
        }, res => {
            let data = '';
            res.on('data', c => data += c);
            res.on('end', () => resolve(data));
        });
        req.on('error', reject);
        req.write(payload);
        req.end();
    });
}

// ---- 决策树视图 ----
class KaiTreeProvider {
    constructor(endpoint) { this.endpoint = endpoint; }
    getTreeItem(element) { return element; }
    getChildren() {
        return [
            new vscode.TreeItem('调度决策', vscode.TreeItemCollapsibleState.None),
            new vscode.TreeItem('安全事件', vscode.TreeItemCollapsibleState.None),
            new vscode.TreeItem('网络流量', vscode.TreeItemCollapsibleState.None),
        ];
    }
}

class KaiBackendProvider {
    constructor(endpoint) { this.endpoint = endpoint; }
    getTreeItem(element) { return element; }
    getChildren() {
        return [
            new vscode.TreeItem('DeepSeek API', vscode.TreeItemCollapsibleState.None),
            new vscode.TreeItem('Kimi K3 API', vscode.TreeItemCollapsibleState.None),
        ];
    }
}

module.exports = { activate, deactivate };
