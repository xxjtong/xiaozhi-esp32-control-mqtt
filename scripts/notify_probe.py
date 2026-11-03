#!/usr/bin/env python3
"""Probe the second MQTT control channel's notification feedback.

Exercises the notify / abort_notify commands added for the broadcast closed loop
(see docs/control-mqtt-notify-feedback.md) and prints the replies plus the
notify_result reported by the device.

Requires paho-mqtt (and pyserial for the optional serial log).

Examples:
    # Discover the device id by resetting it and waiting for hello
    python3 notify_probe.py --discover

    # Query status and check for the notify_active field
    python3 notify_probe.py --device <uuid> --case query

    # Play a notification (audio_url must be a direct link, no redirects)
    python3 notify_probe.py --device <uuid> --case notify --audio-url http://host/a.ogg

    # Start a notification, cancel it after 2.5 s, expect outcome=cancelled
    python3 notify_probe.py --device <uuid> --case abort --audio-url http://host/a.ogg

    # Send abort_notify while idle, expect success=false
    python3 notify_probe.py --device <uuid> --case abort-idle
"""
import argparse
import json
import re
import sys
import threading
import time

try:
    import paho.mqtt.client as mqtt
except ImportError:
    sys.exit("paho-mqtt is required: pip install paho-mqtt")


def parse_args():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--broker", default="mqtt.virsical.cn")
    p.add_argument("--port", type=int, default=21883)
    p.add_argument("--user", default="admin")
    p.add_argument("--password", default="public")
    p.add_argument("--device", help="device uuid; omit with --discover")
    p.add_argument("--discover", action="store_true",
                   help="reset the board over serial and read the uuid from hello")
    p.add_argument("--serial", default="/dev/cu.usbmodem1101")
    p.add_argument("--no-serial", action="store_true", help="do not read the uart log")
    p.add_argument("--case", default="query",
                   choices=["query", "notify", "abort", "abort-idle"])
    p.add_argument("--audio-url", help="direct http(s) link to a mono Ogg Opus file")
    p.add_argument("--notify-id", default="probe-001")
    p.add_argument("--cancel-after", type=float, default=2.5,
                   help="seconds to wait before abort_notify (case=abort)")
    return p.parse_args()


class Probe:
    def __init__(self, args):
        self.args = args
        self.messages = []
        self.device = args.device
        self.client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
        self.client.username_pw_set(args.user, args.password)
        self.client.on_connect = self._on_connect
        self.client.on_message = self._on_message

    def _on_connect(self, client, userdata, flags, rc, properties=None):
        topic = "xiaozhi/+/reply" if self.device is None else f"xiaozhi/{self.device}/reply"
        client.subscribe(topic)

    def _on_message(self, client, userdata, msg):
        try:
            data = json.loads(msg.payload.decode())
        except ValueError:
            return
        self.messages.append(data)
        print(f"  <- {json.dumps(data, ensure_ascii=False)}")

    def start(self):
        self.client.connect(self.args.broker, self.args.port, 60)
        self.client.loop_start()
        time.sleep(1.2)

    def stop(self):
        self.client.loop_stop()
        self.client.disconnect()

    def publish(self, payload):
        print(f"  -> {json.dumps(payload, ensure_ascii=False)}")
        self.client.publish(f"xiaozhi/{self.device}/cmd", json.dumps(payload), qos=0)

    def wait_for(self, kind, timeout):
        """Wait until a message of the given type (or command_type) arrives."""
        end = time.time() + timeout
        while time.time() < end:
            for m in self.messages:
                if m.get("type") == kind or m.get("command_type") == kind:
                    return m
            time.sleep(0.2)
        return None


def reset_board(port):
    """Pulse RTS to reboot the board so it emits a fresh hello."""
    try:
        import serial
    except ImportError:
        print("[!] pyserial missing, cannot reset the board")
        return False
    try:
        s = serial.Serial(port, 115200)
        s.setDTR(False)
        s.setRTS(True)
        time.sleep(0.2)
        s.setRTS(False)
        time.sleep(0.3)
        s.setRTS(True)
        s.close()
        return True
    except Exception as e:
        print(f"[!] reset failed: {e}")
        return False


def uart_reader(port, duration, pattern=r"NotifyPlayer|Notification|StateMachine|abort"):
    try:
        import serial
    except ImportError:
        return
    try:
        s = serial.Serial(port, 115200, timeout=1)
    except Exception:
        return
    end = time.time() + duration
    while time.time() < end:
        data = s.read(4096)
        if not data:
            continue
        for line in data.decode("utf8", "ignore").splitlines():
            if re.search(pattern, line, re.I):
                print(f"    [uart] {line.strip()}")
    s.close()


def start_uart(args, duration):
    if args.no_serial:
        return None
    t = threading.Thread(target=uart_reader, args=(args.serial, duration), daemon=True)
    t.start()
    return t


def main():
    args = parse_args()
    probe = Probe(args)
    probe.start()

    if args.discover or not args.device:
        print("[..] discovering device id (resetting board)")
        reset_board(args.serial)
        hello = probe.wait_for("hello", 90)
        if not hello:
            print("[!!] no hello received")
            return 1
        probe.device = args.device = hello["device_id"]
        print(f"[ok] device_id = {probe.device}")
        if args.discover and args.case == "query" and not args.device:
            pass

    uart = start_uart(args, 30)

    if args.case == "query":
        probe.publish({"type": "query_status"})
        probe.wait_for("query_status", 6)
        reply = [m for m in probe.messages if m.get("command_type") == "query_status"]
        if reply:
            extra = reply[-1].get("extra", {})
            print("\n=== notify 相关字段 ===")
            print(f"  notify_active = {extra.get('notify_active', '(缺失!)')}")
            print(f"  notify_id     = {extra.get('notify_id', '(无，未在播报)')}")

    elif args.case == "notify":
        if not args.audio_url:
            sys.exit("--audio-url is required for case=notify")
        probe.publish({"type": "notify", "id": args.notify_id,
                       "audio_url": args.audio_url})
        time.sleep(12)

    elif args.case == "abort":
        if not args.audio_url:
            sys.exit("--audio-url is required for case=abort")
        probe.publish({"type": "notify", "id": args.notify_id,
                       "audio_url": args.audio_url})
        time.sleep(args.cancel_after)
        probe.publish({"type": "abort_notify"})
        time.sleep(6)

    elif args.case == "abort-idle":
        probe.publish({"type": "query_status"})
        time.sleep(2)
        probe.publish({"type": "abort_notify"})
        time.sleep(3)

    if uart:
        uart.join(timeout=5)
    probe.stop()

    result = [m for m in probe.messages if m.get("type") == "notify_result"]
    if result:
        last = result[-1]
        print(f"\n=== notify_result ===\n  id={last.get('id')} "
              f"outcome={last.get('outcome')} duration_ms={last.get('duration_ms')}")
    elif args.case in ("notify", "abort"):
        print("\n[!!] 未收到 notify_result")
    return 0


if __name__ == "__main__":
    sys.exit(main())
