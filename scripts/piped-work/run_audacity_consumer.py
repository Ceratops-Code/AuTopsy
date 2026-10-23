"""Own stock-Audacity compatibility sessions and their process observations.

Pinned packages stay read-only. The marked case root owns generated projects,
private portable profiles, child processes and their bounded scratch outputs.
"""

import ctypes
import hashlib
import json
import os
import shutil
import subprocess
import time
from array import array
from datetime import datetime
from pathlib import Path

from prepare_audacity_consumer import PINNED_CONSUMERS, AudacityConsumerCache


class OwnedConsumerProcess:
    """Start suspended, attach a kill-on-close job, then allow the app to run.

    Direct Win32 extended startup atomically attaches the job while keeping the
    standalone CTest interpreter free of a binary-extension dependency.
    Windows owns descendant membership, including crash reporters. No unrelated
    PID is discovered or terminated. Closing our last job handle also covers a
    killed Python worker, without a saved PID list or a recovery log.
    """

    def __init__(self, command, cwd, environment, stdout, stderr):
        import msvcrt
        from ctypes import wintypes as w

        class Startup(ctypes.Structure):
            _fields_ = [
                ("cb", w.DWORD), ("reserved", w.LPWSTR), ("desktop", w.LPWSTR),
                ("title", w.LPWSTR), ("x", w.DWORD), ("y", w.DWORD),
                ("width", w.DWORD), ("height", w.DWORD), ("xchars", w.DWORD),
                ("ychars", w.DWORD), ("fill", w.DWORD), ("flags", w.DWORD),
                ("show", w.WORD), ("reserved_bytes", w.WORD),
                ("reserved_pointer", ctypes.c_void_p),
                ("stdin", w.HANDLE), ("stdout", w.HANDLE), ("stderr", w.HANDLE),
            ]

        class ProcessInfo(ctypes.Structure):
            _fields_ = [("process", w.HANDLE), ("thread", w.HANDLE),
                        ("pid", w.DWORD), ("thread_id", w.DWORD)]

        class ExtendedStartup(ctypes.Structure):
            _fields_ = [("startup", Startup), ("attributes", ctypes.c_void_p)]

        class BasicLimits(ctypes.Structure):
            _fields_ = [
                ("process_time", ctypes.c_int64), ("job_time", ctypes.c_int64),
                ("flags", w.DWORD), ("minimum_working_set", ctypes.c_size_t),
                ("maximum_working_set", ctypes.c_size_t), ("active_limit", w.DWORD),
                ("affinity", ctypes.c_size_t), ("priority", w.DWORD), ("scheduling", w.DWORD),
            ]

        class ExtendedLimits(ctypes.Structure):
            _fields_ = [("basic", BasicLimits), ("io", ctypes.c_uint64 * 6),
                        ("process_memory", ctypes.c_size_t), ("job_memory", ctypes.c_size_t),
                        ("peak_process_memory", ctypes.c_size_t), ("peak_job_memory", ctypes.c_size_t)]

        self.kernel = kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.CreateJobObjectW.argtypes = [ctypes.c_void_p, w.LPCWSTR]
        kernel.CreateJobObjectW.restype = w.HANDLE
        kernel.SetInformationJobObject.argtypes = [w.HANDLE, ctypes.c_int, ctypes.c_void_p, w.DWORD]
        kernel.InitializeProcThreadAttributeList.argtypes = [
            ctypes.c_void_p, w.DWORD, w.DWORD, ctypes.POINTER(ctypes.c_size_t)]
        kernel.UpdateProcThreadAttribute.argtypes = [
            ctypes.c_void_p, w.DWORD, ctypes.c_size_t, ctypes.c_void_p,
            ctypes.c_size_t, ctypes.c_void_p, ctypes.c_void_p]
        kernel.DeleteProcThreadAttributeList.argtypes = [ctypes.c_void_p]
        kernel.ResumeThread.argtypes = [w.HANDLE]
        kernel.ResumeThread.restype = w.DWORD
        kernel.TerminateProcess.argtypes = [w.HANDLE, w.UINT]
        kernel.TerminateJobObject.argtypes = [w.HANDLE, w.UINT]
        kernel.WaitForSingleObject.argtypes = [w.HANDLE, w.DWORD]
        kernel.GetExitCodeProcess.argtypes = [w.HANDLE, ctypes.POINTER(w.DWORD)]
        kernel.CloseHandle.argtypes = [w.HANDLE]
        kernel.QueryInformationJobObject.argtypes = [w.HANDLE, ctypes.c_int,
                                                     ctypes.c_void_p, w.DWORD, ctypes.c_void_p]
        kernel.CreateProcessW.argtypes = [
            w.LPCWSTR, w.LPWSTR, ctypes.c_void_p, ctypes.c_void_p,
            w.BOOL, w.DWORD, ctypes.c_void_p, w.LPCWSTR,
            ctypes.POINTER(Startup), ctypes.POINTER(ProcessInfo),
        ]
        self.job = kernel.CreateJobObjectW(None, None)
        self.process = None
        self.pid = None
        self.status = None
        if not self.job:
            raise ctypes.WinError(ctypes.get_last_error())
        info = ProcessInfo()
        attributes = None
        attributes_ready = False
        limits = ExtendedLimits()
        limits.basic.flags = 0x2000  # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
        try:
            if not kernel.SetInformationJobObject(self.job, 9, ctypes.byref(limits), ctypes.sizeof(limits)):
                raise ctypes.WinError(ctypes.get_last_error())
            extended = ExtendedStartup()
            startup = extended.startup
            startup.cb = ctypes.sizeof(extended)
            startup.flags = 0x101  # STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES
            startup.show = 4  # SW_SHOWNOACTIVATE: never borrow shared input.
            size = ctypes.c_size_t()
            kernel.InitializeProcThreadAttributeList(None, 1, 0, ctypes.byref(size))
            if not size.value:
                raise ctypes.WinError(ctypes.get_last_error())
            attributes = ctypes.create_string_buffer(size.value)
            if not kernel.InitializeProcThreadAttributeList(attributes, 1, 0, ctypes.byref(size)):
                raise ctypes.WinError(ctypes.get_last_error())
            attributes_ready = True
            job = w.HANDLE(self.job)
            # PROC_THREAD_ATTRIBUTE_JOB_LIST attaches ownership atomically with
            # process creation: killing this worker cannot strand a suspended app.
            if not kernel.UpdateProcThreadAttribute(
                attributes, 0, 0x2000D, ctypes.byref(job), ctypes.sizeof(job), None, None,
            ):
                raise ctypes.WinError(ctypes.get_last_error())
            extended.attributes = ctypes.addressof(attributes)
            block = ctypes.create_unicode_buffer(
                "\0".join(f"{key}={value}" for key, value in sorted(environment.items())) + "\0\0")
            with open(os.devnull, "rb") as stdin:
                streams = (stdin, stdout, stderr)
                handles = [msvcrt.get_osfhandle(stream.fileno()) for stream in streams]
                for handle in handles:
                    os.set_handle_inheritable(handle, True)
                startup.stdin, startup.stdout, startup.stderr = handles
                try:
                    # Suspended + Unicode environment + no console + extended startup.
                    if not kernel.CreateProcessW(
                        str(command[0]), ctypes.create_unicode_buffer(subprocess.list2cmdline(command)),
                        None, None, True, 0x08080404, block, str(cwd),
                        ctypes.byref(startup), ctypes.byref(info),
                    ):
                        raise ctypes.WinError(ctypes.get_last_error())
                finally:
                    for handle in handles:
                        os.set_handle_inheritable(handle, False)
            self.process, self.pid = info.process, info.pid
            if kernel.ResumeThread(info.thread) == 0xFFFFFFFF:
                raise ctypes.WinError(ctypes.get_last_error())
        except BaseException:
            if info.process and not self.process:
                self.process, self.pid = info.process, info.pid
            self.close()
            raise
        finally:
            if info.thread:
                kernel.CloseHandle(info.thread)
            if attributes_ready:
                kernel.DeleteProcThreadAttributeList(attributes)

    def poll(self):
        if not self.process:
            return self.status
        code = ctypes.c_uint32()
        if not self.kernel.GetExitCodeProcess(self.process, ctypes.byref(code)):
            raise ctypes.WinError(ctypes.get_last_error())
        return None if code.value == 259 else code.value

    def close(self):
        """Stop all owned descendants before copied files can be removed."""
        if not self.job:
            return
        try:
            if not self.kernel.TerminateJobObject(self.job, 1):
                raise ctypes.WinError(ctypes.get_last_error())
            # Job accounting can reach zero before process teardown releases
            # named pipes and loaded DLLs. Wait for the owned process object,
            # rather than treating accounting or a terminate request as exit.
            if self.process and self.kernel.WaitForSingleObject(self.process, 10000) != 0:
                raise RuntimeError("The owned consumer did not finish process teardown.")
            # Basic accounting ends with DWORD Total/Active/Terminated counts.
            accounting = ctypes.create_string_buffer(48)
            deadline = time.monotonic() + 10
            while True:
                if not self.kernel.QueryInformationJobObject(
                    self.job, 1, accounting, len(accounting), None,
                ):
                    raise ctypes.WinError(ctypes.get_last_error())
                active = ctypes.c_uint32.from_buffer(accounting, 40).value
                if not active:
                    break
                if time.monotonic() >= deadline:
                    raise RuntimeError("Owned consumer descendants did not exit within 10 seconds.")
                time.sleep(0.02)
            self.status = self.poll()
        finally:
            if self.process:
                self.kernel.CloseHandle(self.process)
                self.process = None
            self.kernel.CloseHandle(self.job)
            self.job = None


