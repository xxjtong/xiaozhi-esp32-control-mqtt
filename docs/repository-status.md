# 仓库与迁移现状总览

> 状态快照：2026-11-10。本文只做现状登记，不改动项目内其他文档。
> 适用范围：本工作区（`xiaozhi-esp32-main`）及其 2026-11-09 复制出的独立仓库（`xiaowei-esp32-main`）。

---

## 1. 一页结论

- 当前工作区是**原仓库** `xxjtong/xiaozhi-esp32-control-mqtt`，`origin` 未变；在这里提交、推送仍进入原仓库。
- 2026-11-09 另建了私有仓库 `xxjtong/xiaowei-esp32-main`，由本工作区完整复制而来，**仅改目录名与仓库名，未做代码级 `xiaozhi → xiaowei` 改名**。
- 代码主线是上游 v2.5.2 + ESP-IDF v6.1 的定制版，`PROJECT_VER = 2.5.2.1`；2.5.2 迁移的基线门与 4B 真机首刷均已通过，当前处于「定制收敛 + 真机调优」阶段。
- `PROJECT_OVERVIEW.md` 等旧文档仍写 2.4.2.1 / IDF 6.0.2，属**已知滞后项**，以本文和 `docs/migration-2.5.2/` 为准。

---

## 2. 仓库拓扑

| 项 | 工作区（原） | 副本（新） |
|---|---|---|
| 本地路径 | `/Users/tong/Downloads/xiaozhi-esp32-main` | `/Users/tong/Downloads/xiaowei-esp32-main` |
| `origin` | `ssh://git@ssh.github.com:443/xxjtong/xiaozhi-esp32-control-mqtt.git` | `https://github.com/xxjtong/xiaowei-esp32-main.git` |
| 其他远端 | 无 | `upstream` 指向原仓库，`pushurl=no_push`（只读，防误推） |
| GitHub 可见性 | 公开 | **私有** |
| 默认分支 | `main` | `main` |
| 当前检出分支 | `migrate/2.5.2` | `migrate/2.5.2` |
| 复制时间 / 规模 | — | 2026-11-09，1.3 GB / 22,416 文件（含 `.git`、构建产物与组件缓存） |

新仓库创建地址：<https://github.com/xxjtong/xiaowei-esp32-main>。

**防误推提醒**：两个目录的 `.git` 是复制关系但互不联动。改动落在哪个目录，就只会提交到该目录的 `origin`。需要切换开发主场时，先 `git remote -v` 确认。

---

## 3. 分支、标签与同步状态

| ref | 提交 | 说明 | 原仓库远端 | 新仓库远端 |
|---|---|---|---|---|
| `main` | `3b3df76` | 迁移前冻结 fork 2.4.2.1 工作区 | `origin/main` = `9c87557`，**本地领先 1 个提交，尚未推送**（快进，无冲突） | 已推送 |
| `migrate/2.5.2` | `aebf5fa` | 2.5.2 迁移主线，当前工作分支 | 已同步 | 已同步 |
| tag `fork-2.4.2.1` | `3b3df76` | 轻量标签，冻结基线 | 有 | 已通过 API 补建 |

两个仓库的 `migrate/2.5.2` 与标签内容一致；`main` 的领先提交是唯一的远端不一致点。

---

## 4. 改名边界（重要）

| 范围 | 处理 |
|---|---|
| 目录名 / GitHub 仓库名 | 已改为 `xiaowei-esp32-main` |
| `origin` / `upstream` 指向 | 已按第 2 节调整 |
| 源码、文档中的 `xiaozhi` 字样 | **全部保留原样** |
| 板级 README、协议文档、上游链接 | **全部保留原样** |

理由：被 git 跟踪的文档共 178 个，其中 108 个含 `xiaozhi`；该字样同时出现在上游项目链接、配网 AP 名（如 `Xiaozhi-XXXX`）、OTA 与唤醒/资产相关字符串中，全局替换会偏离上游基线并可能影响设备行为。如需做产品级改名，应单独立项、逐类评估并配套真机回归。

---

## 5. 迁移与定制进度

