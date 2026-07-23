<p align="center">
  <a href="./README.md">
    <img alt="English" src="https://img.shields.io/badge/English-EAEAEA?style=for-the-badge&labelColor=EAEAEA&color=111111" />
  </a>
  <a href="./README.zh-CN.md">
    <img alt="简体中文" src="https://img.shields.io/badge/简体中文-111111?style=for-the-badge" />
  </a>
</p>

<p align="center">
  <img src="screenshots/cover.webp" alt="Code Buddy cover" width="100%" />
</p>

<h1 align="center">Code Buddy</h1>

<p align="center">
  一个基于 StickS3 的 Codex 硬件伙伴，改编自
  <a href="https://github.com/anthropics/claude-desktop-buddy">Claude Desktop Buddy</a>。
</p>

<p align="center">
  给设备刷一次固件，在 macOS 上运行一次 <code>code-buddy</code>，之后照常使用 <code>codex</code>，审批提示和会话状态就会转移到独立硬件上。
</p>

> 如果你想自己做硬件客户端，可以看 [firmware/REFERENCE.md](firmware/REFERENCE.md) 里的 BLE 协议和 JSON 负载定义。

## 项目包含什么

- 一个 macOS 主机桥接层，负责与 StickS3 配对、同步时间、安装原生 BLE helper，并管理本地 `codex` shim。
- 一套 StickS3 固件，包含状态页、审批页、设置页和离线页。
- 一套尽量不打扰日常工作的流程：先跑一次 `code-buddy`，之后直接用 `codex`。

## v0.1.41 亮点

- 暂时拿不到最新账户额度、返回 `null` 或 BLE 断开时，额度进度不再消失；Mac 桥接进程重启后也会恢复最近一次有效值，而不是清空设备显示。
- 按 Figma 重做的横屏状态页继续显示 `RUNNING`、`WAITING`、`IDLE` 或 `OFFLINE`，心跳则改为最近 20 秒真实输入加输出 token 消耗曲线；新采样从右侧进入并向左推进，活动越强，颜色越从绿色靠近 mint。
- 自动旋转首页会在第一帧前判断明确姿态，并在菜单、设置等竖屏页面之间保留最近一次稳定方向，修复设备明明已经横放却先闪一下竖屏再切回横屏的问题。
- 竖屏首页保留原来的 90px 高、1× ASCII pet，并在居中前恢复其内置 6×8 点阵字体；`HH:MM:SS` 统一为同一基线上的原生 14pt 单行时间，秒保持弱化色，日期使用原生 8pt。
- 英文标签恢复为原生字号的 JetBrains Mono Regular/Bold，数字保留斜杠零；0 计数显示为 40% 白，秒显示为 60% 白，月日和星期使用纯白。
- 29×3 额度点阵保留 6px 圆点和任务运行时的对角波形，底部和左侧对齐到新的 4px 边界。
- 横屏时钟和状态页使用原生字号的 JetBrains Mono Regular/Bold 位图子集，不再做分数倍拉伸；非 ASCII 界面继续使用一套比例字体，并保留充足的应用分区空间。
- 每个完整 Codex turn 结束后只响一次；既支持受管的 CLI 会话，也支持从本地日志发现的 Codex Desktop 主任务，并会自动排除重复快照和 subagent 完成事件。
- 安全 Wi-Fi OTA 支持 Mac 手动推送和可选的自动更新，包含签名清单、物理确认、回滚检查和设备端紧凑进度提示。
- 充电时钟和任务运行界面共用横屏布局，同时保证审批与设置界面可读。

## 快速开始

### 1. 给 StickS3 刷机

从 GitHub Releases 下载 `code-buddy-sticks3-v{version}-full.bin`，然后写入到 `0x0`。

优先方式：

- 如果当前 release 提供 web flasher，直接用它把合并镜像写到 `0x0`。

兜底方式：

```bash
esptool --chip esp32s3 --port /dev/cu.usbmodem101 --baud 460800 write_flash 0x0 code-buddy-sticks3-v0.1.41-full.bin
```

开发者本地生成 release 镜像：

```bash
./scripts/build-firmware-release.sh
```

脚本会生成两种不同的文件：`*-full.bin` 是 USB 恢复/首次刷写镜像，`*-app.bin` 是 OTA 使用的应用镜像。不要把合并后的 `full.bin` 传给 OTA 命令。

### 2. 在 macOS 上安装

```bash
brew install CharlexH/tap/code-buddy
code-buddy
```

首次运行时，Code Buddy 会：

- 安装原生蓝牙 helper
- 与 `Codex-*` 设备配对
- 同步设备时间
- 安装 launchd agent
- 安装本地 `codex` shim
- 把 `~/.code-buddy/bin` 加进 `~/.zprofile`

仅主机侧的更新在协议仍兼容时不需要重新刷机；涉及屏幕、声音或 OTA 运行时的功能，需要配套版本的设备固件。

### 无线固件更新

首次通过 USB 刷入支持 OTA 的完整固件，并在设备设置中配置 Wi-Fi 后，打开 **Settings > OTA update**，然后运行：

```bash
code-buddy firmware update
```

开发固件可以显式指定 app-only 镜像：

```bash
code-buddy firmware update --firmware firmware/.pio/build/m5stack-sticks3/firmware.bin
```

