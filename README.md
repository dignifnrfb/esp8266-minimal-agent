# minimal-agent-0.1.4-breathe-diag 源码快照

本分支保存已通过签名 OTA 安装的 0.1.4 源码，供本地复现和排查。它不是看门狗问题修复版，也不是新的部署。main 分支未修改。

## 源码身份
- 原始构建基线：`f472400415fe753edae1733d4a2049d6aef953f1`。
- 固件版本字符串：`minimal-agent-0.1.4-breathe-diag`（结尾是 diag）。
- `src/minimal/main.cpp` SHA-256：
  `a1c2a7663d80cb0f6181a84b8e32464a5b0ea59428c1c0fd68bf1f50dd31464a`。
- 除 main.cpp 和本说明外，构建配置、protocol.h、工具及原有测试均保留上述基线原文。未混入后续修复。
- 源码相同不保证不同路径或工具环境下的二进制逐字节相同；排查地址必须配套实际构建的 ELF。

## 本地构建（不烧录）
建议 Python 3.11+。仓库锁定 PlatformIO 6.2.0、espressif8266 4.2.1（ESP8266 Arduino 3.1.2）和 ArduinoJson 6.21.5。

```sh
git clone --branch snapshot/minimal-agent-0.1.4-breathe-diag --single-branch https://github.com/dignifnrfb/esp8266-minimal-agent.git
cd esp8266-minimal-agent
python -m venv .venv
# Windows PowerShell:
.venv\Scripts\Activate.ps1
# Linux/macOS 则使用：source .venv/bin/activate
python -m pip install -r requirements.txt
python -m platformio run -e minimal_agent -j 1
```

产物：`.pio/build/minimal_agent/firmware.bin` 和同目录 `firmware.elf`。
目标 NodeMCU v2，4 MiB Flash / 1 MiB LittleFS，160 MHz、DIO，内存布局由 platformio.ini 指定。

可选主机检查：安装 g++ 和 OpenSSL CLI 后运行 `python tools/check.py --skip-build`。原有测试检查协议和签名包，不验证呼吸灯的实际时序，也不能证明设备不会重启。本次仅发布已安装源码，未重新运行完整工具链测试。

Windows 可用 `Get-FileHash src/minimal/main.cpp -Algorithm SHA256` 检查源码；Linux 使用 `sha256sum src/minimal/main.cpp`。

## 当前行为及排查边界
- GPIO2 / D4 板载 LED，低电平点亮；首次认证轮询成功后启动随机平滑呼吸。
- PWM 1 kHz，范围显式设为 1023，亮度约 10%～80%。每轮 2～6 秒是有效动画时间；同步 TLS/HTTP/OTA 会暂停亮度包络，因此墙钟周期可能更长。
- 每次轮询附带同一个 boot_diag：版本 v=1、reason、boot_heap、prev_valid=false；reason 1～3 时附带 exccause、epc1、excvaddr。服务器必须支持该字段。
- 不写 RTC 或额外 Flash 故障日志。突然断电或首次成功上报前的重启可能没有完整记录，boot_heap 只是重启后的快照。
- 已观察到软件看门狗（reason=3）及硬件看门狗（reason=1）重启，根因尚未确认。硬件看门狗不等于硬件损坏。
- exccause=4 是一级中断，该情况下 EXCVADDR 不更新，不能把 excvaddr 当作非法访问地址。匹配 ELF 的 epc1 符号有助分析，但单个地址不等于根因。
- 完整串口崩溃输出可能有更多信息，应用串口速率 115200；启动 ROM 输出使用 74880。保存与固件匹配的 ELF，分享日志前去除凭据和个人网络信息。

参考：[官方 SDK reset 枚举](https://github.com/esp8266/Arduino/blob/3.1.2/tools/sdk/include/user_interface.h)、[官方异常代码表](https://arduino-esp8266.readthedocs.io/en/3.1.2/exception_causes.html)。

## 配置和升级注意
本仓库不含真实 Wi-Fi 密码、设备 Token、生产公私钥、服务端凭据或设备配置；这些仍由设备持有者私下保管。配置沿用 LittleFS，不应为排查直接擦除或上传文件系统。

[原始基线的协议及 USB 配置说明](https://github.com/dignifnrfb/esp8266-minimal-agent/blob/f472400415fe753edae1733d4a2049d6aef953f1/README.md)中的示例版本号属于旧版本，配置格式与签名协议仍适用。不要运行 keygen 替换现有生产签名身份；普通本地构建不需要私钥。这个分支没有自动回滚能力，错误固件可能需要 USB 恢复。