def prepare_consumer_profile(executable, configuration, environment, scratch):
    """Give each stopped consumer fresh preferences and empty private temp roots.

    Ending a stock process with an open copied project leaves its recovery list.
    Never carry that list into the next startup. Existing stock IPC is checked
    before this function, and the session job has ended before the next call.
    """
    if (scratch / "_owner").read_text() != "AuTopsy stock consumer scratch 1":
        raise RuntimeError("Consumer preferences require the owned scratch directory.")
    targets = [executable.parent / "Portable Settings",
               Path(environment["TEMP"]), Path(environment["TMP"]),
               scratch / "_temp"]
    targets = list(dict.fromkeys(targets))
    for path in targets:
        if path.is_symlink() or not path.resolve().is_relative_to(scratch.resolve()):
            raise RuntimeError("Consumer profile cleanup escapes its owned scratch directory.")
    profile = targets[0]
    for path in targets:
        wanted = [path / "audacity.cfg"] if path == profile else []
        if path.is_dir() and sorted(path.iterdir()) == wanted:
            if path != profile or wanted[0].read_text(encoding="utf-8") == configuration:
                continue
        if path.exists():
            shutil.rmtree(path)
        path.mkdir()
        if path == profile:
            (path / "audacity.cfg").write_text(configuration, encoding="utf-8")


