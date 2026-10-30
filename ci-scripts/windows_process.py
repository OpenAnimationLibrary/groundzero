"""Windows CI process containment established before the first instruction.

The runner owns the job handle; the application and converter children never
inherit it. PID-based discovery after a crash cannot provide this guarantee.
Windows 10/Server 2016 or newer is required by JOB_LIST (all Windows CI hosts).
"""
import ctypes
import math
import os
import subprocess
import time


DWORD = ctypes.c_uint32
BOOL = ctypes.c_int32
HANDLE = ctypes.c_void_p
SIZE_T = ctypes.c_size_t
LPVOID = ctypes.c_void_p


class _StartupInfo(ctypes.Structure):
    _fields_ = [("cb", DWORD), ("reserved", ctypes.c_wchar_p),
                ("desktop", ctypes.c_wchar_p), ("title", ctypes.c_wchar_p),
                ("x", DWORD), ("y", DWORD), ("x_size", DWORD), ("y_size", DWORD),
                ("x_chars", DWORD), ("y_chars", DWORD), ("fill", DWORD),
                ("flags", DWORD), ("show", ctypes.c_uint16),
                ("reserved_size", ctypes.c_uint16), ("reserved_bytes", LPVOID),
                ("stdin", HANDLE), ("stdout", HANDLE), ("stderr", HANDLE)]


class _StartupInfoEx(ctypes.Structure):
    _fields_ = [("startup", _StartupInfo), ("attributes", LPVOID)]


class _ProcessInfo(ctypes.Structure):
    _fields_ = [("process", HANDLE), ("thread", HANDLE),
                ("pid", DWORD), ("tid", DWORD)]


class _BasicLimits(ctypes.Structure):
    _fields_ = [("process_time", ctypes.c_int64), ("job_time", ctypes.c_int64),
                ("flags", DWORD), ("min_working_set", SIZE_T),
                ("max_working_set", SIZE_T), ("active_limit", DWORD),
                ("affinity", SIZE_T), ("priority", DWORD), ("scheduling", DWORD)]


class _ExtendedLimits(ctypes.Structure):
    _fields_ = [("basic", _BasicLimits), ("io", ctypes.c_uint64 * 6),
                ("process_memory", SIZE_T), ("job_memory", SIZE_T),
                ("peak_process_memory", SIZE_T), ("peak_job_memory", SIZE_T)]


class _Accounting(ctypes.Structure):
    _fields_ = [("times", ctypes.c_int64 * 4), ("page_faults", DWORD),
                ("total", DWORD), ("active", DWORD), ("terminated", DWORD)]


