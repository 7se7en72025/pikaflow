"""Lifetime-owned Windows test workers; no third-party Python packages."""
from __future__ import annotations

import os
import threading

if os.name == "nt":
    import ctypes
    from ctypes import wintypes

    class BasicLimits(ctypes.Structure):
        _fields_ = [("process_time", ctypes.c_longlong), ("job_time", ctypes.c_longlong),
            ("flags", wintypes.DWORD), ("minimum_working_set", ctypes.c_size_t),
            ("maximum_working_set", ctypes.c_size_t), ("active_processes", wintypes.DWORD),
            ("affinity", ctypes.c_size_t), ("priority", wintypes.DWORD), ("scheduling", wintypes.DWORD)]

    class IOCounters(ctypes.Structure):
        _fields_ = [(name, ctypes.c_ulonglong) for name in
            ("read_operations", "write_operations", "other_operations", "read_bytes", "write_bytes", "other_bytes")]

    class ExtendedLimits(ctypes.Structure):
        _fields_ = [("basic", BasicLimits), ("io", IOCounters),
            ("process_memory", ctypes.c_size_t), ("job_memory", ctypes.c_size_t),
            ("peak_process_memory", ctypes.c_size_t), ("peak_job_memory", ctypes.c_size_t)]

    class ThreadEntry(ctypes.Structure):
        _fields_ = [("size", wintypes.DWORD), ("usage", wintypes.DWORD),
            ("thread_id", wintypes.DWORD), ("process_id", wintypes.DWORD),
            ("base_priority", wintypes.LONG), ("delta_priority", wintypes.LONG), ("flags", wintypes.DWORD)]

    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.CreateJobObjectW.argtypes = [ctypes.c_void_p, wintypes.LPCWSTR]
    kernel.CreateJobObjectW.restype = wintypes.HANDLE
    kernel.SetInformationJobObject.argtypes = [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD]
    kernel.SetInformationJobObject.restype = wintypes.BOOL
    kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel.OpenProcess.restype = wintypes.HANDLE
    kernel.AssignProcessToJobObject.argtypes = [wintypes.HANDLE, wintypes.HANDLE]
    kernel.AssignProcessToJobObject.restype = wintypes.BOOL
    kernel.TerminateJobObject.argtypes = [wintypes.HANDLE, wintypes.UINT]
    kernel.TerminateJobObject.restype = wintypes.BOOL
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel.CloseHandle.restype = wintypes.BOOL
    kernel.CreateToolhelp32Snapshot.argtypes = [wintypes.DWORD, wintypes.DWORD]
    kernel.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
    kernel.Thread32First.argtypes = [wintypes.HANDLE, ctypes.POINTER(ThreadEntry)]
    kernel.Thread32First.restype = wintypes.BOOL
    kernel.Thread32Next.argtypes = [wintypes.HANDLE, ctypes.POINTER(ThreadEntry)]
    kernel.Thread32Next.restype = wintypes.BOOL
    kernel.OpenThread.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel.OpenThread.restype = wintypes.HANDLE
    kernel.ResumeThread.argtypes = [wintypes.HANDLE]
    kernel.ResumeThread.restype = wintypes.DWORD

    def resume_worker(pid):
        snapshot = kernel.CreateToolhelp32Snapshot(4, 0)  # TH32CS_SNAPTHREAD
        if snapshot == ctypes.c_void_p(-1).value:
            raise ctypes.WinError(ctypes.get_last_error())
        resumed = False
        try:
            entry = ThreadEntry()
            entry.size = ctypes.sizeof(entry)
            found = kernel.Thread32First(snapshot, ctypes.byref(entry))
            while found:
                if entry.process_id == pid:
                    thread = kernel.OpenThread(2, False, entry.thread_id)  # THREAD_SUSPEND_RESUME
                    if not thread:
                        raise ctypes.WinError(ctypes.get_last_error())
                    try:
                        count = kernel.ResumeThread(thread)
                        if count == 0xFFFFFFFF:
                            raise ctypes.WinError(ctypes.get_last_error())
                        resumed |= count > 0
                    finally:
                        kernel.CloseHandle(thread)
                entry.size = ctypes.sizeof(entry)
                found = kernel.Thread32Next(snapshot, ctypes.byref(entry))
            if not resumed:
                raise OSError("could not resume the suspended test worker")
        finally:
            kernel.CloseHandle(snapshot)


class ProcessJob:
    def __init__(self):
        self.handle = None
        self.lock = threading.Lock()
        if os.name == "nt":
            self.handle = kernel.CreateJobObjectW(None, None)
            if not self.handle:
                raise ctypes.WinError(ctypes.get_last_error())
            limits = ExtendedLimits()
            limits.basic.flags = 0x2000  # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
            if not kernel.SetInformationJobObject(self.handle, 9, ctypes.byref(limits), ctypes.sizeof(limits)):
                error = ctypes.WinError(ctypes.get_last_error())
                self.close()
                raise error

    @property
    def creation_flags(self):
        return 4 if os.name == "nt" else 0  # CREATE_SUSPENDED, assign before user code can run.

    def start(self, process):
        if os.name != "nt":
            return
        handle = kernel.OpenProcess(0x0100 | 0x0001, False, process.pid)  # SET_QUOTA | TERMINATE
        if not handle:
            raise ctypes.WinError(ctypes.get_last_error())
        try:
            if not kernel.AssignProcessToJobObject(self.handle, handle):
                raise ctypes.WinError(ctypes.get_last_error())
        finally:
            kernel.CloseHandle(handle)
        resume_worker(process.pid)

    def terminate(self):
        with self.lock:
            if self.handle and not kernel.TerminateJobObject(self.handle, 1):
                raise ctypes.WinError(ctypes.get_last_error())

    def close(self):
        with self.lock:
            if self.handle:
                kernel.CloseHandle(self.handle)
                self.handle = None