class StockAudacitySession:
    """One file opened by normal startup, with PID-checked scripting only after load."""

    def __init__(self, executable, project, environment, display, scratch, configuration):
        self.command = [str(executable), str(project)]
        self.project = project
        self.environment, self.display, self.scratch = environment, display, scratch
        self.configuration = configuration
        self.app = None
        self.handles = []
        self.stdout = self.stderr = None
        self.kernel = None

    def __enter__(self):
        try:
            self.start()
            return self
        except BaseException as error:
            self.__exit__(type(error), error, error.__traceback__)

    def start(self):
        kernel = self.kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.WaitNamedPipeW.argtypes = [ctypes.c_wchar_p, ctypes.c_uint32]
        kernel.CreateFileW.argtypes = [ctypes.c_wchar_p, ctypes.c_uint32, ctypes.c_uint32,
                                      ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p]
        kernel.CreateFileW.restype = ctypes.c_void_p
        kernel.CloseHandle.argtypes = [ctypes.c_void_p]
        kernel.GetNamedPipeServerProcessId.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint32)]
        kernel.PeekNamedPipe.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint32,
                                       ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint32), ctypes.c_void_p]
        kernel.ReadFile.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint32,
                                   ctypes.POINTER(ctypes.c_uint32), ctypes.c_void_p]
        kernel.WriteFile.argtypes = kernel.ReadFile.argtypes
        pipes = [r"\\.\pipe\ToSrvPipe", r"\\.\pipe\FromSrvPipe"]
        for pipe in pipes:
            if kernel.WaitNamedPipeW(pipe, 1) or ctypes.get_last_error() != 2:
                raise RuntimeError("An existing stock Audacity pipe blocks this isolated session.")
        prepare_consumer_profile(Path(self.command[0]), self.configuration,
                                 self.environment, self.scratch)
        self.stdout = (self.scratch / "_stdout.log").open("w", encoding="utf-8")
        self.stderr = (self.scratch / "_stderr.log").open("w", encoding="utf-8")
        self.app = OwnedConsumerProcess(self.command, Path(self.command[0]).parent,
                                       self.environment, self.stdout, self.stderr)
        user = ctypes.WinDLL("user32", use_last_error=True)
        user.WaitForInputIdle.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
        deadline = time.monotonic() + 40
        while True:
            text = startup_window_messages(self.app.pid, visible_only=True)
            if (self.project.stem in text and "Track Panel" in text
                    and kernel.WaitNamedPipeW(pipes[0], 50)):
                break
            if self.app.poll() is not None or time.monotonic() >= deadline:
                raise RuntimeError("Stock file startup failed: " + text)
            time.sleep(0.05)
        if user.WaitForInputIdle(self.app.process, 30000):
            raise RuntimeError("Stock Audacity did not finish file startup.")
        self.check_display()
        for pipe in pipes:
            handle = kernel.CreateFileW(pipe, 0xC0000000, 0, None, 3, 0, None)
            if handle == ctypes.c_void_p(-1).value:
                raise ctypes.WinError(ctypes.get_last_error())
            self.handles.append(handle)
            pid = ctypes.c_uint32()
            if not kernel.GetNamedPipeServerProcessId(handle, ctypes.byref(pid)) or pid.value != self.app.pid:
                raise RuntimeError("Stock scripting pipe belongs to another process; no command sent.")

    def check_display(self):
        """Refuse commands if any owned shown frame lies outside the selected screen."""
        from ctypes import wintypes as w

        user = ctypes.WinDLL("user32", use_last_error=True)
        callback = ctypes.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
        user.GetWindowThreadProcessId.argtypes = [w.HWND, ctypes.POINTER(w.DWORD)]
        user.GetWindowRect.argtypes = [w.HWND, ctypes.POINTER(w.RECT)]
        user.IsWindowVisible.argtypes = [w.HWND]
        user.EnumWindows.argtypes = [callback, w.LPARAM]
        rows: list[list[int] | None] = []
        if self.app is None:
            raise RuntimeError("No owned consumer process is open.")
        owned_pid = self.app.pid

        @callback
        def inspect(window, _):
            pid = w.DWORD()
            user.GetWindowThreadProcessId(window, ctypes.byref(pid))
            if pid.value == owned_pid and user.IsWindowVisible(window):
                rectangle = w.RECT()
                if not user.GetWindowRect(window, ctypes.byref(rectangle)):
                    rows.append(None)
                elif rectangle.right > rectangle.left and rectangle.bottom > rectangle.top:
                    rows.append([rectangle.left, rectangle.top, rectangle.right, rectangle.bottom])
            return True

        user.EnumWindows(inspect, 0)
        left, top, right, bottom = self.display["work"]
        if not rows or any(row is None or not (
            left <= row[0] < row[2] <= right and top <= row[1] < row[3] <= bottom
        ) for row in rows):
            raise RuntimeError(f"Stock project window escaped selected display: {rows}; {self.display}")

    def send(self, text):
        # Stock Windows mod-script-pipe consumes a NUL-terminated line.
        if self.kernel is None or self.app is None or len(self.handles) != 2:
            raise RuntimeError("No owned consumer scripting session is open.")
        data = (text + "\r\n\0").encode("utf-8")
        count = ctypes.c_uint32()
        if not self.kernel.WriteFile(self.handles[0], data, len(data), ctypes.byref(count), None):
            raise ctypes.WinError(ctypes.get_last_error())
        response = b""
        deadline = time.monotonic() + 45
        while time.monotonic() < deadline:
            available = ctypes.c_uint32()
            if not self.kernel.PeekNamedPipe(self.handles[1], None, 0, None, ctypes.byref(available), None):
                raise ctypes.WinError(ctypes.get_last_error())
            if available.value:
                buffer = ctypes.create_string_buffer(available.value)
                if not self.kernel.ReadFile(self.handles[1], buffer, len(buffer), ctypes.byref(count), None):
                    raise ctypes.WinError(ctypes.get_last_error())
                response += buffer.raw[:count.value]
                reply = response.decode("utf-8").replace("\x00", "").replace("\r\n", "\n")
                if "BatchCommand finished:" in reply and reply.endswith("\n\n"):
                    if "BatchCommand finished: OK" not in reply:
                        raise RuntimeError(f"Stock Audacity rejected {text}: {reply}")
                    return reply.split("BatchCommand", 1)[0].strip()
            time.sleep(0.01)
        raise RuntimeError(f"Stock command timed out: {text}; reply {response!r}; "
                           + startup_window_messages(self.app.pid))

    def query(self, text):
        return json.loads(self.send(text))

    def __exit__(self, kind, error, traceback):
        problems = []
        for handle in self.handles:
            assert self.kernel is not None
            if not self.kernel.CloseHandle(handle):
                problems.append(str(ctypes.WinError(ctypes.get_last_error())))
        self.handles.clear()
        status = self.app.poll() if self.app else "not-started"
        if self.app:
            try:
                self.app.close()
            except BaseException as cleanup:
                problems.append(f"owned process cleanup: {cleanup}")
        for stream in (self.stdout, self.stderr):
            if stream:
                stream.close()
        if error is not None or problems:
            evidence = {}
            for name in ("stdout", "stderr"):
                path = self.scratch / ("_" + name + ".log")
                evidence[name] = path.read_text(encoding="utf-8", errors="replace")[-8000:] if path.is_file() else "<absent>"
            raise RuntimeError(
                f"Stock project {self.project}: {type(error).__name__}: {error}; "
                f"command {self.command!r}; exit status {status}; "
                f"stdout: {evidence['stdout'] or '<absent>'}; stderr: {evidence['stderr'] or '<absent>'}; "
                f"cleanup: {problems or 'OK'}"
            ) from error
        return False


