# 把初版推送到 GitHub

本项目位于 `D:\计协学习\connect6-agent-demo`，已经有 `.git` 和真实的历史提交。**不要删除 `.git`，也不需要重新 `git init`。**

Git 保存本机版本；GitHub 保存远程副本。`commit` 不会自动上传，`push` 才会。

## 1. 在 GitHub 建空仓库

登录自己的 GitHub 账号，打开 [新建仓库](https://github.com/new)。

- Repository name：`connect6-agent-demo`
- Description：`六子棋 Web Demo：Flask、SQLite、自编 AI、Agent 复盘与棋谱导出`
- 按作业要求选择 Public；没有公开要求也可以先选 Private。
- 因为本地已有项目，不勾选 Add README，不自动添加 `.gitignore` 或 License，保持远程为空。
- 点击 Create repository，复制 HTTPS 地址，形如 `https://github.com/你的用户名/connect6-agent-demo.git`。

## 2. 检查本地版本

打开 PowerShell：

```powershell
cd D:\计协学习\connect6-agent-demo
git status
git log --oneline -10
git remote -v
```

交付时已把本次代码和新文档做成本地提交。目录里原有的 `docs/完整教学讲义.md`、`任务2验收清单.md` 未被本次提交纳入，避免替你提交已有但未跟踪的资料。你确认内容后可以单独提交：

```powershell
git add docs/完整教学讲义.md 任务2验收清单.md
git commit -m "docs: add personal learning notes"
```

如果没有修改需要提交，Git 提示 `nothing to commit` 是正常的，不要为了凑次数做空提交。

首次自己创建提交若提示身份未配置，可设置本仓库身份：

```powershell
git config user.name "你的名字或GitHub用户名"
git config user.email "你在GitHub使用的邮箱或GitHub提供的noreply邮箱"
```

该身份会进入后续提交记录，使用自己愿意展示的信息。

## 3. 绑定远程并第一次推送

**将下面 URL 中的“你的用户名”替换为真实用户名。**

```powershell
git remote add origin https://github.com/你的用户名/connect6-agent-demo.git
git branch -M main
git push -u origin main
```

如果上一步 `git remote -v` 已经列出 `origin`，先确认它是否是自己的目标仓库。已正确则跳过 `remote add`；需要改正时执行：

```powershell
git remote set-url origin https://github.com/你的用户名/connect6-agent-demo.git
git push -u origin main
```

HTTPS 推送可能弹出 Git Credential Manager 浏览器登录窗口，用自己的 GitHub 账号完成登录。不要把登录密码或 Token 填进源码或聊天。若终端要求密码，GitHub HTTPS 不使用账号密码，可通过凭据管理器完成登录，或使用 GitHub 支持的令牌认证方式。

## 4. 确认已经上传

1. 刷新 GitHub 仓库页面，看到 README、`app.py`、`bot.py`、`static/`。
2. 查看 Commits，确认原有历史和本次新提交都在。
3. 查看 Actions，首次推送会触发 Demo checks。绿色表示 GitHub 上的测试也通过；本机通过不等于 Actions 已经通过。
4. 确认 `.venv/`、`.env`、`data/connect6.db` 没有出现在仓库中。
5. 将仓库 URL 记录到作业提交材料。

本项目首次推送尚未代你执行：需要你先建好仓库并完成 GitHub 登录。按以上步骤能把已存在的全部本地历史一次推送过去，保留原来的版本轨迹。

## 5. 以后每次优化：改动 → 验证 → 提交 → 推送

例如你调整 AI 防守评分后：

```powershell
cd D:\计协学习\connect6-agent-demo
.\.venv\Scripts\python.exe -m unittest discover -s tests -v
node --check static/app.js
git diff
git status
git add bot.py docs/迭代日志.md
git diff --cached
git commit -m "feat: improve defensive move scoring"
git push
```

只 `git add` 这次实际改动的文件。每次尝试可以很小，但要说清楚“改了什么、为什么改、怎么验证、观察到什么”。一次尝试没有提升也可提交真实结果，之后再优化，不伪造历史或测试结论。

建议下一轮做：双子组合评分、更多固定棋局测试、复盘定位到棋盘、适配平台回放格式。每轮完成一个可运行结果后就推送一次。

## 常见错误

| 提示 | 处理 |
| --- | --- |
| `not a git repository` | 先 `cd D:\计协学习\connect6-agent-demo`，不要在父目录执行 |
| `remote origin already exists` | 执行 `git remote -v`，确认地址后跳过添加或使用 `set-url` |
| `Repository not found` | 核对 URL、仓库是否创建、当前登录账号是否有写权限 |
| `src refspec main does not match any` | 检查 `git log` 是否有提交，并执行 `git branch -M main` |
| `non-fast-forward` | 远程可能已自动创建 README；先 `git fetch origin`、查看差异并合并，保留双方历史，不使用强制推送 |
| `LF/CRLF` 警告 | 通常是 Windows 换行转换提示；本项目用 `.gitattributes` 统一源码，不是推送失败 |
| 网络连接超时 | 先确认浏览器可打开 GitHub，恢复网络后重试 `git push` |

GitHub 官方参考：[将本地代码添加到 GitHub](https://docs.github.com/en/migrations/importing-source-code/using-the-command-line-to-import-source-code/adding-locally-hosted-code-to-github)。
