# 六子棋 Agent Web Demo

任务 2 初版：选择平台支持的 **19×19 六子棋**，用 Python + Flask + SQLite + 原生 JavaScript 实现人机对战、保存对局、Agent 复盘和棋谱导出。默认全部在本地运行，**不需要 API Key**。

下棋 AI 是自己编写的 `bot.py`。它同时被 Web 后端调用，也可以作为单个 Python 文件提交给竞赛平台。Web 服务本身使用 Flask 等依赖；提交用的 `bot.py` 仅使用 Python 标准库。

## 已实现

| 需求 | 初版实现 |
| --- | --- |
| 前端与后端 | 可点击棋盘 + Flask JSON API |
| 对局信息存储 | SQLite 保存棋局、逐颗落子、AI 决策来源和复盘结果 |
| 自己的 AI 对手 | 先找直接成六、再封堵、再根据连线和中心位置打分 |
| Agent 功能 | 读取棋谱 → 重建棋盘 → 检查直接威胁 → 输出问题、手数、建议 → 保存复盘 |
| 对战后分析 | 终局自动复盘，进行中也可点击分析 |
| 棋谱导出 | TXT 便于阅读；JSON 保留结构化数据 |
| 棋谱回放 | 滑块逐颗查看，回到最新位置继续下棋 |
| 历史恢复 | 刷新恢复同一棋局，可切换最近 50 局 |
| 平台协议 | 单文件 `bot.py`，支持 `arena.v1` 多条 JSON Lines 请求 |
| 版本管理 | 保留已有 Git 历史，新增真实改动的提交；推送 GitHub 后运行自动检查 |

## 立即运行（Windows PowerShell）

需要 Python 3.10 或更高版本。本机已有 `.venv` 时可直接执行最后一条命令。

```powershell
cd D:\计协学习\connect6-agent-demo
python -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r requirements.txt
.\.venv\Scripts\python.exe app.py
```

打开 [本地对弈室](http://127.0.0.1:5000)。不用配置 `.env`，先选择“我的规则 AI · 离线”体验。

你执黑，黑方首回合只下一颗；随后每个回合连续下两颗。AI 执白。任意横、竖、斜方向六子或以上相连立即获胜，棋盘下满和棋。一颗子已经获胜就立刻结束，不会强制补齐第二颗。没有 PASS、禁手和认输动作。

停止服务：在启动它的终端按 `Ctrl+C`。重新启动后数据库仍保留。数据库首次启动自动创建，旧版结构会自动补齐字段。

## 文件职责

```text
bot.py                     棋规 + 自己的下棋 AI + 平台 stdin/stdout 入口
app.py                     Flask 路由、事务、数据库、棋谱导出
agent.py                   本地复盘 Agent、落点校验、可选外部模型适配
static/index.html          页面结构
static/styles.css          页面与棋盘样式
static/app.js              点击、恢复、AI 回合、回放和复盘显示
data/connect6.db            运行后自动生成，Git 忽略
tests/test_demo.py         核心规则、接口和平台协议测试
docs/                      需求、AI 接入、平台核对、GitHub 教程、验收记录
.github/workflows/check.yml 推送后的自动检查
```

## 如何接入自己的纯代码 AI

修改 `bot.py` 中的 `choose_ai_move(board, player)`，保持返回 `(row, col)` 两个整数即可。

- `board` 是 19×19 二维列表；空位为 `None`，棋子为 `"black"` / `"white"`。
- `player` 是当前需要落子的颜色，策略应兼容黑白两方。
- 坐标从 0 开始，`row` 向下，`col` 向右。
- 每次只返回一颗子；两颗子的回合由 Web 后端或平台裁判调度。

平台给的是 `0 / 1 / 2` 棋盘，`decide()` 会把它转换成策略使用的格式。Web 后端和平台入口共用同一份算法，所以优化后可以在浏览器直接试玩，再把 `bot.py` 上传评测。重启 Web 后端使 Python 代码改动生效。

详细说明见 [AI 与 Agent 接入教程](docs/第02课-AI接入.md)。外部大模型接口只是可选扩展，默认关闭，平台提交不使用它。

## 平台提交

已于 2026-11-14 查看[六子棋规则](http://139.196.148.174/connect6)、[学生客户端页面](http://139.196.148.174/client)及该页提供的开发模板。

在学生客户端选择六子棋、Python 3，上传本项目根目录的 **`bot.py`**。客户端按固定版本执行本机评测，再把棋谱交给服务器校验。平台每次决策请求一颗子，硬超时 10,000 ms，stdout 只能输出一行协议 JSON。

提交文件无需 Flask、OpenAI SDK、数据库或 API Key。项目导出的 TXT/JSON 是本 Demo 的格式，不宣称能直接导入平台回放器。完整核对记录见 [平台规则与提交](docs/平台规则与提交.md)。当前已通过本地协议测试，尚未登录平台完成正式评测。

## 数据库与 API

`games` 记录状态、胜者和对手类型；`moves` 记录每颗子的顺序、位置与 AI 原因；`analyses` 按棋局及落子数保存复盘。棋盘由逐颗落子记录重建。复用同一棋谱的已保存复盘，继续落子后会产生新的分析版本。

| 方法 | 路径 | 用途 |
| --- | --- | --- |
| GET | `/api/health` | 后端状态 |
| GET / POST | `/api/games` | 最近 50 局 / 新建棋局 |
| GET | `/api/games/<id>` | 恢复完整棋局与复盘 |
| POST | `/api/games/<id>/moves` | 黑方下一颗，JSON 为 `{"row":9,"col":9}` |
| POST | `/api/games/<id>/ai-turn` | AI 完成本回合所需的一或两颗 |
| POST | `/api/games/<id>/analysis` | Agent 读取并分析已保存棋谱 |
| GET | `/api/games/<id>/export?format=txt` | 下载文本棋谱（坐标从 1 开始） |
| GET | `/api/games/<id>/export?format=json` | 下载 JSON（坐标从 0 开始） |

## 验证与 GitHub

```powershell
.\.venv\Scripts\python.exe -m unittest discover -s tests -v
node --check static/app.js
git log --oneline
```

测试使用临时数据库，不删除现有对局。Node 只用于检查 JavaScript 语法，运行 Web 服务不需要 Node。

第一次推送请按 [GitHub 首次推送教程](docs/GitHub首次推送.md) 操作；每次优化后运行检查、`git commit`、`git push`。本地提交和 GitHub 推送是两个动作，尚未设置远程仓库时不要把“已提交”当成“已上传”。

## 初版能力边界

这是本地单人教学 Demo。AI 使用简单启发式，不承诺棋力；本地 Agent 重点识别直接成六威胁、完整回合后的漏防和开局偏边建议，不进行多回合战术证明。模型扩展也不能保证所有棋局判断正确。本版未加入多用户账号与在线匹配。

开发与演示记录见 [CHANGELOG](CHANGELOG.md) 和 [v0.1 验收记录](docs/v0.1验收记录.md)。