def read_compatibility_fixture_inputs(root):
    """Bind the current descriptor and every project/audio input to immutable acceptance."""
    descriptor = root / "_compatibility-expected.json"
    expected = json.loads(descriptor.read_text(encoding="utf-8"))
    if not isinstance(expected, list) or not expected:
        raise RuntimeError("The compatibility fixture descriptor must contain project cases.")
    files = {descriptor.name: hashlib.sha256(descriptor.read_bytes()).hexdigest()}
    for case in expected:
        for field in ("project", "audio"):
            if field not in case:
                continue
            name = case[field]
            if not isinstance(name, str) or Path(name).name != name:
                raise RuntimeError("Compatibility fixture paths must be direct case-owned children.")
            path = root / name
            if path.is_symlink() or not path.is_file():
                raise RuntimeError("A compatibility input is missing or linked: " + str(path))
            files[name] = hashlib.sha256(path.read_bytes()).hexdigest()
    return files


def check_portable_audacity_projects(root, display, ffmpeg):
    """Use pinned consumers; fixture expectations belong to native acceptance.

    Keep the single current descriptor with its generated projects until the
    marked case owner replaces that fixture group. It is accepted input data,
    never a command log or an execution checkpoint.
    """
    with AudacityConsumerCache(root.parent / "_audacity-consumers", PINNED_CONSUMERS) as cache:
        consumers = [cache.prepare_consumer(package) for package in PINNED_CONSUMERS]
        outcomes = []
        for consumer in consumers:
            result = check_stock_audacity_projects(root, consumer["executable"], display, ffmpeg)
            result["package"] = {key: value for key, value in consumer.items() if key != "executable"}
            outcomes.append(result)
        return outcomes


