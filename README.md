# smart_desk

基于 **ESP32-S3** 的桌面智能终端固件。集成了天气时钟、相册（图片/视频播放）、USB 摄像头预览、电脑状态监控等功能，带 320×240 触摸屏与本地音频输出。

---

## 硬件平台

| 项目 | 型号 / 规格 |
|---|---|
| 主控 | ESP32-S3，双核 Xtensa LX7 @ 240 MHz |
| Flash | 16 MB，QIO 80 MHz |
| PSRAM | 8 MB，Octal 80 MHz（`CONFIG_SPIRAM_MODE_OCT`） |
| 显示屏 | ST7789，320×240，8 位 Intel 8080 并口 |
| 触摸 | FT6336U 电容触摸（I2C） |
| IO 扩展 | XL9555（I2C，16 路 IO） |
| 音频功放 | NS4168，I2S 标准模式 |
| 存储 | TF 卡（SPI 模式）+ 片内 SPIFFS 分区 |
| 摄像头 | UVC 免驱摄像头（MJPG，320×240） |

### 引脚分配

**LCD（8080 并口）**

| 信号 | GPIO |
|---|---|
| DC | 1 |
| WR | 41 |
| CS | 2 |
| D0 ~ D7 | 40, 38, 39, 48, 45, 21, 47, 14 |

LCD 复位脚接在 XL9555 的 `P1_3`，背光接 `P1_2`。

**I2C 总线**

| 设备 | SDA | SCL | 说明 |
|---|---|---|---|
| XL9555 | 10 | 11 | 中断脚 GPIO 17 |
| FT6336U | 同总线 | 同总线 | 触摸中断经 XL9555 `P1_1` 中转 |

**SD 卡（SPI）**

| 信号 | GPIO |
|---|---|
| CS | 4 |
| MOSI（CMD） | 5 |
| CLK | 6 |
| MISO（DATA0） | 7 |

**I2S 音频（NS4168）**

| 信号 | GPIO |
|---|---|
| BCLK | 46 |
| LRCLK / WS | 9 |
| SDATA / DOUT | 8 |

功放使能脚 `CTRL` 接 XL9555 的 `P0_0`，高电平开启。**默认保持拉低**，避免 I2S 未起时钟时喇叭底噪。

**按键（经 XL9555）**

`P0_1` ~ `P0_4` 四路按键，低电平有效，支持短按/长按（默认 3 秒）回调。

---

## 功能

### 1. 主页（Home）

- 数字时钟，开机后通过 SNTP 同步网络时间，时区 `CST-8`
- 日期显示
- 实时天气信息

### 2. 相册（Album）

访问 TF 卡 `/sd/DCIM/` 目录，网格浏览多媒体文件。

| 类型 | 支持格式 | 实现方式 |
|---|---|---|
| 图片 | `.png` / `.jpg` / `.jpeg` / `.bmp` | LVGL LODEPNG / TJPGD 解码 |
| 视频 | MJPEG 编码的 AVI（PCM 音频） | TJPGD 逐帧解码 + I2S 播放 |
| 摄像头 | UVC 摄像头实时预览 | TJPGD 解码 MJPEG 流 |

扫描时预生成 70×70 RGB565 缩略图缓存到 PSRAM，避免列表滚动时反复解码文件。

**视频播放器**特性：

- AVI 容器解析（`hdrl` / `movi`，支持 `00dc` / `00db` / `01wb`）
- MJPEG 解码 + RGB888→RGB565 转换，PSRAM 双缓冲
- NS4168 通过 I2S 输出 PCM 音频
- 音频作为主时钟，落后时丢视频帧保证音画同步
- 支持暂停 / 进度条拖动 / 退出

### 3. 电脑状态监控（Monitor）

通过 AIDA64 的远程传感器 HTTP 接口，读取并显示 PC 的：

- CPU 占用率 / 温度
- 内存占用率 / 已用容量

### 4. 摄像头预览

UVC 摄像头接入后自动开机，MJPG 320×240 输出，解码后直接作为 LVGL 图像源全屏显示。

### 5. Web 控制页

Wi-Fi 连接后可访问设备 HTTP 服务：

| 路径 | 说明 |
|---|---|
| `/` | 网页界面（从 SPIFFS `html` 分区加载） |
| `/ws` | WebSocket 实时通道 |

