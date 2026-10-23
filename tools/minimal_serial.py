#!/usr/bin/env python3
"""USB provisioning for minimal_agent. No credential values are printed."""
import argparse
import json
from pathlib import Path
import time


def receive(port, timeout=20):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        line = port.readline()
        if line.startswith(b'{'):
            try:
                result = json.loads(line)
                if result.get('version', '').startswith('minimal-agent-'):
                    return result
            except (ValueError, UnicodeError):
                pass
    raise TimeoutError('No minimal firmware status received')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', required=True, help='For example COM9 or /dev/ttyUSB0')
    parser.add_argument('--device-id', help='Required for writes; must match USB status')
    parser.add_argument('--config', type=Path, help='Private JSON; never echoed')
    parser.add_argument('--public-key', type=Path, help='RSA2048 public key for firmware releases')
    parser.add_argument('--enable', action='store_true', help='Enable configured outbound polling')
    parser.add_argument('--command', choices=['status', 'pause', 'resume'], default='status')
    args = parser.parse_args()
    if (args.public_key or args.enable) and not args.config:
        parser.error('Provisioning options require --config')
    if (args.config or args.command != 'status') and not args.device_id:
        parser.error('Device writes require an explicit --device-id')
    try:
        import serial
        with serial.Serial(args.port, 115200, timeout=.3) as port:
            port.dtr = False
            port.rts = False
            port.reset_input_buffer()
            port.write(b'status\n')
            before = receive(port)
            if args.device_id and before.get('device_id') != args.device_id:
                raise ValueError('Connected device identity does not match')
            if args.config:
                config = json.loads(args.config.read_text(encoding='utf-8-sig'))
                if args.public_key:
                    config['firmware_public_key'] = args.public_key.read_text(encoding='utf-8-sig')
                if args.enable:
                    config['enabled'] = True
                if config.get('allow_lan_http') is not False:
                    raise ValueError('Minimal firmware requires HTTPS')
                raw = b'config ' + json.dumps(config, ensure_ascii=False, separators=(',', ':')).encode() + b'\n'
                if len(raw) > 8200:
                    raise ValueError('Configuration too large')
                port.write(b'pause\n')
                receive(port)
                # Pause TLS polling, then pace chunks to avoid overflowing UART RX.
                for start in range(0, len(raw), 64):
                    port.write(raw[start:start+64])
                    port.flush()
                    time.sleep(.02)
                result = receive(port)
                if not result.get('configured') or result['error'] not in ('disabled', 'waiting_for_wifi_and_clock'):
                    raise ValueError('Device rejected configuration')
            elif args.command != 'status':
                port.write((args.command + '\n').encode())
                result = receive(port)
            else:
                result = before
            print(json.dumps(result, ensure_ascii=False, indent=2))
    except Exception as exc:
        parser.exit(1, 'USB operation failed (%s); credentials were not printed.\n' % type(exc).__name__)


if __name__ == '__main__':
    main()