def check_stock_audacity_projects(root, stock_executable, display, ffmpeg):
    """Open, save and reopen each generated file twice through ordinary startup.

    Only this marked root holds copied binaries, preferences and working files.
    One private directory is replaced on entry and removed on every exit. Its
    sessions own process trees, so failed reporter children cannot lock cleanup.
    Source projects and the fixture descriptor remain read-only inputs.
    """
    stock_executable = Path(stock_executable).resolve(strict=True)
    if not (root / "_owner").is_file() or (root / "_owner").read_text() != "AuTopsy behavior tests 1":
        raise RuntimeError("Stock compatibility requires the marked, exclusively owned native test root.")
    version = executable_version(stock_executable)
    if version[0] != 3:
        raise RuntimeError("The AUP3 compatibility consumer must be stock Audacity 3.")
    expected = json.loads((root / "_compatibility-expected.json").read_text(encoding="utf-8"))
    before = read_compatibility_fixture_inputs(root)
    private = root / "_stock-audacity"
    signature = "AuTopsy stock consumer scratch 1"
    marker = private / "_owner"
    if private.exists():
        if private.is_symlink() or not marker.is_file() or marker.read_text() != signature:
            raise RuntimeError("The stock consumer scratch directory has another owner.")
        shutil.rmtree(private)
    private.mkdir()
    marker.write_text(signature)
    primary = None
    try:
        copied = private / "_app"
        shutil.copytree(stock_executable.parent, copied,
                        ignore=shutil.ignore_patterns("Portable Settings", "*.aup3", "*.aup4", "*.aup"))
        executable = copied / stock_executable.name
        profile = copied / "Portable Settings"
        profile.mkdir()
        temporary, gui_temporary = private / "_temp", private / "_gui_temp"
        temporary.mkdir()
        gui_temporary.mkdir()
        module = copied / "modules/mod-script-pipe.dll"
        stamp = datetime.fromtimestamp(module.stat().st_mtime).strftime("%Y-%m-%dT%H:%M:%S")  # noqa: DTZ006 -- native preference uses local file time.
        left, top, right, bottom = display["work"]
        width, height = min(1100, right-left-32), min(650, bottom-top-32)
        configuration = (
            "WantAssociateFiles=0\n[Version]\n" +
            f"Major={version[0]}\nMinor={version[1]}\nMicro={version[2]}\n" +
            f"[Directories]\nTempDir={temporary.as_posix()}\n" +
            "[GUI]\nShowSplashScreen=0\nShowHowTo=0\n[Update]\nDefaultUpdatesChecking=0\n" +
            "[Module]\nmod-script-pipe=1\n[ModuleDateTime]\nmod-script-pipe=" + stamp + "\n" +
            f"[Window]\nX={left+16}\nY={top+16}\nWidth={width}\nHeight={height}\n" +
            f"Normal_X={left+16}\nNormal_Y={top+16}\nNormal_Width={width}\nNormal_Height={height}\n" +
            "Maximized=0\nIconized=0\n")
        environment = dict(os.environ, TEMP=str(gui_temporary), TMP=str(gui_temporary))
        for name in ("AUTOPSY_BEHAVIOR_ROOT", "AUTOPSY_TEST_DISPLAY", "AUTOPSY_OWNED_APP_PID"):
            environment.pop(name, None)
        outcomes: list[str] = []
        for case in expected:
            for repeat in range(2):
                working = private / f"_input-{len(outcomes)}-{repeat}.aup3"
                shutil.copyfile(root / case["project"], working)
                reopened = private / f"_reopened-{len(outcomes)}-{repeat}.aup3"
                with StockAudacitySession(executable, working, environment, display, private, configuration) as session:
                    actual = read_audio_project_state(session.query)
                    if actual != case["state"]:
                        raise RuntimeError(f"Stock content differs: expected {case['state']}; got {actual}")
                    if case.get("audio"):
                        audio = private / "_mix.wav"
                        select_audio_project_tracks(session.send, session.query)
                        session.send("SelectTime: Start=0 End=5")
                        session.send(f'Export2: Filename="{audio.as_posix()}" NumChannels=2')
                        pcm = []
                        for path in (root / case["audio"], audio):
                            samples = array("f")
                            samples.frombytes(run_process(
                                [ffmpeg, "-v", "error", "-i", str(path), "-f", "f32le",
                                 "-ac", "2", "-ar", "48000", "-"], text=False, timeout=30))
                            pcm.append(samples)
                        lengths = [len(samples) for samples in pcm]
                        difference = max((abs(a-b) for a, b in zip(*pcm, strict=True)), default=0) if lengths[0] == lengths[1] else None
                        if difference is None or difference > 0.0005:
                            raise RuntimeError(f"Stock audio mix differs: sample lengths {lengths}, largest difference {difference}.")
                        audio.unlink()
                    output = private / f"_saved-{len(outcomes)}-{repeat}.aup3"
                    session.send(f'SaveProject2: Filename="{output.as_posix()}"')
                    session.send(f'SaveCopy: Filename="{reopened.as_posix()}"')
                # Each load has its own GUI-thread startup. Never use scripting
                # New/Close/OpenProject2 to create or destroy stock GUI objects.
                with StockAudacitySession(executable, reopened, environment, display, private, configuration) as session:
                    if read_audio_project_state(session.query) != case["state"]:
                        raise RuntimeError("Stock save/reopen changed standard audio or labels.")
            outcomes.append(case["project"])
        if read_compatibility_fixture_inputs(root) != before:
            raise RuntimeError("The consumer changed an original compatibility fixture.")
        return {"version": list(version), "executable_sha256": hashlib.sha256(executable.read_bytes()).hexdigest(),
                "script_module_sha256": hashlib.sha256(module.read_bytes()).hexdigest(),
                "projects": outcomes, "open_save_reopen_repeats": 2, "audio_tolerance": 0.0005}
    except BaseException as error:
        primary = error
        raise
    finally:
        try:
            shutil.rmtree(private)
        except OSError as cleanup:
            if primary is not None:
                raise RuntimeError(f"{primary}; stock scratch cleanup at {private}: {cleanup}") from primary
            raise RuntimeError(f"Stock scratch cleanup at {private}: {cleanup}") from cleanup


