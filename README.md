# ESP8266 minimal_agent 源码

独立的 `minimal-agent-0.1.0` PlatformIO 工程，目标为 NodeMCU / ESP8266、4 MiB Flash、1 MiB LittleFS。包含可构建源码、USB 配置脚本、服务器签名脚本和主机测试。

板端保留 Wi-Fi、NTP、TLS 1.2、设备认证、任务轮询/结果、持久化去重、RSA2048 签名的完整固件 HTTPS 拉取升级。网页、模型、对话历史和编译器由服务器实现。源码不包含设备凭据，也不依赖原工程。

## 构建和验证

建议 Python 3.11 及以上；主机测试还需 `g++` 和 OpenSSL 命令行工具。Linux 可先安装 `python3-venv build-essential openssl`。Windows 使用 Python、MinGW g++ 和 OpenSSL，并将其加入 PATH。

```sh
python -m venv .venv
# Linux/macOS: source .venv/bin/activate
# Windows PowerShell: .venv\Scripts\Activate.ps1
python -m pip install -r requirements.txt
python -m platformio run -e minimal_agent -j 1
python tools/check.py --skip-build
```

`python tools/check.py` 会先编译再验证。首次编译需下载工具链和锁定版本的 ArduinoJson 6.21.5；不需要访问设备服务器。`-j 1` 限制并行编译，适合资源较少的服务器。

产物为 `.pio/build/minimal_agent/firmware.bin`。`platformio.ini` 锁定 espressif8266 4.2.1、ArduinoJson 6.21.5，以及原板使用的 Flash/内存布局。测试包含 45 项协议边界检查及 9 项签名包检查，其中一项与已安装 ESP8266 Arduino 框架的 `tools/signing.py` 输出逐字节比较。

若工具链装在非默认目录，设置 `PLATFORMIO_CORE_DIR`。若验证另一份产物，设置 `MINIMAL_FIRMWARE`。仅构建与签名测试不代表已完成真实设备 OTA 测试。

2026-10-23 已从独立目录构建成功：程序 Flash 457,607 字节、静态 RAM 31,796 字节，45 项协议检查和 9 项签名包测试全部通过。此验证只使用已安装的工具链与新工程下载的依赖，不需要原工程的源码或配置。真实设备未烧录，真实 OTA 尚未测试。

## 服务器签名兼容性

签名格式直接兼容 ESP8266 Arduino Updater：

```text
未压缩的 firmware.bin | RSA2048 签名（256 字节） | uint32_le(256)
```

签名算法为 RSA PKCS#1 v1.5 + SHA-256，签名覆盖原始 `firmware.bin`。任务的 `size` 和 `sha256` 覆盖**整个签名包**，包括签名及长度尾部。仅支持未压缩、首字节为 `0xE9` 的应用镜像，签名包最多 1,048,576 字节；板端还会检查实际可用升级空间。

```sh
# private/ 目录必须在仓库外保存备份；私钥仅由服务器持有。
mkdir private
python tools/minimal_sign.py keygen --private private/release-private.pem --public private/release-public.pem
python tools/minimal_sign.py sign --private private/release-private.pem --image .pio/build/minimal_agent/firmware.bin --out private/release.bin.signed --version minimal-agent-0.1.0 --path /v1/firmware/release.bin.signed
python tools/minimal_sign.py verify --public private/release-public.pem --image private/release.bin.signed
```

`sign` 同时生成 `private/release.bin.signed.job.json`，内容是任务 `type` 和 `args`；服务器需要另加 `seq`、`id`，放进轮询响应的 `job`。私钥、真实配置和固件产物均被 `.gitignore` 排除。脚本拒绝覆盖现有密钥或发布产物。签名测试使用临时密钥，不会建立生产签名身份。

## HTTP 协议 1

所有设备请求使用 `Authorization: Bearer <device_token>`。配置 `server_url` 不带结尾 `/`；固件拼接以下路径：

| 请求 | 用途 |
| --- | --- |
| `POST /v1/device/poll` | 心跳和取一个任务 |
| `POST /v1/device/result` | 上报结果，未确认时重试 |
| `GET /v1/firmware/<name>` | 下载带签名的固件；也附带设备认证 |

轮询请求含 `protocol:1`、`device_id`、`boot_id`、`firmware`、`last_seq`、`uptime_s`、`heap`、`rssi`、`target`、`signed_ota_ready`、`job_types`。设备 ID 自动取 `esp8266-` 加无冒号的小写 MAC。

