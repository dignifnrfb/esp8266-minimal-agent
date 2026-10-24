# minimal-agent-0.1.5-stack-fix

修复 `0.1.4-breathe-diag` 在 HTTPS 轮询期间反复触发看门狗的问题，基于源码快照 `256564e7dc47f57a5c4445c0a672cc39db81662b`。

## 原因和修复

ESP8266 Arduino 3.1.2 的 continuation 栈为 4096 字节。旧版优化构建把 `receivePoll` 和 `updateFirmware` 内联进 `tick`，连没有任务的普通轮询也预留了 OTA 的大块局部变量；旧版诊断构建中实测 `tick` 栈帧为 1376 字节。TLS 调用又叠加框架和加密调用栈，原版诊断中栈余量降到 48 字节，并直接捕获了保护值、stack_end、pc_ret、pc_suspend 被改写、`can_yield()=0`，随后 Soft WDT。

本版把任务处理、OTA 连接和镜像流处理分成禁止内联的函数，避免把 OTA 的哈希对象和 512 字节缓冲提前带入轮询及 TLS 握手栈。编译器、框架、CPU 频率、MMU/共享 IRAM 堆、TLS 缓冲、超时、会话复用和证书验证均沿用 0.1.4。呼吸灯、RSA2048 签名升级、LittleFS 配置及任务序号保留。

USB `status` 增加 `stack_ok`、`stack_free` 和 `reset_reason`。`stack_free` 是本次启动以来 continuation 栈的最低余量（字节），不是剩余堆内存。轮询 `boot_diag` 协议仍是 v1。

## 构建

Python 3.11+，依赖锁定 PlatformIO 6.2.0、espressif8266 4.2.1（Arduino 3.1.2）和 ArduinoJson 6.21.5。

```sh
python -m venv .venv
# Windows: .venv\Scripts\Activate.ps1
# Linux/macOS: source .venv/bin/activate
python -m pip install -r requirements.txt
python -m platformio run -e minimal_agent -j 1
```

产物为 `.pio/build/minimal_agent/firmware.bin` 和配套 `firmware.elf`。NodeMCU v2，4 MiB Flash / 1 MiB LittleFS，160 MHz、DIO。以本分支的完整源码和 platformio.ini 构建，不要使用此前未通过实板验证的超时/内存候选。

主机检查需要 g++ 和 OpenSSL CLI：

```sh
python tools/check.py --skip-build
```

## 验证

实板 COM9 验证：连续 600.2 秒，新增 133 次成功轮询，boot_id 不变，无看门狗复位，栈保护值全程正常，最低栈余量 964 字节。主动软件重启后已恢复 7 次成功轮询，配置、公钥就绪状态及任务高水位保留。45 项协议检查和 9 项签名包检查通过。

主机协议和签名包检查不替代实板 HTTPS OTA 验收。生产私钥在服务器；本次 USB 烧录不能证明签名 OTA 下载、提交及新版本心跳已经通过。

[完整排查与修复日志](docs/fixes/2026-10-24-tls-stack-overflow.md)记录了复位证据、未通过的对照、栈帧变化及实板结果，并附有可读取的 JSON 验证摘要。

## 功能和配置

- GPIO2 / D4 板载 LED，LOW 亮；首次成功轮询后开始 1 kHz 随机平滑呼吸。每轮 2～6 秒为有效动画时间，同步网络请求会暂停亮度包络。
- 保留 CA、域名、时间和 TLS1.2 验证，接收/发送缓冲为 16384/1024 字节，TLS admission 为 28500 字节。
- 协议1，支持 status、device_inspect(environment)、firmware_update。OTA 包仍为镜像 + RSA2048 PKCS#1 v1.5 SHA256 签名 + 小端签名长度。
- Wi-Fi、设备 Token、CA、公钥、任务高水位及 ACK 继续保存在 LittleFS。升级时不要擦除或上传文件系统，不要重新生成生产签名身份。
- 不额外写 RTC 或 Flash 故障日志；保留实际构建的 ELF 供解码。

仓库不含真实凭据、生产签名材料、设备配置、整片 Flash 或原始串口捕获。

[基线协议与 USB 配置说明](https://github.com/dignifnrfb/esp8266-minimal-agent/blob/f472400415fe753edae1733d4a2049d6aef953f1/README.md)的配置和签名协议仍适用；示例版本号属于旧版。