def read_audio_project_state(query):
    """Compare standard audio/label data, excluding optional presentation tracks."""
    tracks = query("GetInfo: Type=Tracks Format=JSON")
    return {
        "tracks": [{key: item[key] for key in ("kind", "name", "start", "end", "channels", "volume", "pan", "mute", "solo") if key in item}
                   for item in tracks if item["kind"] not in ("video", "trackgroup")],
        "clips": [{key: item[key] for key in ("name", "start", "end") if key in item}
                  for item in query("GetInfo: Type=Clips Format=JSON")],
        "labels": [item[1] for item in query("GetInfo: Type=Labels Format=JSON")],
    }


def select_audio_project_tracks(send, query):
    """Select audio by native kind, without invoking label/clip text selection."""
    mode = "Set"
    for index, track in enumerate(query("GetInfo: Type=Tracks Format=JSON")):
        if track["kind"] == "wave":
            send(f"SelectTracks: Track={index} TrackCount=1 Mode={mode}")
            mode = "Add"


def run_process(arguments, **options):
    if os.name == "nt":
        options.setdefault("creationflags", subprocess.CREATE_NO_WINDOW)
    try:
        result = subprocess.run(arguments, capture_output=True, **options)
    except subprocess.TimeoutExpired as error:
        raise RuntimeError(
            f"Command timed out: {subprocess.list2cmdline([str(x) for x in arguments])}\n"
            f"stdout: {error.stdout or '<absent>'!r}\nstderr: {error.stderr or '<absent>'!r}"
        ) from error
    if result.returncode:

        def display(value):
            if not value:
                return "<absent>"
            return value.decode(errors="replace") if isinstance(value, bytes) else value

        raise RuntimeError(
            f"Command: {subprocess.list2cmdline([str(x) for x in arguments])}\n"
            f"Exit: {result.returncode}\nstdout:\n{display(result.stdout)}\n"
            f"stderr:\n{display(result.stderr)}"
        )
    return result.stdout