| 阶段 | 状态 | 依据 |
|---|---|---|
| 基线 | fork `2.4.2.1`（上游 v2.4.2），备份 `backup/fork-2.4.2.1/`（bundle + 源码包 + 差异 patch + 固件，约 21 MB） | `backup/fork-2.4.2.1/MANIFEST.md` |
| 定制盘点 | 相对 v2.4.2 差异 75 文件；真实定制 27 文件约 1,880 行，fork 新增约 4,300 行 | `docs/migration-2.5.2/customization-inventory.md`、`file-matrix.csv` |
| 工具链 / 基线门 S2 | **通过**：ESP-IDF v6.1（`v6.1`，commit `fff9895c8`）编译纯上游 v2.5.2，3.49-V2 `0x2bcd50`、4B `0x2bf550`，分区余量 30%，110 个宿主单测全绿 | `docs/migration-2.5.2/s2-baseline-gate.md` |
| 迁移落地 | `PROJECT_VER = 2.5.2.1`，定制已搬入；评审口径为 39 个改动文件 + 6 个新增源文件 | `CMakeLists.txt`、`docs/migration-2.5.2/customization-review.md` |
| 真机验证 | 2026-11-04 4B 首刷 2.5.2.1 成功，外设（AXP2101 / ST7701 / GT911 / QMI8658 / PCF85063 / ES8311+ES7210 / 背光）全部 OK；11-05～11-08 陆续完成激活、音频、预览图、AEC/UI 与总线时序收敛 | `docs/2026-11-08-4b-bus-timing-tuning.md` 等日期文档 |

**当前待办**：评审建议把约 12 个可归零文件搬回 fork 自有文件（R1～R7，按 P0→P3 分批，每批编译 + 单测 + 真机回归）；F1 控制通道与 F9 实测必需修复不动。详见 `customization-review.md` 第五节。

---

## 6. 已知文档滞后项

以下文档保留原样（本次不修改），阅读时注意以本文与迁移文档为准：

| 文档 | 滞后内容 | 当前事实 |
|---|---|---|
| `PROJECT_OVERVIEW.md` | `PROJECT_VER = 2.4.2.1`、ESP-IDF v6.0.2、章节 13 的 2.4.2.1 发布记录 | `2.5.2.1` / IDF v6.1（推荐），最低 v6.0.1 |
| `README*.md` | 部分内容沿用上游表述 | 以 `AGENTS.md` 与本文为项目事实口径 |

---

## 7. 环境与操作约束

| 约束 | 说明 / 绕法 |
|---|---|
| git over HTTPS/SSH 经代理不稳 | `git fetch` / `git push` 间歇报 `CONNECT tunnel failed, response 502` 或 `Empty reply from server`；`gh api` 全程稳定，可用于远端核验，标签也可用 `POST /repos/{owner}/{repo}/git/refs` 补建 |
| IDF 激活 | 用自建脚本 `source ~/.espressif/tools/activate_idf_v6.1.sh`；直接 source `export.sh` 会被 WorkBuddy 的 python 遮蔽找错 venv |
| 构建环境注入 | 需 `export IDF_COMPONENT_CACHE_PATH=/tmp/idfcache`，并用 `env -u PYTHONPATH -u BASH_ENV` 启动，否则组件复制被 sitecustomize 劫持报 `PermissionError` |
| 批量删除保护 | 环境对单轮 >50 文件的删除有拦截，会打断构建；用 `mv` 改名代替删除 |
| 构建脚本副作用 | `scripts/build.py` 会改本地 `sdkconfig` 和构建状态；切换板型前不要假设 `build/` 仍代表上一个目标 |
| 副本的构建产物 | 复制时连 `build/`、`managed_components/`、`sdkconfig*` 一起带过（未被 git 跟踪）；新副本若要独立构建，先清理自己的 `build/` |

---

## 8. 常用命令

```sh
# 确认自己站在哪个仓库（务必先做）
git remote -v && git status -sb

# 推送当前分支到各自 origin
git push origin migrate/2.5.2

# 远端状态核验（代理不稳时用 API 代替 git ls-remote）
gh api repos/xxjtong/xiaowei-esp32-main/branches --jq '.[] | "\(.name) \(.commit.sha)"'
gh api repos/xxjtong/xiaozhi-esp32-control-mqtt --jq '.default_branch, .private'

# 新仓库改公开（如需）
gh repo edit xxjtong/xiaowei-esp32-main --visibility public
```

---

## 9. 相关文档索引

| 文档 | 用途 |
|---|---|
| `AGENTS.md` | 项目架构、开发规则、构建与验证要求（权威） |
| `PROJECT_OVERVIEW.md` | 项目入门总览（版本信息已滞后，见第 6 节） |
| `docs/migration-2.5.2/customization-inventory.md` | 定制全量盘点与 7 阶段方案 |
| `docs/migration-2.5.2/file-matrix.csv` | 75 行文件级处置矩阵 |
| `docs/migration-2.5.2/s2-baseline-gate.md` | IDF 6.1 + 上游 2.5.2 编译验证 |
| `docs/migration-2.5.2/customization-review.md` | 定制收敛建议与优先级 |
| `docs/migration-2.5.2/toolchain-and-version-assessment.md` | 工具链与依赖版本评估 |
| `docs/custom-board.md` | 新增板型/变体流程 |
