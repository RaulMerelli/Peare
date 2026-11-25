"""Run a large logical TIB-sector probe inside a bounded child process."""

import ctypes as C
import os
import sys


def limit_memory():
    limit = 768 * 1024 * 1024
    if os.name != "nt":
        import resource
        resource.setrlimit(resource.RLIMIT_AS, (limit, limit))
        return None

    from ctypes import wintypes as W

    class BasicLimits(C.Structure):
        _fields_ = [
            ("process_time", C.c_int64), ("job_time", C.c_int64), ("flags", W.DWORD),
            ("working_set_min", C.c_size_t), ("working_set_max", C.c_size_t),
            ("process_count", W.DWORD), ("affinity", C.c_size_t),
            ("priority", W.DWORD), ("scheduling", W.DWORD),
        ]

    class IoCounters(C.Structure):
        _fields_ = [(name, C.c_uint64) for name in (
            "read_count", "write_count", "other_count", "read_bytes", "write_bytes", "other_bytes")]

    class ExtendedLimits(C.Structure):
        _fields_ = [
            ("basic", BasicLimits), ("io", IoCounters), ("process_memory", C.c_size_t),
            ("job_memory", C.c_size_t), ("peak_process_memory", C.c_size_t),
            ("peak_job_memory", C.c_size_t),
        ]

    kernel = C.WinDLL("kernel32", use_last_error=True)
    kernel.CreateJobObjectW.argtypes = (C.c_void_p, W.LPCWSTR)
    kernel.CreateJobObjectW.restype = W.HANDLE
    kernel.SetInformationJobObject.argtypes = (W.HANDLE, C.c_int, C.c_void_p, W.DWORD)
    kernel.SetInformationJobObject.restype = W.BOOL
    kernel.AssignProcessToJobObject.argtypes = (W.HANDLE, W.HANDLE)
    kernel.AssignProcessToJobObject.restype = W.BOOL
    kernel.GetCurrentProcess.restype = W.HANDLE
    job = kernel.CreateJobObjectW(None, None)
    if not job:
        raise C.WinError(C.get_last_error())
    info = ExtendedLimits()
    info.basic.flags = 256  # JOB_OBJECT_LIMIT_PROCESS_MEMORY
    info.process_memory = limit
    if not kernel.SetInformationJobObject(job, 9, C.byref(info), C.sizeof(info)):
        raise C.WinError(C.get_last_error())
    if not kernel.AssignProcessToJobObject(job, kernel.GetCurrentProcess()):
        raise C.WinError(C.get_last_error())
    return job


def main():
    job = limit_memory()
    from peare_opener import _HANDLE, load_library

    library, archive, mode = sys.argv[1:]
    peare = load_library(library)
    with peare.open_file(archive) as opener:
        resource = _HANDLE()
        assert opener._L.peare_opener_open_resource_at(opener._h, 0, 0, C.byref(resource)) == 0
        try:
            if mode in ("sparse", "inline_sparse"):
                cluster = 256 * 512
                assert peare.read_resource(resource, 0, 16) == b"Q" * 16
                assert peare.read_resource(resource, cluster - 8, 16) == b"Q" * 8 + bytes(8)
                assert peare.read_resource(resource, 2 * cluster, 16) == bytes(16)
                assert peare.read_resource(resource, 16382 * cluster - 8, 16) == bytes(8) + b"R" * 8
                assert peare.read_resource(resource, 16382 * cluster, 16) == b"R" * 16
                assert peare.read_resource(resource, 16383 * cluster - 8, 16) == b"R" * 8
            else:
                for _ in range(2):
                    output = (C.c_uint8 * 1)(0x5a)
                    count = C.c_size_t(99)
                    status = opener._L.peare_resource_read(resource, 0, output, 1, C.byref(count))
                    assert status != 0 and count.value == 0 and bytes(output) == b"\x5a"
        finally:
            opener._L.peare_resource_destroy(resource)
    return job


if __name__ == "__main__":
    main()
