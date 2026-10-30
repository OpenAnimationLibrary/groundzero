"""Scoped WER capture and bounded postmortem of the original Windows process.

Only per-application LocalDumps values are temporarily changed. Raw dumps never
enter test-report artifacts; the text report records exact executable/PDB hashes.
"""
import hashlib
import ctypes
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def _sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _wer_service_diagnostic():
    """Observe the service only; never start it or change its configuration."""
    manager = service = None
    api = None
    try:
        api = ctypes.WinDLL("advapi32", use_last_error=True)
        declarations = {
            "OpenSCManagerW": ([ctypes.c_wchar_p, ctypes.c_wchar_p, ctypes.c_uint32], ctypes.c_void_p),
            "OpenServiceW": ([ctypes.c_void_p, ctypes.c_wchar_p, ctypes.c_uint32], ctypes.c_void_p),
            "QueryServiceStatusEx": ([ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p,
                                      ctypes.c_uint32, ctypes.POINTER(ctypes.c_uint32)], ctypes.c_int32),
            "CloseServiceHandle": ([ctypes.c_void_p], ctypes.c_int32),
        }
        for name, (arguments, result) in declarations.items():
            function = getattr(api, name)
            function.argtypes, function.restype = arguments, result
        manager = api.OpenSCManagerW(None, None, 1)  # SC_MANAGER_CONNECT
        if not manager:
            raise ctypes.WinError(ctypes.get_last_error())
        service = api.OpenServiceW(manager, "WerSvc", 4)  # SERVICE_QUERY_STATUS
        if not service:
            raise ctypes.WinError(ctypes.get_last_error())
        status = (ctypes.c_uint32 * 9)()  # SERVICE_STATUS_PROCESS
        required = ctypes.c_uint32()
        if not api.QueryServiceStatusEx(service, 0, status, ctypes.sizeof(status), ctypes.byref(required)):
            raise ctypes.WinError(ctypes.get_last_error())
        return f"WerSvc observed state={status[1]} win32_exit={status[3]} service_exit={status[4]}"
    except (AttributeError, OSError) as error:
        return f"WerSvc state unavailable ({type(error).__name__}, winerror={getattr(error, 'winerror', None)})"
    finally:
        if api is not None:
            if service:
                api.CloseServiceHandle(service)
            if manager:
                api.CloseServiceHandle(manager)


def windows_dump_prerequisites(executable_name, registry):
    """Bounded scalar witnesses, without private paths or policy mutations."""
    wer = r"SOFTWARE\Microsoft\Windows\Windows Error Reporting"
    debugger = r"SOFTWARE\Microsoft\Windows NT\CurrentVersion\AeDebug"
    entries = [
        ("AeDebug Auto", debugger, "Auto"),
        ("AeDebug application exclusion", debugger + r"\AutoExclusionList", executable_name),
        ("WerSvc start mode", r"SYSTEM\CurrentControlSet\Services\WerSvc", "Start"),
        ("WER Disabled (not by itself a LocalDumps veto)", wer, "Disabled"),
        ("WER policy Disabled", r"SOFTWARE\Policies\Microsoft\Windows\Windows Error Reporting", "Disabled"),
        ("LocalDumps application DumpType", wer + "\\LocalDumps\\" + executable_name, "DumpType"),
        ("LocalDumps application DumpCount", wer + "\\LocalDumps\\" + executable_name, "DumpCount"),
    ]
    lines = ["Read-only original-dump prerequisite witnesses:"]
    for label, path, name in entries:
        try:
            access = registry.KEY_QUERY_VALUE | registry.KEY_WOW64_64KEY
            with registry.OpenKey(registry.HKEY_LOCAL_MACHINE, path, 0, access) as key:
                value, kind = registry.QueryValueEx(key, name)
            # Never print arbitrary registry strings (notably debugger paths).
            scalar = str(value) if isinstance(value, int) and 0 <= value <= 0xFFFFFFFF else (
                value if isinstance(value, str) and value in ("0", "1") else "non-scalar")
            lines.append(f"{label}: {scalar} (registry type {kind})")
        except FileNotFoundError:
            lines.append(f"{label}: absent")
        except OSError as error:
            lines.append(f"{label}: unavailable ({type(error).__name__}, winerror={getattr(error, 'winerror', None)})")
    lines.append(_wer_service_diagnostic())
    return "\n".join(lines) + "\n"