主机代理会作为唯一的蓝牙所有者，为一次性不可变清单签名，通过短时本地 HTTPS 提供 app-only 镜像，并等待设备 A 键确认。Mac 只有在设备重连并证明目标版本已运行、首次启动健康状态有效后才会报告成功。不要把 `*-full.bin` 用于 OTA。

打开 **Settings > auto ota** 后，设备可以自动接受更新的可信版本。自动更新仍遵循相同的签名、版本、启动健康检查和回滚策略。

正常使用时，原生 BLE helper 会作为 macOS 后台 agent 运行，所以重连过程不应该再打开 helper 窗口或抢走焦点。macOS 首次蓝牙权限确认仍可能出现，这是系统权限弹窗，不能跳过。如果需要调试 helper 事件，可以用 `CODE_BUDDY_BLE_HELPER_DEBUG_WINDOW=1` 打开事件日志窗口。

### 3. 正常使用

```bash
codex
```

初始化完成后请开一个新 shell。此后你可以保持原来的 CLI 使用方式，Code Buddy 会在后台维持桥接，并把审批提示显示到 StickS3 上。

Codex Desktop 任务通过本地 Codex 会话日志以只读方式发现，可以更新状态数字、未查看数量和完成提示音；Desktop 的审批请求不会转发到设备。

## 按键说明

|                         | 常规界面             | 宠物界面    | 信息界面    | 审批界面    |
| ----------------------- | -------------------- | ----------- | ----------- | ----------- |
| **A**（正面）           | 下一个页面           | 下一个页面  | 下一个页面  | **批准**    |
| **B**（右侧）           | 滚动 transcript      | 下一页      | 下一页      | **拒绝**    |
| **长按 A**              | 菜单                 | 菜单        | 菜单        | 菜单        |
| **Power**（左侧，短按） | 熄屏 / 亮屏          |             |             |             |
| **Power**（左侧，约 6s）| 强制关机             |             |             |             |
| **摇一摇**              | dizzy                |             |             | —           |
| **正面朝下**            | nap（恢复能量）      |             |             |             |

屏幕在 30 秒无操作后会自动熄灭；如果有待处理审批，会保持常亮。按任意键都可以唤醒。

## Buddy 状态

| 状态        | 触发条件                  | 表现                        |
| ----------- | ------------------------- | --------------------------- |
| `sleep`     | bridge 未连接             | 闭眼，慢呼吸                |
| `idle`      | 已连接，但没有紧急事件    | 眨眼，左右看                |
| `busy`      | 会话正在活跃运行          | 出汗，忙碌                  |
| `attention` | 有待审批请求              | 警觉，**LED 闪烁**          |
| `celebrate` | 升级（每 50K tokens）、周五时钟彩蛋 | 撒花，跳动                  |
| `dizzy`     | 你摇了设备                | 蚊香眼，摇晃                |
| `heart`     | 在 5 秒内完成批准         | 飘心                        |

当 StickS3 处于 USB 供电、已经同步过时间、且没有运行中或待审批会话时，会进入充电时钟状态。周五 15:00 到午夜之间，宠物会偶尔庆祝：每 12 秒循环里大约庆祝 4 秒。

<details>
<summary><strong>角色和自定义素材包</strong></summary>

固件内置了十八个 ASCII 宠物，每个宠物都包含七种动画：`sleep`、`idle`、`busy`、`attention`、`celebrate`、`dizzy` 和 `heart`。

在设备上进入 `menu -> next pet` 可以轮换角色。选择会保存在设备存储里，重启后仍会保留。

如果你想换成自定义 GIF 角色，可以准备一个包含 `manifest.json` 和对应七种状态 GIF 的角色包。GIF 建议宽度为 96px：

```json
{
  "name": "bufo",
  "colors": {
    "body": "#6B8E23",
    "bg": "#000000",
    "text": "#FFFFFF",
    "textDim": "#808080",
    "ink": "#000000"
  },
  "states": {
    "sleep": "sleep.gif",
    "idle": ["idle_0.gif", "idle_1.gif", "idle_2.gif"],
    "busy": "busy.gif",
    "attention": "attention.gif",
    "celebrate": "celebrate.gif",
    "dizzy": "dizzy.gif",
    "heart": "heart.gif"
  }
}
```

说明：

- `idle` 可以是一张 GIF，也可以是一组 GIF 数组。
- 高度控制在约 140px 以内会比较适合 StickS3 屏幕。
- 可以参考 [firmware/characters/bufo/](firmware/characters/bufo/) 的现成示例。
- 资源处理和刷写工具在 [firmware/tools/prep_character.py](firmware/tools/prep_character.py) 和 [firmware/tools/flash_character.py](firmware/tools/flash_character.py)。
</details>

## 恢复命令

```bash
code-buddy doctor
code-buddy repair
code-buddy uninstall
```

`doctor` 会告诉你哪里出了问题、为什么会这样，以及下一步应该怎么做。

<details>
<summary><strong>从源码运行</strong></summary>

```bash
python3 -m venv .venv
.venv/bin/pip install -e '.[dev]'
.venv/bin/code-buddy
```

验证命令：

- 主机侧测试：`.venv/bin/pytest -q`
- 固件构建：`cd firmware && pio run`
</details>
