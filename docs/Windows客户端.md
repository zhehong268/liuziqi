# Windows 客户端

[下载最新版](https://github.com/zhehong268/liuziqi/releases/latest)，支持 Windows 10 / 11 的 64 位系统。

## 打开棋局

下载 `MoYi-Setup-Windows-x64.exe`，按安装向导完成安装，再打开“墨弈六子棋”。程序会启动本机服务并打开浏览器。

不想安装可以下载 `MoYi-Portable-Windows-x64.zip`，先完整解压，再运行 `MoYi/MoYi.exe`。不要只从压缩包中拖出 exe，旁边的 `_internal` 目录也要保留。

人机练习可以直接使用，不需要 Python 或 API Key。下次启动会恢复最近一局未结束的棋局。关闭启动窗口会停止服务，已经下过的棋子仍会保留。

## 上传 AI

1. 安装并启动 [Docker Desktop](https://www.docker.com/products/docker-desktop/)，使用 Linux 容器。
2. 点击启动窗口的“准备对战环境”。首次会下载 Python 和 C++ 编译环境，需要联网。
3. 状态显示“对战环境已就绪”后，打开棋局，在“AI 挑战”中上传单文件 `.py` 或 `.cpp`。
4. 填写名称，选择执黑或执白，开始挑战。页面可以观战，结束后可以复盘和导出棋谱。

双方每颗子限时 10 秒，单线程，协议是 `arena.v1`。源码格式和例子见 [上传 AI 与对战](上传AI与对战.md)。环境准备失败时，可点“打开记录目录”，查看 `docker-build.log`。

## 记录与更新

安装版和便携版都把数据放在：

```text
%LOCALAPPDATA%\MoYi\data
```

其中 `connect6.db` 是棋局数据库，`arena/` 保存上传版本和编译缓存。可以用“打开记录目录”找到它。需要迁移时，先关闭程序，再备份整个目录。

下载新安装包覆盖安装即可，更新、卸载不会自动清除记录。不同电脑各自保存对局，在线练习版的浏览器记录也单独保存。在线版网址：[墨弈](https://zhehong268.github.io/liuziqi/)。

## 从源码打包

需要 Windows、Python 3.12、Inno Setup 6。

```powershell
python -m pip install -r requirements.txt pyinstaller==6.22.3
.\packaging\build.ps1 -Version 0.4.1
```

脚本先生成程序，再用独立的临时数据库检查 HTTP 服务、练习、棋谱导出和官方源码，检查通过后才生成安装包。下载文件在 `dist/installer/`，同时生成 SHA256 校验文件。

仓库的 `Build Windows client` 工作流执行同样步骤。推送版本标签后会发布到 GitHub Release。
