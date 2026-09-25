# ESP32-S3 桌面智能终端（天气站 · AI 助手 · 电子书阅读器）

> 基于 [78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32)（v2.2.2）二次开发，
> 目标硬件：**微雪 ESP32-S3-RLCD-4.2** 开发板（400×300 反射式单色屏）。
>
> 原项目说明文档见 [README_zh.md](README_zh.md) / [README_ja.md](README_ja.md)。

本项目在小智 AI 语音助手的基础上，把设备改造为一台桌面智能终端：
常显的天气站时钟页、音乐播放页、番茄钟（带白噪音）、备忘录，并保留完整的语音对话能力。
下一阶段目标是增加**电子书阅读器**功能（见路线图）。

## 在原项目基础上新增了什么

### 1. 板级支持（全新目录 `main/boards/waveshare-s3-rlcd-4.2/`）

- **400×300 RLCD 反射屏驱动**：SPI 40MHz、1-bit 帧缓冲（PSRAM）、RGB565→单色转换 LUT、对比度调节（`rlcd_driver.cc`）
- **板载外设驱动**：
  - SHTC3 温湿度传感器（I2C）
  - PCF85063 实时时钟（纽扣电池备份，断电保持走时）
  - SD 卡（SDMMC，挂载于 `/sdcard`，存放白噪音 MP3 与后续电子书）
  - ADC 电量检测（GPIO4）+ 锂电充电管理
  - ES8311/ES7210 音频编解码（沿用原项目）
- **按键交互**：BOOT 单击 = 语音对话开关；USER 单击 = 切换页面、双击 = 刷新数据、长按 = 滚动显示系统信息

### 2. 全新 UI（LVGL 9，三页 + 状态栏）

| 页面 | 内容 |
|---|---|
| 🌤 **天气页**（主页） | 2×2 卡片布局：大字时钟 / 日历+天气 / AI 对话卡（表情+状态文字）/ MEMO 备忘录 |
| 🎵 **音乐页** | 唱片封面、歌名/歌手、上/中/下三行歌词、进度条+时间 |
| 🍅 **番茄钟页** | 状态文字、大号倒计时、进度条、"25分钟 专注 / 5分钟 休息"设定 |

- 右上角浮动状态栏胶囊：WiFi 图标 + 电池图标 + 电量百分比；左上角温湿度
- 每页底部统一的 AI 状态卡（表情图 + 状态文字），与小智对话状态联动
- 低电量弹窗提示；**5 分钟无操作自动省电**（刷新频率 1s → 5s）
- 页面代码拆分：`weather_ui.cc` / `music_ui.cc` / `pomodoro_ui.cc` / `data_update_task.cc`（后台数据刷新任务）

### 3. 新增功能

- **番茄钟**：启动/暂停/恢复/停止，可联动 SD 卡白噪音（MP3）伴听
- **白噪音播放器**：修复原框架播放卡顿，SD 卡文件扫描与管理（`managers/sdcard_manager`、`pomodoro_manager`）
- **备忘录**：新增/列表/完成/清空，NVS 持久化，主界面常显
- **天气回写**：由 AI 通过 MCP 工具写入天气数据，避免设备端频繁抓取
- **自动省电模式**：空闲降频刷新 + 活动检测唤醒

### 4. 新增 MCP 工具（语音可控）

| 工具 | 作用 |
|---|---|
| `self.disp.switch` | 语音切换页面（`toggle`/`music`/`weather`/`pomodoro`） |
| `self.pomodoro.start/stop/pause/status` | 番茄钟控制（含白噪音开关、分钟数参数） |
| `self.memo.add/list/done/clear` | 备忘录增删查改 |
| `self.weather.update` | AI 回写天气数据 |
| `self.disp.network` | 重新配网 |
| `self.system.info` | 查询 CPU/内存/运行时等系统信息 |

### 5. 文档

- 板卡完整文档：[main/boards/waveshare-s3-rlcd-4.2/README.md](main/boards/waveshare-s3-rlcd-4.2/README.md)
  （硬件、按键、屏幕布局、MCP 工具、功耗分析、故障排查）
- 计划文档：[docs/2026-02-10-计划文档/](docs/2026-02-10-计划文档/)
- 电子书功能设计：[docs/2026-09-25-电子书阅读器计划.md](docs/2026-09-25-电子书阅读器计划.md)

## 路线图

- [ ] **电子书阅读器**：SD 卡本地 TXT 阅读（章节索引/手动分页/GBK 转码）+ MCP 联网下载，详见 [电子书阅读器计划](docs/2026-09-25-电子书阅读器计划.md)
- [ ] RLCD 脏区刷新优化（提升翻页流畅度）
- [ ] 备忘录日期提醒、传感器历史数据可视化

## 硬件

| 项 | 规格 |
|---|---|
| 主控 | ESP32-S3-WROOM-1-N16R8（双核 240MHz，16MB Flash + 8MB PSRAM） |
| 屏幕 | 4.2" 400×300 RLCD 反射屏（1-bit 单色，阳光可读，静态几乎零耗电） |
| 传感器 | SHTC3 温湿度、PCF85063 RTC |
| 音频 | ES8311 解码 / ES7210 编码 / MAX98357A 功放 |
| 存储 | SD 卡（SDMMC） |
| 电源 | 锂电池 + USB-C 充电，ADC 电量检测 |

购买与官方 Wiki：[微雪 ESP32-S3-RLCD-4.2](https://www.waveshare.com/wiki/ESP32-S3-RLCD-4.2)

## 编译与烧录

### 环境要求

- **ESP-IDF v5.5.2**（组件清单要求 ≥5.5.2）
- Python 3.10+（3.14 已验证可用）

```bash
# 1. 进入项目
cd xiaozhi-esp32

# 2. 加载 ESP-IDF 环境
source ~/esp/esp-idf/export.sh     # 路径按实际安装位置

# 3. 首次配置：选择开发板
idf.py set-target esp32s3
idf.py menuconfig                  # 选择 Board Type → Waveshare S3 RLCD 4.2

# 4. 编译
idf.py build

# 5. 烧录 + 串口监视
idf.py flash monitor
```

常用命令：`idf.py erase-flash`（清 NVS 重置配网）、`idf.py menuconfig`（配置）。

## 目录结构（本项目相关部分）

```
main/
├── boards/waveshare-s3-rlcd-4.2/   ← 本项目核心（板级 + UI + 管理器）
│   ├── waveshare-s3-rlcd-4.2.cc    # 板级入口：按键、MCP 工具注册
│   ├── rlcd_driver.cc              # RLCD 屏驱动
│   ├── custom_lcd_display.cc       # 显示核心类与页面切换
│   ├── weather_ui.cc / music_ui.cc / pomodoro_ui.cc
│   ├── data_update_task.cc         # 后台数据刷新（时间/天气/传感器/AI状态）
│   ├── managers/                   # weather / pomodoro / sdcard / sensor
│   └── assets/                     # 字体、图标
├── display/                        # 通用显示层（LVGL，沿用原项目）
├── application.cc                  # 主状态机（原项目）
└── mcp_server.cc                   # MCP 框架（原项目）
```

## 致谢与许可

- 感谢 [78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) 提供的优秀开源基础
- 本项目遵循原项目 **MIT 许可证**（见 [LICENSE](LICENSE)）
