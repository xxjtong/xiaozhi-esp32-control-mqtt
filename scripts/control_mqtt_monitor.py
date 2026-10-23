#!/usr/bin/env python3
"""Monitor the second (control) MQTT broker for device online/offline events.

The firmware publishes on the reply topic whenever a device comes up or changes
state:

    {"type": "hello",          "device_id": "...", "status": "idle", "version": "2.4.2"}
    {"type": "session_update", "device_id": "...", "status": "listening"}
    {"type": "goodbye",        "device_id": "..."}
    {"type": "reply",          "command_type": "...", "success": true, ...}

This script subscribes to the reply topic of every device and prints
ONLINE / OFFLINE / STATE events.

Usage:
    python3 scripts/control_mqtt_monitor.py
    python3 scripts/control_mqtt_monitor.py --probe-interval 60 --offline-timeout 150
    python3 scripts/control_mqtt_monitor.py --json | jq .
    python3 scripts/control_mqtt_monitor.py --topic 'xiaozhi/+/reply' -v

Exit with Ctrl+C to get a summary.
"""

from __future__ import annotations

import argparse
import json
import signal
import sys
import time
from dataclasses import dataclass, field
from datetime import datetime

try:
    import paho.mqtt.client as mqtt
except ImportError:  # pragma: no cover - dependency hint
    sys.exit("paho-mqtt is required: python3 -m pip install paho-mqtt")

DEFAULT_HOST = "mqtt.virsical.cn"
DEFAULT_PORT = 21883
DEFAULT_USERNAME = "admin"
DEFAULT_PASSWORD = "public"
DEFAULT_TOPIC = "xiaozhi/+/reply"
DEFAULT_CMD_TOPIC = "xiaozhi/{device_id}/cmd"


@dataclass
class Device:
    device_id: str
    online: bool = False
    state: str = "unknown"
    version: str = ""
    first_seen: float = field(default_factory=time.time)
    last_seen: float = field(default_factory=time.time)

    @property
    def short_id(self) -> str:
        return self.device_id[-8:] if len(self.device_id) > 8 else self.device_id


