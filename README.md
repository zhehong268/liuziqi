# 六子棋 Agent Demo

这是“电子斗蛐蛐”教学项目的第一版，目标是先跑通一个能下棋的 Web Demo，再逐步加入数据库、Agent 分析、棋谱导出和 GitHub 迭代。

## 本版已实现

- Flask 提供页面和 JSON API。
- SQLite 保存棋局与每一步落子。
- 19×19 六子棋规则：黑方首回合 1 子，之后每回合 2 子，连成 6 子获胜。
- 一个确定性的基线 AI，支持和白方对战。
- 文本棋谱导出接口。

## 启动

在 PowerShell 中执行：

```powershell
python -m venv .venv
.\.venv\Scripts\Activate.ps1
pip install -r requirements.txt
python app.py
```

然后打开 <http://127.0.0.1:5000>。

如果 PowerShell 阻止激活脚本，也可以直接使用虚拟环境解释器：

```powershell
.\.venv\Scripts\python.exe -m pip install -r requirements.txt
.\.venv\Scripts\python.exe app.py
```

## API 速查

```text
POST /api/games                       创建棋局
GET  /api/games/<id>                  获取棋局、棋盘和棋谱
POST /api/games/<id>/moves            黑方落一子，JSON: {"row": 9, "col": 9}
POST /api/games/<id>/ai-turn          白方自动完成本回合
GET  /api/games/<id>/export           下载文本棋谱
```

第一版没有把大模型直接放进落子逻辑。先用规则和启发式 AI 保证棋局状态可靠，下一阶段再通过 Flask 后端调用 OpenAI Responses API 做棋局分析。这样 Agent 的输入是已经保存好的结构化棋谱，分析结果也可以被复现和检查。

## Git 迭代

当前本地仓库已经有连续的功能、文档和安全修复提交。每次完成一个可验证的小功能就提交一次，推送到 GitHub 前再添加远程地址：

```powershell
git remote add origin https://github.com/<你的用户名>/connect6-agent-demo.git
git branch -M main
git push -u origin main
```

不要提交 `.env`、API Key 或 `data/*.db`。