无任务的响应：

```json
{"protocol":1,"poll_after_ms":3000,"job":null}
```

升级任务响应结构如下；用签名脚本生成的真实参数替换示例占位值：

```json
{
  "protocol": 1,
  "poll_after_ms": 3000,
  "job": {
    "seq": 1,
    "id": "release-1",
    "type": "firmware_update",
    "args": {
      "path": "/v1/firmware/release.bin.signed",
      "version": "minimal-agent-0.1.0",
      "target": "esp8266:nodemcuv2:4m1m",
      "size": 500000,
      "sha256": "REPLACE_WITH_WHOLE_SIGNED_PACKAGE_SHA256"
    }
  }
}
```

支持的任务仅为 `status`（`args:{}`）、`device_inspect`（`args:{"resource":"environment"}`）和 `firmware_update`。任务 `seq` 必须是单设备严格递增的正 uint32；服务器应在收到确认之前保持同一个未完成任务，并按设备持久化序号，不能重置为 1。未知任务会返回终态失败。

结果请求含 `protocol`、`device_id`、`boot_id`、`seq`、`id`、`status`、`ok`、`data`。正确的确认响应是 `{"protocol":1,"ack":<该任务seq>}`。`status` 为 `completed`，或断电恢复情况下的 `indeterminate`；后者表示不能确定执行结果，服务器不应以新序号自动重放有副作用的动作。

JSON 响应最多 2048 字节。升级 GET 必须返回 200、准确的 `Content-Length` 和完整签名包，不支持 chunked、重定向或跨站下载。路径限 `/v1/firmware/` 下的安全字符。TLS 验证 CA、域名和时间，保留 16 KiB 接收缓冲；没有 `setInsecure()` 或 HTTP 降级。

下载先检查完整包 SHA-256，再由框架验证 RSA 签名并提交升级。成功结果中的 `data.state` 为 `verified_pending_reboot`，收到 ACK 后或最多 10 秒后重启；服务器还需等待新版本心跳才可标记部署完成。任务 `version` 只是请求元数据，不能代替实际运行版本验证。更新业务版本时应同时修改 `src/minimal/main.cpp` 的 `Version` 字符串。

## 首次 USB 配置

**本仓库尚未在真实板子上烧录或验证 OTA。** 首次将旧固件迁移到本固件需要 USB；现有完整固件不是此最小版本，不应直接按本协议向旧固件下发升级。

准备私有 JSON，字段如下（值由设备持有者填写）：

| 字段 | 说明 |
| --- | --- |
| `wifi_ssid` | 1–32 字节 |
| `wifi_password` | 空字符串或 8–64 字节 |
| `server_url` | HTTPS 地址，可包含端口/路径前缀 |
| `device_token` | 24–192 个可打印 ASCII 字符，无空白 |
| `ca_pem` | 完整 PEM 根证书，64–4096 字节 |
| `enabled` | 布尔值，是否启用轮询 |
| `allow_lan_http` | 必须为 `false` |
| `poll_ms` | 1000–60000，建议 3000 |
| `firmware_public_key` | 可选，RSA2048 公钥 PEM；未配置时禁用升级 |

本地首次 USB 烧录及配置示例，替换端口和实际设备 ID：

```sh
python -m platformio run -e minimal_agent -t upload --upload-port YOUR_PORT
python tools/minimal_serial.py --port YOUR_PORT
python tools/minimal_serial.py --port YOUR_PORT --device-id YOUR_DEVICE_ID --config private/config.json --public-key private/release-public.pem --enable
```

读取状态不要求预先知道 ID；写配置或暂停/恢复必须显式提供匹配 ID。配置脚本只输出脱敏状态，拒绝向非 minimal 固件写配置。配置保存在 LittleFS 的 `/minimal.json`；任务日志保留 `/gateway-job.json`、`/gateway-ack`，以兼容原设备任务序号。

串口速度 115200，支持 `status`、`pause`、`resume`、`reboot`、`config <一行JSON>`。没有板载网页、配网热点或局域网 OTA 推送服务。全新空白文件系统可初始化；损坏的非空文件系统不会自动格式化，需要人工恢复。

完整固件更新能够替换应用和驱动，需后续固件继续保留网络、签名升级与相同 Flash 布局。该方案没有独立不可改写的安全监管层，也没有自动回滚；错误应用仍可能需要 USB 恢复。