### 6. Wi-Fi

- 支持 STA 配网，连接状态回调驱动 SNTP 与天气服务启动
- 内置 AP 回退模式

---

## 项目结构

```
smark_desk/
├── CMakeLists.txt                    顶层构建
├── partitions.webserver.csv          自定义分区表
├── sdkconfig / sdkconfig.defaults    配置
├── html/                             网页资源（打包进 html 分区）
├── img/                              UI 图片资源（打包进 img 分区）
├── bootani/                          开机动画资源（打包进 bootani 分区）
├── components/
│   ├── ap_wifi/
│   │   ├── ap_wifi.c                 Wi-Fi 初始化与状态管理
│   │   ├── wifi_manager.c            STA/AP 连接逻辑
│   │   ├── ws_server.c               HTTP + WebSocket 服务
│   │   └── bsp/
│   │       ├── xl9555.c/h            IO 扩展芯片驱动
│   │       ├── button.c/h            按键状态机（短按/长按）
│   │       ├── ft6336u_driver.c/h    触摸屏驱动
│   │       ├── usb_camera.c/h        UVC 摄像头 + TJPGD 解码
│   │       └── jpeg_color.c          JPEG 色彩转换
│   └── sd_card/                      TF 卡挂载（FATFS）
├── main/
│   ├── main.c                        应用入口
│   ├── lv_port.c/h                   LVGL 显示/触摸/文件系统移植
│   ├── weather.c/h                   天气数据获取
│   ├── aida64.c/h                    PC 状态监控
│   ├── animotion.c/h                 动画播放
│   └── ui/
│       ├── generated/                GuiGuider 生成的 UI 代码
│       │   ├── screens/              home / monitor / album 三个屏幕
│       │   ├── assets/               字体与图片
│       │   └── events/               控件事件
│       └── custom/                   自定义业务逻辑
│           ├── custom.c/h            入口与分发
│           ├── album_picture.c       相册网格 + 图片查看 + 摄像头预览
│           ├── album_video.c         AVI 视频播放器
│           ├── album_folder.c        文件夹浏览
│           ├── album_memory.c        缩略图缓存
│           ├── home_weather.c        天气与时钟刷新
│           └── monitor.c             PC 状态显示
└── managed_components/               组件管理器下载的依赖
```

---

## 分区表

`partitions.webserver.csv`：

| 名称 | 类型 | 子类型 | 大小 | 用途 |
|---|---|---|---|---|
| `nvs` | data | nvs | 24 KB | Wi-Fi 凭据等非易失数据 |
| `phy_init` | data | phy | 4 KB | 射频校准 |
| `factory` | app | factory | 2.5 MB | 应用固件 |
| `html` | data | spiffs | 16 KB | 网页文件 |
| `img` | data | spiffs | 128 KB | UI 图片 |
| `bootani` | data | spiffs | 1 MB | 开机动画 |

> 注意：当前分区表**没有 OTA 分区**，如需远程升级需要重新规划。

---

## 软件依赖

由 `idf_component.yml` 与组件管理器自动获取：

| 组件 | 版本 |
|---|---|
| ESP-IDF | v6.1 |
| `lvgl/lvgl` | ^9.6.0 |
| `espressif/esp_lvgl_port` | ^2.9.0 |
| `espressif/cjson` | ^1.7.19 |
| `espressif/usb` | UVC 主机协议栈 |
| `espressif/usb_host_uvc` | UVC 设备类驱动 |

---

## 编译与烧录

### 环境要求

- ESP-IDF **v6.1**（安装在 `D:\esp32\v6.1\esp-idf`）
- 目标芯片：`esp32s3`

### 命令

```bash
# 初始化环境（Windows PowerShell）
. $env:IDF_PATH\export.ps1

# 设置目标芯片
idf.py set-target esp32s3

# 编译
idf.py build

# 烧录（COM 口按实际情况修改）
idf.py -p COM7 flash

# 监视串口日志
idf.py -p COM7 monitor

# 一步完成编译+烧录+监视
idf.py -p COM7 flash monitor
```

### 应用配置

Wi-Fi 热点密码、天气 API Key、AIDA64 地址等通过菜单配置：

```bash
idf.py menuconfig    # Smart Desk Configuration
```