class Monitor:
    def __init__(self, args: argparse.Namespace) -> None:
        self.args = args
        self.devices: dict[str, Device] = {}
        self.connected = False
        self._last_probe = 0.0

        self.client = self._create_client()
        self.client.on_connect = self._on_connect
        self.client.on_disconnect = self._on_disconnect
        self.client.on_message = self._on_message
        self.client.reconnect_delay_set(min_delay=1, max_delay=30)

    # -- paho compatibility -------------------------------------------------

    def _create_client(self):
        client_id = f"xiaozhi-monitor-{int(time.time()) % 100000}"
        protocol = getattr(mqtt, "MQTTv311", 4)
        if hasattr(mqtt, "CallbackAPIVersion"):  # paho-mqtt >= 2.0
            return mqtt.Client(
                mqtt.CallbackAPIVersion.VERSION2,
                client_id=client_id,
                protocol=protocol,
            )
        return mqtt.Client(client_id=client_id, protocol=protocol)

    # -- output helpers -----------------------------------------------------

    def _emit(self, event: str, device: Device, detail: str = "") -> None:
        now = datetime.now().strftime("%H:%M:%S")
        if self.args.json:
            record = {
                "time": datetime.now().isoformat(timespec="seconds"),
                "event": event,
                "device_id": device.device_id,
                "state": device.state,
                "version": device.version,
                "detail": detail,
            }
            print(json.dumps(record, ensure_ascii=False), flush=True)
            return
        print(
            f"[{now}] {event:<8} {device.short_id:<10} "
            f"state={device.state:<17} {detail}".rstrip(),
            flush=True,
        )

    def _log(self, message: str) -> None:
        stamp = datetime.now().strftime("%H:%M:%S")
        if self.args.json:
            # Keep diagnostics in the JSON stream so pipelines do not lose them.
            print(
                json.dumps(
                    {
                        "time": datetime.now().isoformat(timespec="seconds"),
                        "event": "log",
                        "message": message,
                    },
                    ensure_ascii=False,
                ),
                flush=True,
            )
            return
        print(f"[{stamp}] {message}", file=sys.stderr, flush=True)

    # -- MQTT callbacks -----------------------------------------------------

    def _on_connect(self, client, userdata, flags, rc, properties=None) -> None:
        code = getattr(rc, "value", rc)  # paho 2.x passes a ReasonCode object
        if code != 0:
            self._log(f"connect refused: rc={rc}")
            return
        self.connected = True
        self._log(f"connected to {self.args.host}:{self.args.port}, subscribing {self.args.topic}")
        client.subscribe(self.args.topic, qos=0)

    def _on_disconnect(self, client, userdata, *args) -> None:
        self.connected = False
        # paho 2.x: (flags, reason_code, properties); paho 1.x: (rc,)
        raw = args[1] if len(args) >= 2 else (args[0] if args else "?")
        code = getattr(raw, "value", raw)
        self._log(f"disconnected (rc={code}), will retry")

    def _on_message(self, client, userdata, msg) -> None:
        payload = msg.payload.decode("utf-8", errors="replace")
        if self.args.verbose:
            self._log(f"{msg.topic} <- {payload}")
        self.handle_message(msg.topic, payload)

    # -- message handling ---------------------------------------------------

    def handle_message(self, topic: str, payload: str) -> None:
        """Process one reply-topic message. Extracted for testability."""
        try:
            data = json.loads(payload)
        except json.JSONDecodeError:
            self._log(f"non-JSON payload on {topic}: {payload[:120]}")
            return
        if not isinstance(data, dict):
            return

        msg_type = data.get("type")
        device_id = str(data.get("device_id") or "").strip()
        if not device_id:
            # Fall back to the topic: xiaozhi/<device_id>/reply
            parts = topic.split("/")
            if len(parts) >= 3:
                device_id = parts[-2]
        if not device_id:
            return

        device = self.devices.get(device_id)
        if device is None:
            device = Device(device_id=device_id)
            self.devices[device_id] = device

        device.last_seen = time.time()
        if data.get("version"):
            device.version = str(data["version"])

        if msg_type == "hello":
            device.state = str(data.get("status") or "unknown")
            if not device.online:
                device.online = True
                self._emit("ONLINE", device, f"version={device.version or '?'}")
            else:
                self._emit("HELLO", device, f"version={device.version or '?'}")

        elif msg_type == "session_update":
            previous = device.state
            device.state = str(data.get("status") or "unknown")
            if not device.online:
                device.online = True
                self._emit("ONLINE", device, "")
            if device.state != previous:
                self._emit("STATE", device, f"{previous} -> {device.state}")

        elif msg_type == "goodbye":
            if device.online:
                device.online = False
                self._emit("OFFLINE", device, "(goodbye)")
            else:
                self._emit("GOODBYE", device, "")

        elif msg_type == "reply":
            # Replies carry the state in "device_state" instead of "status".
            reported = data.get("device_state")
            if reported:
                device.state = str(reported)
            if self.args.verbose:
                self._emit("REPLY", device, str(data.get("command_type") or ""))

    # -- periodic probing ---------------------------------------------------

    def probe_devices(self) -> None:
        """Ask known devices for their status so silent ones can be detected."""
        if not self.connected or self.args.probe_interval <= 0:
            return
        now = time.time()
        if now - self._last_probe < self.args.probe_interval:
            return
        self._last_probe = now
        payload = json.dumps({"type": "query_status"})
        for device in self.devices.values():
            if not device.online:
                continue
            topic = self.args.cmd_topic.format(device_id=device.device_id)
            self.client.publish(topic, payload, qos=0)

    def check_timeouts(self) -> None:
        if self.args.offline_timeout <= 0:
            return
        now = time.time()
        for device in self.devices.values():
            if device.online and now - device.last_seen > self.args.offline_timeout:
                device.online = False
                silence = int(now - device.last_seen)
                self._emit("OFFLINE", device, f"(silent {silence}s)")

    # -- summary ------------------------------------------------------------

    def summary(self) -> None:
        total = len(self.devices)
        online = [d for d in self.devices.values() if d.online]
        if self.args.json:
            print(
                json.dumps(
                    {
                        "event": "summary",
                        "devices_seen": total,
                        "devices_online": len(online),
                        "online_ids": [d.device_id for d in online],
                    },
                    ensure_ascii=False,
                ),
                flush=True,
            )
            return
        print("\n=== summary ===")
        print(f"devices seen: {total}, currently online: {len(online)}")
        for device in sorted(self.devices.values(), key=lambda d: d.short_id):
            flag = "online " if device.online else "offline"
            print(f"  {device.short_id:<10} {flag}  state={device.state:<17} {device.device_id}")


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Monitor the XiaoZhi control MQTT broker for device online/offline events.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument("--host", default=DEFAULT_HOST, help="broker host")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT, help="broker port")
    parser.add_argument("--username", default=DEFAULT_USERNAME, help="broker username")
    parser.add_argument("--password", default=DEFAULT_PASSWORD, help="broker password")
    parser.add_argument("--topic", default=DEFAULT_TOPIC, help="reply topic filter to subscribe")
    parser.add_argument(
        "--cmd-topic",
        default=DEFAULT_CMD_TOPIC,
        help="per-device command topic template, used when probing",
    )
    parser.add_argument(
        "--probe-interval",
        type=float,
        default=0,
        help="seconds between query_status probes to known devices (0 disables)",
    )
    parser.add_argument(
        "--offline-timeout",
        type=float,
        default=0,
        help="mark a device offline after this many silent seconds (0 disables)",
    )
    parser.add_argument("--json", action="store_true", help="emit JSON lines instead of text")
    parser.add_argument("-v", "--verbose", action="store_true", help="also print raw messages")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    monitor = Monitor(args)

    if args.offline_timeout > 0 and args.probe_interval <= 0:
        print(
            "warning: --offline-timeout without --probe-interval will mark idle "
            "devices offline, because they only publish on state changes",
            file=sys.stderr,
            flush=True,
        )

    if args.password:
        monitor.client.username_pw_set(args.username, args.password)
    elif args.username:
        monitor.client.username_pw_set(args.username)

    try:
        monitor.client.connect(args.host, args.port, keepalive=60)
    except OSError as exc:
        print(f"cannot connect to {args.host}:{args.port}: {exc}", file=sys.stderr)
        return 2

    monitor.client.loop_start()

    def _stop(signum, frame):  # noqa: ARG001
        raise KeyboardInterrupt

    signal.signal(signal.SIGTERM, _stop)

    try:
        while True:
            time.sleep(1)
            monitor.probe_devices()
            monitor.check_timeouts()
    except KeyboardInterrupt:
        pass
    finally:
        monitor.client.loop_stop()
        monitor.client.disconnect()

    monitor.summary()
    return 0


if __name__ == "__main__":
    sys.exit(main())
