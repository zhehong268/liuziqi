# 六子棋

[![Demo checks](https://github.com/zhehong268/liuziqi/actions/workflows/check.yml/badge.svg?branch=main)](https://github.com/zhehong268/liuziqi/actions/workflows/check.yml)

“电子斗蛐蛐”任务 2 的六子棋 Web 应用，用 Flask、SQLite 和原生 JavaScript 编写。可以和 AI 下棋，保存对局，复盘并导出棋谱。

## 运行

需要 Python 3.10 或以上版本。下面是 Windows PowerShell 的启动命令：

```powershell
git clone https://github.com/zhehong268/liuziqi.git
cd liuziqi
python -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r requirements.txt
.\.venv\Scripts\python.exe app.py
```

打开 [http://127.0.0.1:5000](http://127.0.0.1:5000)。默认使用本地代码 AI，不需要 API Key。后续启动只需在项目目录运行最后一条命令，按 `Ctrl+C` 停止。

## 玩法

19×19 棋盘，黑先。黑方首回合下一颗，之后双方每回合各下两颗。横、竖或斜向连成六颗及以上获胜，棋盘下满和棋。第一颗已经成六时立即结束，不用补第二颗。

你执黑，AI 执白。页面还可以：

- 自动保存对局，刷新后接着下。
- 从历史列表打开最近 50 局。
- 分析当前棋局，终局后自动复盘。
- 拖动滑块回放棋谱，回到最新位置继续落子。
- 下载 TXT 或 JSON 棋谱。

## AI 和复盘

下棋算法在 `bot.py` 的 `choose_ai_move(board, player)` 中。先找直接成六的落点，再封堵对手，剩下的位置按连线和中心距离评分。它每次返回一个 `(row, col)`，坐标从 0 开始。

Web 和平台提交共用这个函数。要改自己的策略，修改函数里的评分或搜索逻辑，然后重启后端试玩。

复盘 Agent 在 `agent.py` 中：从数据库读取棋谱，逐颗还原局面，检查黑方落完一回合后是否漏掉白方的直接成六威胁，再给出对应手数和练习建议。分析结果也会保存，刷新后能继续看。

目前的策略比较简单，还没有双子组合搜索。复盘也只检查部分棋形，不一定能发现所有问题。代码中保留了可选的外部模型接口，配置方法见 [AI 接入说明](docs/第02课-AI接入.md)。

## 目录

```text
app.py                 Flask 接口、数据库和棋谱导出
bot.py                 棋规、下棋算法、平台协议入口
agent.py               复盘分析和可选模型接口
static/                HTML、CSS、JavaScript
data/connect6.db        启动时生成的数据库
tests/test_demo.py      规则、API 和协议测试
docs/                  开发说明
```

数据库中的 `games` 保存棋局，`moves` 保存每颗落子，`analyses` 保存复盘。数据库和本地配置已加入 `.gitignore`。

## 平台提交

到[学生客户端](http://139.196.148.174/client)选择六子棋、Python 3，上传根目录的 `bot.py`。

`bot.py` 只用 Python 标准库。它按 `arena.v1` 协议读取 stdin、输出 stdout，每次请求返回一颗子，单次决策限时 10 秒。平台所需的 Docker 配置和完整协议见 [平台提交说明](docs/平台规则与提交.md)。

目前已做本地协议测试，正式评测还没跑。导出的 TXT / JSON 是本项目自己的格式，暂时不能直接用于平台回放。

## 测试

```powershell
.\.venv\Scripts\python.exe -m unittest discover -s tests -v
```

前端改动还可以用 `node --check static/app.js` 检查语法。Node.js 只用于检查，启动应用不需要它。推送后会运行 GitHub Actions，结果在 [Actions](https://github.com/zhehong268/liuziqi/actions) 查看。

## 开发文档

- [后端与数据库](docs/第01课-需求与架构.md)
- [AI 接入](docs/第02课-AI接入.md)
- [接口说明](docs/接口说明.md)
- [Git 提交与推送](docs/GitHub首次推送.md)
- [迭代日志](docs/迭代日志.md)
- [更新记录](CHANGELOG.md)
