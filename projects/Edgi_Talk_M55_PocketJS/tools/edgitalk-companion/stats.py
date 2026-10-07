"""PC statistics for the Edgi Talk monitor page.

`psutil` is used when it is installed. Without it the module falls back to the standard library:
/proc and /sys on Linux, the Win32 API through ctypes on Windows. Anything that cannot be read is
reported as unavailable instead of raising, so the sender keeps running on any machine.
"""

from __future__ import annotations

import glob
import os
import platform
import shutil
import socket
import sys
import time
from typing import Any

try:  # optional
    import psutil  # type: ignore
except ImportError:  # pragma: no cover - depends on the environment
    psutil = None

# Sensor names that describe the CPU package, best first.
_CPU_SENSORS = ("coretemp", "k10temp", "zenpower", "cpu_thermal", "cpu-thermal", "x86_pkg_temp", "acpitz")


def _read(path: str) -> str:
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        return handle.read()


# -- CPU ---------------------------------------------------------------------------------


def _linux_cpu_times() -> tuple[int, int]:
    """(busy, total) jiffies from the aggregate line of /proc/stat."""
    fields = [int(v) for v in _read("/proc/stat").splitlines()[0].split()[1:9]]
    idle = fields[3] + (fields[4] if len(fields) > 4 else 0)
    total = sum(fields)
    return total - idle, total


def _windows_cpu_times() -> tuple[int, int]:
    import ctypes
    from ctypes import wintypes

    idle, kernel, user = wintypes.FILETIME(), wintypes.FILETIME(), wintypes.FILETIME()
    if not ctypes.windll.kernel32.GetSystemTimes(ctypes.byref(idle), ctypes.byref(kernel), ctypes.byref(user)):
        raise OSError("GetSystemTimes failed")

    def value(ft: Any) -> int:
        return (ft.dwHighDateTime << 32) | ft.dwLowDateTime

    total = value(kernel) + value(user)  # kernel time already includes idle time
    return total - value(idle), total


class Sampler:
    """Reads CPU load as the change since the previous call, so call it on a steady interval."""

    def __init__(self) -> None:
        self._last: tuple[int, int] | None = None
        if psutil is not None:
            psutil.cpu_percent(None)  # prime the counter

    def cpu_percent(self) -> int:
        if psutil is not None:
            return int(round(psutil.cpu_percent(None)))
        try:
            now = _windows_cpu_times() if sys.platform == "win32" else _linux_cpu_times()
        except (OSError, ValueError, IndexError):
            return 0
        last, self._last = self._last, now
        if last is None or now[1] <= last[1]:
            return 0
        return max(0, min(100, int(round(100.0 * (now[0] - last[0]) / (now[1] - last[1])))))


# -- memory, disk, temperature ----------------------------------------------------------------


def memory_mb() -> tuple[int, int]:
    """(used, total) in MiB. Used excludes reclaimable cache, like `free` does."""
    if psutil is not None:
        info = psutil.virtual_memory()
        return int((info.total - info.available) // (1 << 20)), int(info.total // (1 << 20))
    if sys.platform == "win32":
        import ctypes

        class Status(ctypes.Structure):
            _fields_ = [("length", ctypes.c_ulong), ("load", ctypes.c_ulong), ("total", ctypes.c_ulonglong),
                        ("avail", ctypes.c_ulonglong), ("pt", ctypes.c_ulonglong), ("pa", ctypes.c_ulonglong),
                        ("vt", ctypes.c_ulonglong), ("va", ctypes.c_ulonglong), ("ve", ctypes.c_ulonglong)]

        status = Status()
        status.length = ctypes.sizeof(Status)
        if ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(status)):
            return int((status.total - status.avail) >> 20), int(status.total >> 20)
        return 0, 0
    try:
        fields: dict[str, int] = {}
        for line in _read("/proc/meminfo").splitlines():
            name, _, rest = line.partition(":")
            fields[name] = int(rest.split()[0])  # kB
        total = fields["MemTotal"]
        available = fields.get("MemAvailable", fields.get("MemFree", 0))
        return (total - available) // 1024, total // 1024
    except (OSError, ValueError, KeyError, IndexError):
        return 0, 0


def disk_gb(path: str | None = None) -> tuple[int, int]:
    """(used, total) in GiB of the system drive."""
    if path is None:
        path = os.environ.get("SystemDrive", "C:") + "\\" if sys.platform == "win32" else "/"
    try:
        usage = shutil.disk_usage(path)
    except OSError:
        return 0, 0
    return int(usage.used // (1 << 30)), int(usage.total // (1 << 30))


def temperature_c() -> int | None:
    """CPU temperature in degrees C, or None when the OS does not expose one."""
    readings: dict[str, float] = {}
    if psutil is not None and hasattr(psutil, "sensors_temperatures"):
        try:
            for name, entries in psutil.sensors_temperatures().items():
                values = [e.current for e in entries if e.current]
                if values:
                    readings[name] = max(values)
        except (OSError, AttributeError):
            pass
    if not readings and sys.platform.startswith("linux"):
        for hwmon in glob.glob("/sys/class/hwmon/hwmon*"):
            try:
                name = _read(os.path.join(hwmon, "name")).strip()
                values = [int(_read(f)) / 1000.0 for f in glob.glob(os.path.join(hwmon, "temp*_input"))]
            except (OSError, ValueError):
                continue
            if values:
                readings[name] = max(values)
        for zone in glob.glob("/sys/class/thermal/thermal_zone*"):
            try:
                name = _read(os.path.join(zone, "type")).strip()
                readings.setdefault(name, int(_read(os.path.join(zone, "temp"))) / 1000.0)
            except (OSError, ValueError):
                continue
    if not readings:
        return None
    for name in _CPU_SENSORS:
        if name in readings and 0 < readings[name] < 150:
            return int(round(readings[name]))
    value = max(readings.values())
    return int(round(value)) if 0 < value < 150 else None


# -- payload -----------------------------------------------------------------------------------


def local_utc_offset_minutes() -> int:
    offset = -(time.altzone if time.localtime().tm_isdst > 0 and time.daylight else time.timezone)
    return int(offset // 60)


def _clean_host(name: str) -> str:
    """The board's fonts only hold printable ASCII and its host field is 16 characters."""
    text = "".join(ch if 32 <= ord(ch) < 127 and ch not in '"\\' else "?" for ch in name)
    return text[:16] or "PC"


def collect(sampler: Sampler, host: str | None = None, include_time: bool = True) -> dict[str, Any]:
    """One /api/pc payload. Values the board cannot show are sent as -1 (temperature) or 0."""
    used_mb, total_mb = memory_mb()
    used_gb, total_gb = disk_gb()
    temp = temperature_c()
    payload: dict[str, Any] = {
        "host": _clean_host(host or socket.gethostname() or platform.node()),
        "cpu": sampler.cpu_percent(),
        "ramPct": int(round(100.0 * used_mb / total_mb)) if total_mb else 0,
        "ramUsedMb": used_mb,
        "ramTotalMb": total_mb,
        "diskPct": int(round(100.0 * used_gb / total_gb)) if total_gb else 0,
        "diskUsedGb": used_gb,
        "diskTotalGb": total_gb,
        "temp": temp if temp is not None else -1,
    }
    if include_time:
        payload["t"] = int(time.time())
        payload["tz"] = local_utc_offset_minutes()
    return payload