def executable_version(executable):
    """Read the PE resource before creating a first-run app profile."""
    version = ctypes.WinDLL("version", use_last_error=True)
    version.GetFileVersionInfoSizeW.argtypes = [ctypes.c_wchar_p, ctypes.c_void_p]
    version.GetFileVersionInfoW.argtypes = [
        ctypes.c_wchar_p,
        ctypes.c_uint32,
        ctypes.c_uint32,
        ctypes.c_void_p,
    ]
    version.VerQueryValueW.argtypes = [
        ctypes.c_void_p,
        ctypes.c_wchar_p,
        ctypes.POINTER(ctypes.c_void_p),
        ctypes.POINTER(ctypes.c_uint32),
    ]
    size = version.GetFileVersionInfoSizeW(str(executable), None)
    if not size:
        raise ctypes.WinError(ctypes.get_last_error())
    buffer = ctypes.create_string_buffer(size)
    if not version.GetFileVersionInfoW(str(executable), 0, size, buffer):
        raise ctypes.WinError(ctypes.get_last_error())
    value = ctypes.c_void_p()
    length = ctypes.c_uint32()
    if not version.VerQueryValueW(
        buffer, "\\", ctypes.byref(value), ctypes.byref(length)
    ):
        raise ctypes.WinError(ctypes.get_last_error())
    fields = ctypes.cast(value, ctypes.POINTER(ctypes.c_uint32))
    if length.value < 52 or fields[0] != 0xFEEF04BD:
        raise RuntimeError("The executable has no fixed Windows version resource.")
    return fields[2] >> 16, fields[2] & 0xFFFF, fields[3] >> 16


