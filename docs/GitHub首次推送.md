# Git 提交与推送

仓库：[zhehong268/liuziqi](https://github.com/zhehong268/liuziqi)，分支 `main`。首次推送已完成，平时改完代码直接按下面的步骤提交。

## 日常提交

在项目目录运行：

```powershell
.\.venv\Scripts\python.exe -m unittest discover -s tests -v
git diff
git status
git add bot.py docs/迭代日志.md
git diff --cached
git commit -m "调整 AI 防守评分"
git push
```

`git add` 后的文件名换成这次实际修改的文件。提交名用中文说明改动，例如 `修复刷新后丢失棋局的问题`、`补充棋谱导出测试`。`commit` 保存在本地，`push` 上传到 GitHub。

提交历史可以这样看：

```powershell
git log --oneline
git log --oneline --reverse
```

第二条从最早的提交开始显示。提交前面的短编号由 Git 自动生成。

## 首次设置

这一节用于新建仓库时参考。本项目已经配置过，不用再执行。

在 [GitHub](https://github.com/new) 建空仓库，复制 HTTPS 地址。本地已有代码时，不自动生成 README、`.gitignore` 或 License。

```powershell
git remote add origin https://github.com/你的用户名/你的仓库名.git
git branch -M main
git push -u origin main
```

已有 `origin` 时先用 `git remote -v` 检查，地址有误再用 `git remote set-url origin 新地址` 修改。后续只需 `git push`。

如果提交时报身份未配置，在当前仓库设置：

```powershell
git config user.name "你的名字"
git config user.email "你的邮箱或GitHub的noreply邮箱"
```

HTTPS 推送可能弹出浏览器登录窗口。使用自己的 GitHub 账号登录，不要把密码或 Token 写进仓库。

## 这次遇到的两个问题

### 代理端口不对

原来 Git 指向 7877，代理软件实际监听 7897。检查端口后，对当前仓库设置：

```powershell
git config --local http.https://github.com.proxy http://127.0.0.1:7897
git ls-remote origin
```

7897 是本机当时的端口，换代理软件或改过设置后要重新确认。

### non-fast-forward

GitHub 建仓库时生成了一条 README 提交，本地也有自己的历史。两边没有共同祖先，需要先合并：

```powershell
git fetch origin
git merge origin/main --allow-unrelated-histories
```

冲突文件里保留需要的内容，删掉 `<<<<<<<`、`=======`、`>>>>>>>` 标记，然后提交：

```powershell
git add README.md
git commit -m "合并远程初始化提交"
git push -u origin main
```

本项目这次保留了完整 README，合并提交为 `ea43738`。普通的分支落后不一定需要 `--allow-unrelated-histories`，先查看 `git log --oneline --graph --all` 确认情况。

## 其他提示

| 提示 | 检查 |
| --- | --- |
| `not a git repository` | 当前目录是不是项目目录 |
| `remote origin already exists` | `git remote -v` 中是否已有正确地址 |
| `Repository not found` | 仓库地址、登录账号和写入权限 |
| `nothing to commit` | 没有新改动，可能只需要 `git push` |
| `LF/CRLF` 警告 | 换行转换提示，通常不影响提交 |

参考：[GitHub 本地仓库上传说明](https://docs.github.com/en/migrations/importing-source-code/using-the-command-line-to-import-source-code/adding-locally-hosted-code-to-github)。
