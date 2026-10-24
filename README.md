# minimal-agent-0.1.9-tft-calibration

ESP8266 NodeMCU v2 + ST7735S 128×160 SPI 屏幕校准源码快照。独立分支保存，不修改 main。

## 当前状态

- **目标编译已通过，0.1.9 尚未完成实板验证。** 0.1.8 已能显示文字和色块，但照片显示红蓝互换，右、下边框不完整。
- 0.1.9 改用 Adafruit `INITR_BLACKTAB`：RGB 色序、初始偏移 `(0,0)`。这是待实板确认的驱动配置，不代表 PCB 或贴纸的颜色。每次启动仅初始化一次，保留 4MHz SPI、原接线及原图形位置。
- **已知看门狗重启尚未解决。** 最近一次从 0.1.8 升级到 0.1.9 的 OTA 中断，设备重启后仍为 0.1.8。不能把本版当作看门狗修复版，也不能把编译通过当作成功安装。
- 本分支提供完整源码、构建配置和原有工具，**不包含预编译 `.bin` 或 `.elf` 下载文件**。下方的已构建产物哈希仅作身份记录，不是下载链接。

## 接线

屏幕 | NodeMCU 标号 | GPIO / 电源
--- | --- | ---
SCK | D5 | GPIO14
MOSI / SDA | D7 | GPIO13
RES / RESET | D0 | GPIO16
RS / DC | D1 | GPIO5
CS | D2 | GPIO4
GND | GND | 地
VCC | 3V3 | 3.3V
LEDA | 3V3 | 3.3V 背光

**DC 和 CS 不要互换。** D4 / GPIO2 保留给低电平点亮的板载呼吸灯。硬件 SPI 同时配置 D6 / GPIO12 为 MISO，屏幕未接此线，保持悬空；不启用 D8 硬件片选。不要把信号接到 5V。

## 保留的功能

- 开机即运行板载 LED；原始 SYS Ticker 驱动，明暗两段各独立随机 1–3 秒，不依赖联网成功。网络、加密或 Flash 操作仍可能带来时序抖动。
- 屏幕只在启动时画一次 RGBW 色块、ASCII 文字和外框，没有全屏帧缓冲、后台刷新或额外屏幕定时器。
- 保留 0.1.5 的三个 `noinline` TLS/OTA 栈隔离修复、启动诊断、证书校验、签名 OTA、配置和任务序号逻辑。
- `docs/fixes/` 内记录的是历史 0.1.5 验证，不代表 0.1.9 已通过相同实板验收。

## Windows 本地构建

需要 Git、Python 3.11+；没有工具时先安装其官方版本。以下命令在 PowerShell 执行。用虚拟环境中的 Python，无需修改 PowerShell 执行策略。

```powershell
git clone --branch snapshot/minimal-agent-0.1.9-tft-calibration --single-branch https://github.com/dignifnrfb/esp8266-minimal-agent.git
cd esp8266-minimal-agent
python -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r requirements.txt
.\.venv\Scripts\python.exe -m platformio run -e minimal_agent -j 1
```

Linux/macOS 对应使用 `.venv/bin/python`。

构建产物：`.pio/build/minimal_agent/firmware.bin` 和同目录 `firmware.elf`。请保存同次构建的 ELF；不同环境、路径重建的二进制可能不同，解码崩溃地址必须匹配实际烧录的 ELF。

依赖：PlatformIO 6.2.0，espressif8266 4.2.1（Arduino 3.1.2），ArduinoJson 6.21.5，Adafruit ST7735/ST7789 1.11.0，GFX 1.11.11，BusIO 1.16.1。库元数据可能下载官方 seesaw/SD 间接依赖；`lib_ignore` 将其排除在本固件的编译依赖图之外。不要混用此前的旧构建目录或更改内存布局。

目标：`nodemcuv2`，4MiB Flash / 1MiB LittleFS，160MHz，DIO，保留共享 IRAM 堆配置。项目平台配置优先于开发板的默认参数。

## USB 烧录（会重启设备）

确认接线、供电和 COM 口后，关闭其他占用串口的软件。把下面的 `COM9` 换成 Windows 设备管理器中本板的实际端口；不要不加确认地复制端口号。

```powershell
.\.venv\Scripts\python.exe -m platformio run -e minimal_agent -t upload --upload-port COM9
```

这是编译并上传当前分支程序的 USB 命令，不会发送服务端 OTA 任务。**不要执行整片擦除、`erase`、`uploadfs`，也不要更改 Flash/文件系统布局。** 已有 Wi-Fi、Token、CA、公钥及任务序号保存在 LittleFS，程序升级应保留它们。不要重新生成生产签名密钥。USB 烧录能绕过当前 OTA 下载问题，但不等于修复运行时看门狗。

正常 USB 使用未签名的 `firmware.bin`；**不要把附加了签名和长度的 OTA 包当作原始 USB 镜像。** 本仓库不含真实凭据、设备配置、签名密钥或整片 Flash。

应用串口为 115200，ROM 启动日志为 74880。查看日志时可使用已有串口工具；DTR/RTS 控制可能导致开口自动复位，须与自发重启区分。分享日志前遮去设备标识、服务器地址及任何凭据。

## 文件身份和验证边界

`src/minimal/main.cpp` SHA256：
`b285b944777207d936a95dd2cd88d577319b73f9801bd8041494d3a15be5934c`

已完成目标构建的原始程序镜像：478240 字节，SHA256：
`4bf4a47106d5c67b1638f9bc91b228c954f6cd5d20fe8241aaad4f8a1aa05ad5`

该次构建 ELF SHA256：
`6926b813c957a70337248968246e0f7ade6f139c276eef8d7042741035837518`

构建占用：RAM 32220 字节，Flash 474087 字节。原有主机协议/签名包检查工具保留于 `tools/`、`tests/`；有 g++、OpenSSL CLI 和构建依赖后可运行 `python tools/check.py --skip-build`。这些检查不证明硬件无重启。

本次校准的局部主机检查通过：精确变更范围、显示调用边界、4MHz 初始化分派、ASCII 文本范围，以及 1244 个周期的 LED 逻辑回归。它们不替代实际 GPIO、WDT、TLS/OTA 压力和照片验收。

烧录后应确认 R/G/B/W 色块与标签一致、四条边框完整，随后另行验证稳定性；当前仍待完成。结构化构建身份见 [build-manifest.json](build-manifest.json)。

## 官方参考

- [PlatformIO NodeMCU v2](https://docs.platformio.org/en/latest/boards/espressif8266/nodemcuv2.html)
- [Adafruit ST7735 1.11.0 初始化和旋转实现](https://github.com/adafruit/Adafruit-ST7735-Library/blob/1.11.0/Adafruit_ST7735.cpp)
- [ESP8266 Arduino 3.1.2 SPI](https://github.com/esp8266/Arduino/blob/3.1.2/libraries/SPI/SPI.cpp)