def startup_window_messages(pid, visible_only=False):
    """Read owned child captions, optionally only after its main frame is shown."""
    user = ctypes.WinDLL("user32", use_last_error=True)
    callback_type = ctypes.WINFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_ssize_t)
    user.GetWindowThreadProcessId.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(ctypes.c_uint32),
    ]
    user.GetWindowTextW.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p, ctypes.c_int]
    user.IsWindowVisible.argtypes = [ctypes.c_void_p]
    user.EnumWindows.argtypes = [callback_type, ctypes.c_ssize_t]
    user.EnumChildWindows.argtypes = [ctypes.c_void_p, callback_type, ctypes.c_ssize_t]
    messages = []

    @callback_type
    def child(window, _):
        text = ctypes.create_unicode_buffer(2048)
        user.GetWindowTextW(window, text, len(text))
        if text.value:
            messages.append(text.value)
        return 1

    @callback_type
    def parent(window, _):
        owner = ctypes.c_uint32()
        user.GetWindowThreadProcessId(window, ctypes.byref(owner))
        if owner.value == pid and (not visible_only or user.IsWindowVisible(window)):
            child(window, 0)
            user.EnumChildWindows(window, child, 0)
        return 1

    user.EnumWindows(parent, 0)
    return " | ".join(messages) or "<no window text>"