class _Win32:
    def __init__(self):
        if os.name != "nt":
            raise OSError("Windows job containment requires Windows")
        self.kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        declarations = {
            "CreateJobObjectW": ([LPVOID, ctypes.c_wchar_p], HANDLE),
            "SetInformationJobObject": ([HANDLE, ctypes.c_int, LPVOID, DWORD], BOOL),
            "QueryInformationJobObject": ([HANDLE, ctypes.c_int, LPVOID, DWORD, LPVOID], BOOL),
            "TerminateJobObject": ([HANDLE, DWORD], BOOL),
            "CloseHandle": ([HANDLE], BOOL),
            "GetCurrentProcess": ([], HANDLE),
            "DuplicateHandle": ([HANDLE, HANDLE, HANDLE, ctypes.POINTER(HANDLE), DWORD, BOOL, DWORD], BOOL),
            "InitializeProcThreadAttributeList": ([LPVOID, DWORD, DWORD, ctypes.POINTER(SIZE_T)], BOOL),
            "UpdateProcThreadAttribute": ([LPVOID, DWORD, SIZE_T, LPVOID, SIZE_T, LPVOID, LPVOID], BOOL),
            "DeleteProcThreadAttributeList": ([LPVOID], None),
            "CreateProcessW": ([ctypes.c_wchar_p, ctypes.c_wchar_p, LPVOID, LPVOID, BOOL,
                                DWORD, LPVOID, ctypes.c_wchar_p,
                                ctypes.POINTER(_StartupInfo), ctypes.POINTER(_ProcessInfo)], BOOL),
            "WaitForSingleObject": ([HANDLE, DWORD], DWORD),
            "GetExitCodeProcess": ([HANDLE, ctypes.POINTER(DWORD)], BOOL),
        }
        for name, (arguments, result) in declarations.items():
            function = getattr(self.kernel, name)
            function.argtypes, function.restype = arguments, result

    @staticmethod
    def check(result):
        if not result:
            raise ctypes.WinError(ctypes.get_last_error())
        return result

    def close(self, handle):
        if handle:
            self.kernel.CloseHandle(handle)

    def _duplicate(self, handle):
        result = HANDLE()
        current = self.kernel.GetCurrentProcess()
        self.check(self.kernel.DuplicateHandle(current, handle, current,
                                               ctypes.byref(result), 0, True, 2))
        return result.value

    def launch(self, command, cwd, environment, destination):
        import msvcrt
        job, output_handle, input_handle = None, None, None
        initialized = False
        attributes = None
        info = _ProcessInfo()
        try:
            job = self.check(self.kernel.CreateJobObjectW(None, None))
            limits = _ExtendedLimits()
            limits.basic.flags = 0x2000  # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
            self.check(self.kernel.SetInformationJobObject(job, 9, ctypes.byref(limits),
                                                          ctypes.sizeof(limits)))
            output_handle = self._duplicate(msvcrt.get_osfhandle(destination.fileno()))
            with open(os.devnull, "rb") as null_input:
                input_handle = self._duplicate(msvcrt.get_osfhandle(null_input.fileno()))
            handles = (HANDLE * 2)(input_handle, output_handle)
            jobs = (HANDLE * 1)(job)
            size = SIZE_T()
            ctypes.set_last_error(0)
            self.kernel.InitializeProcThreadAttributeList(None, 2, 0, ctypes.byref(size))
            if ctypes.get_last_error() != 122 or not size.value:  # ERROR_INSUFFICIENT_BUFFER
                raise ctypes.WinError(ctypes.get_last_error())
            attributes = ctypes.create_string_buffer(size.value)
            self.check(self.kernel.InitializeProcThreadAttributeList(attributes, 2, 0,
                                                                     ctypes.byref(size)))
            initialized = True
            # Restrict inheritance to the two standard handles. In particular,
            # never inherit the job handle, which would defeat kill-on-close.
            self.check(self.kernel.UpdateProcThreadAttribute(attributes, 0, 0x20002,
                handles, ctypes.sizeof(handles), None, None))  # HANDLE_LIST
            self.check(self.kernel.UpdateProcThreadAttribute(attributes, 0, 0x2000D,
                jobs, ctypes.sizeof(jobs), None, None))  # JOB_LIST
            startup = _StartupInfoEx()
            startup.startup.cb = ctypes.sizeof(startup)
            startup.startup.flags = 0x100  # STARTF_USESTDHANDLES
            startup.startup.stdin = input_handle
            startup.startup.stdout = startup.startup.stderr = output_handle
            startup.attributes = ctypes.cast(attributes, LPVOID)
            command_line = ctypes.create_unicode_buffer(subprocess.list2cmdline(command))
            entries = []
            for key, value in sorted(environment.items(), key=lambda item: item[0].upper()):
                if "\0" in key or "\0" in value or "=" in key[1:] or not key:
                    raise ValueError("Invalid Windows process environment entry")
                entries.append(key + "=" + value)
            environment_block = ctypes.create_unicode_buffer("\0".join(entries) + "\0\0")
            self.check(self.kernel.CreateProcessW(command[0], command_line, None, None, True,
                0x80000 | 0x400, environment_block, str(cwd), ctypes.byref(startup.startup),
                ctypes.byref(info)))  # EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT
            result = job, info.process, int(info.pid)
            job, info.process = None, None  # Ownership transfers only on success.
            return result
        finally:
            self.close(info.thread)
            self.close(info.process)
            if initialized:
                self.kernel.DeleteProcThreadAttributeList(attributes)
            self.close(input_handle)
            self.close(output_handle)
            self.close(job)

    def wait(self, process, timeout):
        milliseconds = 0xFFFFFFFF if timeout is None else min(0xFFFFFFFE, max(0, math.ceil(timeout * 1000)))
        result = self.kernel.WaitForSingleObject(process, milliseconds)
        if result == 258:  # WAIT_TIMEOUT
            return None
        if result != 0:
            raise ctypes.WinError(ctypes.get_last_error())
        code = DWORD()
        self.check(self.kernel.GetExitCodeProcess(process, ctypes.byref(code)))
        return int(code.value)

    def active(self, job):
        accounting = _Accounting()
        self.check(self.kernel.QueryInformationJobObject(job, 1, ctypes.byref(accounting),
                                                        ctypes.sizeof(accounting), None))
        return int(accounting.active)

    def terminate(self, job):
        self.check(self.kernel.TerminateJobObject(job, 1))


class WindowsJobProcess:
    """Small Popen-compatible wait boundary with explicit tree retirement."""
    def __init__(self, command, *, cwd, env, stdout, _api=None):
        self.args = list(map(os.fspath, command))
        self._api = _api if _api is not None else _Win32()
        self._job, self._process, self.pid = self._api.launch(self.args, cwd, env, stdout)
        self.returncode = None

    def wait(self, timeout=None):
        if self.returncode is None:
            result = self._api.wait(self._process, timeout)
            if result is None:
                raise subprocess.TimeoutExpired(self.args, timeout)
            self.returncode = result
        return self.returncode

    def poll(self):
        try:
            return self.wait(0)
        except subprocess.TimeoutExpired:
            return None

    def retire(self, timeout=15):
        """Prove all job members exited, including descendants of a dead root."""
        deadline = time.monotonic() + timeout
        notes = b""
        try:
            active = self._api.active(self._job)
            if active:
                notes += f"Windows job cleanup: terminating {active} remaining process(es).\n".encode()
                self._api.terminate(self._job)
            self.wait(max(0, deadline - time.monotonic()))
            # Preserve the root's uint32 status before releasing our reference.
            # Job accounting may retain a terminated member until references
            # are released; do not hold our root handle while proving zero.
            self._api.close(self._process)
            self._process = None
            while self._api.active(self._job):
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    return False, notes + b"Windows job did not reach zero active processes within 15 seconds.\n"
                time.sleep(min(0.05, remaining))
            return True, notes + b"Windows job cleanup confirmed zero active processes.\n"
        except (OSError, subprocess.TimeoutExpired) as error:
            return False, notes + (f"Windows job cleanup could not confirm exit: {type(error).__name__}: {error}\n").encode(
                "utf-8", errors="backslashreplace")

    def close(self):
        # Closing an uncertain job requests kill-on-close, but is NOT proof of
        # retirement; the caller must retain resources when retire() failed.
        if self._process:
            self._api.close(self._process)
            self._process = None
        if self._job:
            self._api.close(self._job)
            self._job = None