也可以复制 `sdkconfig.app.defaults` 为 `sdkconfig` 后按需修改。`sdkconfig` 已被 gitignore，不会提交到仓库。

### 资源分区烧录

`main/CMakeLists.txt` 已配置三个 SPIFFS 镜像自动打包，`flash` 时会一并写入：

```cmake
spiffs_create_partition_image(html    ../html    FLASH_IN_PROJECT)
spiffs_create_partition_image(img     ../img     FLASH_IN_PROJECT)
spiffs_create_partition_image(bootani ../bootani FLASH_IN_PROJECT)
```

修改 `html/`、`img/`、`bootani/` 下的资源后需要重新 `idf.py flash`。

> 新增 `.c` 文件后需执行 `idf.py reconfigure`，因为 `main/CMakeLists.txt` 使用 `file(GLOB_RECURSE)` 在配置阶段收集源文件。

---

## 使用说明

### 首次使用

1. 烧录固件，屏幕点亮后显示主页
2. 设备默认开启 AP 热点，用手机连接后访问配置页面填入 Wi-Fi 账号密码
3. 连接成功后自动同步时间并拉取天气

### 相册

1. 将 TF 卡格式化为 FAT32
2. 在卡根目录创建 `DCIM/` 文件夹，放入图片或 AVI 视频
3. 插入设备，进入「相册」页，等待扫描完成

**视频要求**（MJPEG AVI）：

```bash
# 用 ffmpeg 把任意视频转成设备可播放格式
ffmpeg -i input.mp4 -c:v mjpeg -q:v 8 -s 320x240 -r 15 \
       -c:a pcm_s16le -ar 44100 -ac 2 \
       -f avi output.avi
```

参数说明：

- **必须是 MJPEG 编码**（`mjpeg` / `MJPG`），H.264/H.265 无法解码
- **必须是 baseline JPEG**，progressive 会解码失败
- 分辨率建议 320×240（更大分辨率可解码但帧率下降）
- 音频必须为 PCM 16 位，采样率 8k~96k

### 摄像头

插入 UVC 免驱摄像头后自动开始预览。建议使用支持 MJPG 输出的型号。

---

## 已知限制

| 限制 | 说明 |
|---|---|
| **无 OTA 升级** | 分区表未预留 OTA 分区，只能通过 USB 烧录升级 |
| **视频帧率受 CPU 限制** | ESP32-S3 无硬件 JPEG 解码，320×240 约 15~20 fps；640×480 约 8~12 fps |
| **内部 RAM 紧张** | UVC 的 URB、I2S 的 DMA 缓冲都在内部 RAM；摄像头与音频同时使用时可能分配失败 |
| **摄像头无热插拔恢复** | `usb_camera_deinit()` 尚未实现，拔出后需要重启 |
| **凭据需手动配置** | Wi-Fi 热点密码、天气 API Key 默认值为空，需在 menuconfig 中配置 |
| **BLE 未启用** | 当前只支持 Wi-Fi，`CONFIG_BT_ENABLED` 关闭 |

---

## 调试

### 常用日志过滤

```bash
idf.py monitor | grep -E "usb_camera|album_video|lvgl"
```

### 内存水位

关键内部 RAM 分配点（UVC URB、I2S DMA、TJPGD 工作池）失败时，优先检查：

```c
heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
```

内部 RAM 碎片化会导致「总空闲够但单次分配失败」。

### 解码性能

`usb_camera` 每 30 帧打印一次：

```
usb_camera: decode 320x240 -> 320x240 scale=1/1 18342us px0=6320
```

`usb_camera: decode avg XXX us` 用于判断是否达到目标帧率（15 fps 对应 66667 us）。

---

## 开发说明

### UI 修改流程

1. 用 **GuiGuider** 编辑 `.gui` 工程，导出到 `main/ui/generated/`
2. 业务逻辑写在 `main/ui/custom/`，不要修改 `generated/` 下的文件
3. 屏幕采用**懒加载**：`setup_ui()` 只创建 home 屏，其它屏首次访问时才创建

### 代码风格

- 缩进 4 空格
- 驱动代码放在 `components/ap_wifi/bsp/`
- 新增组件需在 `CMakeLists.txt` 的 `REQUIRES` / `PRIV_REQUIRES` 中声明依赖

---

## 许可证

[MIT License](LICENSE)
