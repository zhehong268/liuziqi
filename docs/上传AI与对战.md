# 上传 AI 与对战

先启动 Docker Desktop，使用 Linux 容器，在项目目录构建镜像：

```powershell
docker build -t connect6-sandbox:1 sandbox
.\.venv\Scripts\python.exe app.py
```

打开网页，填写名称、上传代码，选择执子后开始挑战。首页的 Python 示例就是项目根目录的 `bot.py`，下载后可以直接上传。

## 代码要求

- 单文件 `.py` 或 `.cpp`，UTF-8 编码，最大 512 KB。
- Python 3 标准库，或 C++17 标准库。暂不支持安装第三方包、压缩包或 Windows EXE。
- 程序常驻，循环读取标准输入；每行一个 JSON 请求，每次输出一行 JSON 响应。
- 输出后立即刷新缓冲区。stdout 只写协议响应，调试日志写 stderr。
- 不创建额外线程或子进程。双方每次请求最多使用 10 秒墙钟时间。

## 请求与响应

请求包含 `protocol`、`request_id`、`type`、`game`、`player`、`turn_index`、`time_limit_ms` 和 `observation`。时间固定为 10000 毫秒。

`observation.board` 是 19×19 的整数数组，0 为空，1 为黑，2 为白。`current_player` 是本次行动方，`moves_in_turn` 是本回合已经落的子数，`remaining_stones` 是本回合剩余子数。`move_number` 是棋盘上已有的子数。

每次只要一颗子，`required_stones_this_turn` 固定为 1；同一回合的第二颗由裁判再次请求。不要自行按请求奇偶判断颜色。

响应格式如下，`request_id` 必须原样返回：

```json
{"protocol":"arena.v1","request_id":"本次请求的编号","type":"move_response","action":{"kind":"PLACE","row":9,"col":9}}
```

坐标从 0 开始，范围为 0～18，必须是整数且指向空位。`bot.py` 的 `main()` 可以作为读取多次请求的参考。

## 裁判怎么限制运行

上传只保存源码，不导入或执行。开始对战后，后台队列串行处理棋局，C++ 在编译容器里生成 Linux 程序，最长编译 60 秒。首次使用某个版本需要编译，之后复用缓存。

对战时双方各有一个容器，均设置 `--pids-limit 1`、`--cpus 1`、512 MB 内存、禁用网络、只读根文件系统、非 root 用户以及移除所有 capabilities。PID 控制器统计的是线程，因此新增线程或子进程会被内核拒绝。启动器用 `exec` 替换自己，并检查容器中的 `pids.max` 确实为 1。

计时从发送每次请求之前开始，包含输入写入和等待响应，不包含编译与容器启动。10 秒内没有合法响应就终止该容器并判负。单行响应上限为 16 KB；协议错、请求编号不匹配、崩溃、越界或重复落子同样判负。一颗子已经成六就立即结束棋局。

界面的倒计时用于观察，判定以服务器的单调时钟为准。限制参考 [Docker run 参数](https://docs.docker.com/reference/cli/docker/container/run/)和 [Linux PID 控制器](https://www.kernel.org/doc/html/latest/admin-guide/cgroup-v2.html#pid)。

## 保存与中断

每次上传产生独立版本，记录名称、语言和 SHA-256。每局保存挑战方颜色、官方版本哈希、每颗落子耗时、终局原因和复盘，TXT / JSON 导出都包含对战信息。

官方引擎从交接包最终版导入，源码在 `official/main.cpp`。修改后新对局会记录新的哈希；之前的棋谱仍保留原版本编号。引擎内部原有的 5 秒搜索预算没有改动，外部裁判对双方统一执行 10 秒硬限制。

刷新或关闭网页不会停止后台比赛。可以用“停止本局”取消；服务重启会把未结束的对局标为中断，保留已下棋谱。

当前 Demo 使用一个后端进程和一个对战队列，队列最多 8 局。没有登录、上传总量配额或多机器调度；对外长期开放前需要补齐这些管理功能。

同一局域网测试时，可在启动前设置 `$env:APP_HOST = '0.0.0.0'`，其他电脑使用服务器的局域网 IP 和 5000 端口访问。公网使用需要另行部署；GitHub 仓库只保存代码，不会运行 Flask 或 Docker。
