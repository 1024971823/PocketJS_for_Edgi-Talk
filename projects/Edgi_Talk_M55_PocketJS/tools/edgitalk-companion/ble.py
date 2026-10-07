"""BlueZ client for the board's EdgiTalk advertiser.

Talks to bluetoothd through GDBus (PyGObject), which is already part of a
normal Linux desktop. The writable characteristic is 16-bit UUID 0xFFF1.
"""

from __future__ import annotations

import json
import os
import time
from typing import Any

import edgitalk

CHAR_UUID = "0000fff1-0000-1000-8000-00805f9b34fb"
ADAPTER = "/org/bluez/hci0"
_char_proxy: Any = None
_char_mac = ""


def adapter_present() -> bool:
    return os.path.isdir("/sys/class/bluetooth/hci0")


def _gi():
    try:
        from gi.repository import Gio, GLib
    except ImportError as error:
        raise edgitalk.CompanionError("这台电脑没有可用的蓝牙接口") from error
    return Gio, GLib


def _bus():
    Gio, _GLib = _gi()
    return Gio.bus_get_sync(Gio.BusType.SYSTEM, None)


def _proxy(path: str, interface: str):
    Gio, _GLib = _gi()
    return Gio.DBusProxy.new_sync(
        _bus(), Gio.DBusProxyFlags.NONE, None, "org.bluez", path, interface, None
    )


def _devices() -> list[tuple[str, dict[str, Any]]]:
    Gio, _GLib = _gi()
    manager = _proxy("/", "org.freedesktop.DBus.ObjectManager")
    objects = manager.call_sync("GetManagedObjects", None, Gio.DBusCallFlags.NONE, 4000, None).unpack()[0]
    found = []
    for path, ifaces in objects.items():
        device = ifaces.get("org.bluez.Device1")
        if device is not None:
            found.append((path, device))
    return found


def find_edgitalk(timeout: float = 8.0) -> str | None:
    """Return the MAC advertising the name EdgiTalk, or None."""
    if not adapter_present():
        return None
    Gio, GLib = _gi()
    try:
        adapter = _proxy(ADAPTER, "org.bluez.Adapter1")
        adapter.call_sync(
            "SetDiscoveryFilter",
            GLib.Variant("(a{sv})", ({"Transport": GLib.Variant("s", "le")},)),
            Gio.DBusCallFlags.NONE,
            4000,
            None,
        )
        adapter.call_sync("StartDiscovery", None, Gio.DBusCallFlags.NONE, 4000, None)
    except Exception:
        return None
    deadline = time.monotonic() + timeout
    found = ""
    try:
        while time.monotonic() < deadline and not found:
            for _path, device in _devices():
                alias = str(device.get("Alias", ""))
                name = str(device.get("Name", ""))
                if alias == "EdgiTalk" or name == "EdgiTalk":
                    found = str(device.get("Address", ""))
                    break
            if not found:
                time.sleep(0.4)
    finally:
        try:
            adapter.call_sync("StopDiscovery", None, Gio.DBusCallFlags.NONE, 2000, None)
        except Exception:
            pass
    return found or None


def _device_path(mac: str) -> str:
    return ADAPTER + "/dev_" + mac.replace(":", "_").upper()


def _property(path: str, interface: str, name: str) -> Any:
    Gio, GLib = _gi()
    props = _proxy(path, "org.freedesktop.DBus.Properties")
    value = props.call_sync(
        "Get",
        GLib.Variant("(ss)", (interface, name)),
        Gio.DBusCallFlags.NONE,
        3000,
        None,
    ).unpack()[0]
    return value


def _characteristic(mac: str):
    Gio, _GLib = _gi()
    root = _device_path(mac)
    manager = _proxy("/", "org.freedesktop.DBus.ObjectManager")
    objects = manager.call_sync("GetManagedObjects", None, Gio.DBusCallFlags.NONE, 4000, None).unpack()[0]
    for path, ifaces in objects.items():
        if not path.startswith(root):
            continue
        char = ifaces.get("org.bluez.GattCharacteristic1")
        if char is not None and str(char.get("UUID", "")).lower() == CHAR_UUID:
            return _proxy(path, "org.bluez.GattCharacteristic1")
    raise edgitalk.CompanionError("连上了板子，但没有找到状态特征")


def _stop_discovery() -> None:
    Gio, _GLib = _gi()
    try:
        adapter = _proxy(ADAPTER, "org.bluez.Adapter1")
        adapter.call_sync("StopDiscovery", None, Gio.DBusCallFlags.NONE, 2000, None)
    except Exception:
        pass


def _connect_error(error: Exception) -> edgitalk.CompanionError:
    text = str(error)
    if "le-connection-abort-by-local" in text or "In Progress" in text:
        return edgitalk.CompanionError("蓝牙连上后被断开了。Wi-Fi 还在，没有把蓝牙关掉。")
    return edgitalk.CompanionError(f"蓝牙连接失败：{text}")


def ensure_link(mac: str) -> None:
    """Open the BLE link. Scanning must already be stopped; a live scan aborts the connect."""
    if not adapter_present():
        raise edgitalk.CompanionError("这台电脑没有蓝牙")
    Gio, _GLib = _gi()
    _stop_discovery()
    path = _device_path(mac)
    device = _proxy(path, "org.bluez.Device1")
    try:
        if bool(_property(path, "org.bluez.Device1", "Connected")):
            return
    except Exception:
        pass
    try:
        device.call_sync("Connect", None, Gio.DBusCallFlags.NONE, 12000, None)
    except Exception as error:
        raise _connect_error(error) from None


def write_stats(mac: str, payload: dict[str, Any]) -> None:
    """Connect if needed and write one stats JSON object."""
    global _char_proxy, _char_mac
    if not adapter_present():
        raise edgitalk.CompanionError("这台电脑没有蓝牙")
    Gio, GLib = _gi()
    path = _device_path(mac)
    device = _proxy(path, "org.bluez.Device1")
    try:
        connected = bool(_property(path, "org.bluez.Device1", "Connected"))
    except Exception:
        connected = False
    if not connected:
        try:
            ensure_link(mac)
        except edgitalk.CompanionError:
            raise
        except Exception as error:
            raise _connect_error(error) from None
        _char_proxy = None
    # Service discovery runs after the link is up, so wait for it every time.
    deadline = time.monotonic() + 10.0
    while time.monotonic() < deadline:
        try:
            if bool(_property(path, "org.bluez.Device1", "ServicesResolved")):
                break
        except Exception:
            pass
        time.sleep(0.25)
    if _char_proxy is None or _char_mac != mac:
        _char_proxy = _characteristic(mac)
        _char_mac = mac
    body = json.dumps(payload, separators=(",", ":")).encode("utf-8")
    options = {"type": GLib.Variant("s", "request")}
    try:
        _char_proxy.call_sync(
            "WriteValue",
            GLib.Variant("(aya{sv})", (body, options)),
            Gio.DBusCallFlags.NONE,
            8000,
            None,
        )
    except Exception as error:
        _char_proxy = None
        raise edgitalk.CompanionError(f"蓝牙发送失败：{error}") from None