class OriginalWindowsDump:
    def __init__(self, executable, directory, *, registry=None, runtime_state=None):
        self.executable = Path(executable).resolve(strict=True)
        self.root = Path(directory).resolve()
        self.registry = registry
        self.runtime_state = runtime_state
        self.folder = None
        self.enabled = False
        self.restore_failed = False
        self.safe_to_release = True
        self.notes = []
        self._key = None
        self._created = False
        self._saved = {}
        self._registry_path = ("SOFTWARE\\Microsoft\\Windows\\Windows Error Reporting\\LocalDumps\\" +
                               self.executable.name)

    def __enter__(self):
        try:
            if self.registry is None:
                import winreg
                self.registry = winreg
            registry = self.registry
            self.root.mkdir(parents=True, exist_ok=True)
            self.folder = Path(tempfile.mkdtemp(prefix="original-windows-", dir=self.root))
            access = registry.KEY_QUERY_VALUE | registry.KEY_SET_VALUE | registry.KEY_WOW64_64KEY
            try:
                self._key = registry.OpenKey(registry.HKEY_LOCAL_MACHINE, self._registry_path, 0, access)
            except FileNotFoundError:
                self._key = registry.CreateKeyEx(registry.HKEY_LOCAL_MACHINE, self._registry_path, 0, access)
                self._created = True
            # Snapshot all affected values before the first mutation, including
            # types. Never rewrite/delete unrelated values or global WER keys.
            for name in ("DumpFolder", "DumpType", "DumpCount"):
                try:
                    self._saved[name] = registry.QueryValueEx(self._key, name)
                except FileNotFoundError:
                    self._saved[name] = None
            registry.SetValueEx(self._key, "DumpFolder", 0, registry.REG_EXPAND_SZ, str(self.folder))
            registry.SetValueEx(self._key, "DumpType", 0, registry.REG_DWORD, 1)
            registry.SetValueEx(self._key, "DumpCount", 0, registry.REG_DWORD, 1)
            self.enabled = True
        except (OSError, ValueError, ImportError) as error:
            self.notes.append(f"Original Windows dump capture unavailable: {type(error).__name__}: {error}")
        except BaseException as error:
            # A context manager whose __enter__ is interrupted never receives
            # __exit__. Restore any already-written per-application values now.
            self.__exit__(type(error), error, error.__traceback__)
            raise
        return self

    def __exit__(self, exception_type, exception, traceback):
        if self.runtime_state is not None and not self.runtime_state["safe_to_release"]:
            self.safe_to_release = False
        registry = self.registry
        if self._key is not None:
            for name, previous in self._saved.items():
                try:
                    if previous is None:
                        try:
                            registry.DeleteValue(self._key, name)
                        except FileNotFoundError:
                            pass
                    else:
                        value, kind = previous
                        registry.SetValueEx(self._key, name, 0, kind, value)
                except OSError as error:
                    self.restore_failed = True
                    self.notes.append(f"Original Windows dump setting restoration failed for {name}: {error}")
            try:
                registry.CloseKey(self._key)
            except OSError as error:
                self.restore_failed = True
                self.notes.append(f"Original Windows dump key close failed: {error}")
            self._key = None
            if self._created:
                try:
                    # DeleteKeyEx refuses a key with subkeys. Also explicitly
                    # retain a key if an unrelated value appeared concurrently.
                    access = registry.KEY_QUERY_VALUE | registry.KEY_WOW64_64KEY
                    with registry.OpenKey(registry.HKEY_LOCAL_MACHINE, self._registry_path, 0, access) as key:
                        subkeys, values, _ = registry.QueryInfoKey(key)
                    if not subkeys and not values:
                        registry.DeleteKeyEx(registry.HKEY_LOCAL_MACHINE, self._registry_path,
                                             registry.KEY_WOW64_64KEY, 0)
                except FileNotFoundError:
                    pass
                except OSError as error:
                    self.restore_failed = True
                    self.notes.append(f"Original Windows dump key restoration failed: {error}")
        if self.folder is not None and not self.safe_to_release:
            self.notes.append("Original raw dump retained outside report artifacts: process-tree exit was not confirmed.")
        elif self.folder is not None:
            try:
                shutil.rmtree(self.folder)
            except OSError as error:
                self.notes.append(f"Original raw Windows dump cleanup failed; excluded from report artifacts: {error}")
        return False

    def collect(self, pid, started_at, finished_at, symbol_directories, capture):
        """Read one exact-process dump; never launch the application again."""
        if not self.enabled:
            return "Original Windows dump was not enabled; no original stack is available.\n"
        if not isinstance(pid, int) or pid <= 0 or started_at is None or finished_at is None:
            return "Original Windows dump identity is unavailable; no dump was selected.\n"
        # WER writes <image>.<pid>.dmp before allowing the crashed process to
        # terminate. A private fresh directory and exact name exclude stale or
        # unrelated-process dumps; time and file checks provide another bound.
        expected = f"{self.executable.name}.{pid}.dmp".casefold()
        candidates = [path for path in self.folder.iterdir()
                      if path.name.casefold() == expected and not path.is_symlink() and path.is_file()]
        if len(candidates) != 1:
            return (f"Original Windows dump unavailable for {self.executable.name}, PID {pid}; "
                    "WER/custom crash handling may not have produced a dump. No replay was substituted.\n" +
                    windows_dump_prerequisites(self.executable.name, self.registry))
        dump = candidates[0]
        metadata = dump.stat()
        if not (32 <= metadata.st_size <= 64 * 1024 * 1024 and
                started_at - 2 <= metadata.st_mtime <= finished_at + 2):
            return "Original Windows dump failed size/time identity bounds; no postmortem was attempted.\n"
        with dump.open("rb") as stream:
            if stream.read(4) != b"MDMP":
                return "Original Windows dump has an invalid minidump signature.\n"
        kits = os.environ.get("ProgramFiles(x86)")
        debugger = Path(kits) / "Windows Kits/10/Debuggers/x64/cdb.exe" if kits else None
        if debugger is None or not debugger.is_file():
            return "Original Windows dump captured, but Windows SDK CDB is unavailable; no stack was decoded.\n"
        identity = [f"Original Windows process: {self.executable.name}, PID {pid}",
                    f"Executable SHA256: {_sha256(self.executable)}"]
        symbols = [Path(path).resolve() for path in symbol_directories if Path(path).is_dir()]
        symbols.append(self.executable.parent)
        seen = set()
        for directory in symbols:
            pdb = directory / (self.executable.stem + ".pdb")
            if pdb.is_file() and pdb.resolve() not in seen:
                seen.add(pdb.resolve())
                identity.append(f"PDB {pdb.name} SHA256: {_sha256(pdb)}")
        module = self.executable.stem
        if not all(character.isalnum() or character in "_-" for character in module):
            return "Original executable name cannot be used safely in debugger commands.\n"
        commands = (f".echo Original Windows process dump (no replay); .lastevent; "
                    f"lmvm {module}; !lmi {module}; .exr -1; .ecxr; ~* kb 12; kv 60; q")
        command = [str(debugger), "-z", str(dump), "-lines", "-y", ";".join(map(str, symbols)),
                   "-c", commands]
        state = {"safe_to_release": True}
        environment = os.environ.copy()
        for name in ("_NT_SYMBOL_PATH", "_NT_ALT_SYMBOL_PATH"):
            environment.pop(name, None)
        try:
            result = capture(command, cwd=self.folder, environment=environment, timeout=120,
                             runtime_state=state)
            raw = result.stdout or b""
            status = f"Original dump CDB exited with status {result.returncode}."
        except subprocess.TimeoutExpired as error:
            raw = error.output or state.get("output", b"")
            status = "Original dump CDB timed out after 120 seconds."
        except (OSError, RuntimeError, ValueError) as error:
            raw = state.get("output", b"")
            status = f"Original dump CDB could not complete: {type(error).__name__}: {error}"
        finally:
            self.safe_to_release = state["safe_to_release"]
            if not self.safe_to_release and self.runtime_state is not None:
                self.runtime_state["safe_to_release"] = False
        if isinstance(raw, str):
            raw = raw.encode("utf-8", errors="backslashreplace")
        if len(raw) > 128 * 1024:
            raw = raw[:64 * 1024] + b"\nOriginal dump text truncated.\n" + raw[-64 * 1024:]
        return ("\n".join(identity) + "\n" + raw.decode("utf-8", errors="replace") + "\n" + status +
                "\nThe original native process status remains authoritative.\n")
