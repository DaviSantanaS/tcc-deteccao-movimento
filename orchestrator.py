#!/usr/bin/env python3
"""Supervisiona o pipeline MediaMTX, publicador e detector."""

from __future__ import annotations

import argparse
import errno
import fcntl
import json
import os
import pathlib
import shutil
import signal
import socket
import stat
import subprocess
import sys
import threading
import time
from dataclasses import dataclass
from types import FrameType
from typing import Any, BinaryIO, Callable, TextIO

from recording import (
    RecordingConfig,
    RecordingController,
    RecordingError,
    SegmentCatalog,
    build_segmenter_command,
)


class Error(RuntimeError):
    """Erro de configuração, inicialização ou supervisão."""


def path(base: pathlib.Path, value: str) -> pathlib.Path:
    candidate = pathlib.Path(value).expanduser()
    if candidate.is_absolute():
        return candidate
    return (base / candidate).resolve()


@dataclass
class Spec:
    name: str
    argv: list[str]
    cwd: pathlib.Path
    required: list[pathlib.Path]
    env: dict[str, str]

    @classmethod
    def load(
        cls,
        name: str,
        data: dict[str, Any],
        base: pathlib.Path,
    ) -> Spec:
        argv = data.get("argv")
        if (
            not isinstance(argv, list)
            or not argv
            or not all(isinstance(item, str) and item for item in argv)
        ):
            raise Error(f"{name}.argv inválido")

        cwd = path(base, data.get("cwd", "."))
        return cls(
            name=name,
            argv=argv,
            cwd=cwd,
            required=[
                path(cwd, item) for item in data.get("required_paths", [])
            ],
            env=data.get("environment", {}),
        )


class Lock:
    def __init__(self, lock_path: pathlib.Path) -> None:
        self.path = lock_path
        self.file: TextIO | None = None

    def __enter__(self) -> Lock:
        self.path.parent.mkdir(parents=True, exist_ok=True)
        candidate = self.path.open("a+", encoding="utf-8")
        try:
            fcntl.flock(
                candidate,
                fcntl.LOCK_EX | fcntl.LOCK_NB,
            )
        except BlockingIOError as exc:
            candidate.close()
            raise Error(f"outra instância mantém {self.path}") from exc

        candidate.seek(0)
        candidate.truncate()
        candidate.write(f"{os.getpid()}\n")
        candidate.flush()
        self.file = candidate
        return self

    def __exit__(
        self,
        _exc_type: object,
        _exc_value: object,
        _traceback: object,
    ) -> None:
        if self.file is not None:
            fcntl.flock(self.file, fcntl.LOCK_UN)
            self.file.close()
            self.file = None


class FIFO:
    def __init__(
        self,
        fifo_path: pathlib.Path,
        stop: threading.Event,
        output_path: pathlib.Path,
        interval: float = 0.1,
        sink: Callable[[str], None] | None = None,
    ) -> None:
        self.path = fifo_path
        self.stop = stop
        self.output_path = output_path
        self.interval = interval
        self.sink = sink
        self.ready = threading.Event()
        self.error: BaseException | None = None
        self.fd: int | None = None
        self.thread = threading.Thread(
            target=self.run,
            name="motion-fifo-reader",
        )

    def prepare(self) -> None:
        try:
            mode = self.path.stat().st_mode
        except FileNotFoundError:
            self.path.parent.mkdir(parents=True, exist_ok=True)
            try:
                os.mkfifo(self.path)
            except FileExistsError:
                pass
            mode = self.path.stat().st_mode

        if not stat.S_ISFIFO(mode):
            raise Error(f"não é FIFO: {self.path}")

    def start(self, timeout: float) -> None:
        self.prepare()
        self.output_path.parent.mkdir(parents=True, exist_ok=True)
        self.thread.start()
        if not self.ready.wait(timeout) or self.error:
            raise Error(
                f"leitor FIFO não ficou pronto: {self.error}"
            )

    def run(self) -> None:
        buffer = b""
        try:
            self.fd = os.open(
                self.path,
                os.O_RDWR | os.O_NONBLOCK,
            )
            with self.output_path.open(
                "a",
                encoding="utf-8",
                buffering=1,
            ) as output:
                self.ready.set()
                while not self.stop.is_set():
                    try:
                        chunk = os.read(self.fd, 4096)
                    except BlockingIOError:
                        self.stop.wait(self.interval)
                        continue
                    except OSError as exc:
                        if exc.errno in (
                            errno.EAGAIN,
                            errno.EWOULDBLOCK,
                        ):
                            self.stop.wait(self.interval)
                            continue
                        raise

                    if not chunk:
                        self.stop.wait(self.interval)
                        continue

                    buffer += chunk
                    while b"\n" in buffer:
                        line, buffer = buffer.split(b"\n", 1)
                        decoded = line.decode(errors="replace")
                        output.write(decoded + "\n")
                        if self.sink is not None:
                            self.sink(decoded)

                if buffer:
                    output.write(
                        buffer.decode(errors="replace") + "\n"
                    )
        except BaseException as exc:
            self.error = exc
            self.ready.set()
        finally:
            if self.fd is not None:
                os.close(self.fd)
                self.fd = None

    def join(self, timeout: float) -> None:
        if self.thread.ident is None:
            return

        self.thread.join(timeout)
        if self.thread.is_alive():
            raise Error("thread FIFO não finalizou")
        if self.error:
            raise Error(
                f"thread FIFO falhou: {self.error}"
            )


@dataclass
class Child:
    spec: Spec
    proc: subprocess.Popen[bytes]
    stdout: BinaryIO
    stderr: BinaryIO

    @property
    def rc(self) -> int | None:
        return self.proc.poll()

    def close(self) -> None:
        self.stdout.close()
        self.stderr.close()


class Supervisor:
    def __init__(
        self,
        config: dict[str, Any],
        base: pathlib.Path,
        detector: str,
    ) -> None:
        self.config = config
        self.base = base
        self.stop = threading.Event()
        self.children: list[Child] = []
        self.closing = False

        self.logs = path(
            base,
            config.get("logs_dir", "logs/orchestrator"),
        )
        self.recording_config = RecordingConfig.load(
            config.get("recording"),
            base,
        )
        self.catalog: SegmentCatalog | None = None
        self.recording: RecordingController | None = None
        if self.recording_config.enabled:
            self.catalog = SegmentCatalog(self.recording_config)
            self.recording = RecordingController(
                self.recording_config,
                detector,
                config["publisher"]["argv"][0],
                config.get("ffprobe", {}).get("executable", "ffprobe"),
                self.catalog,
            )
            self.recording.set_log_path(self.logs / "recording.log")

        fifo_config = config.get("fifo", {})
        self.fifo = FIFO(
            path(
                base,
                fifo_config.get("path", "/tmp/motion_bus"),
            ),
            self.stop,
            self.logs / "motion_bus.log",
            float(
                fifo_config.get(
                    "poll_interval_seconds",
                    0.1,
                )
            ),
            self.recording.enqueue if self.recording is not None else None,
        )

        detectors = config.get("detectors", {})
        if detector not in detectors:
            raise Error(f"detector desconhecido: {detector}")

        self.specs = [
            Spec.load(
                "mediamtx",
                config["mediamtx"],
                base,
            ),
            Spec.load(
                "publisher",
                config["publisher"],
                base,
            ),
            Spec.load(
                "detector",
                detectors[detector],
                base,
            ),
        ]

    def preflight(self) -> None:
        self.logs.mkdir(parents=True, exist_ok=True)
        for spec in self.specs:
            if not spec.cwd.is_dir():
                raise Error(f"cwd inexistente: {spec.cwd}")

            if "/" in spec.argv[0]:
                executable = path(spec.cwd, spec.argv[0])
            else:
                executable = shutil.which(spec.argv[0])

            if not executable or not os.access(
                executable,
                os.X_OK,
            ):
                raise Error(
                    f"executável inválido: {spec.argv[0]}"
                )

            for required_path in spec.required:
                if not required_path.exists():
                    raise Error(
                        "caminho obrigatório inexistente: "
                        f"{required_path}"
                    )

        probe = self.config.get("ffprobe", {}).get(
            "executable",
            "ffprobe",
        )
        if not shutil.which(probe) and not os.access(
            probe,
            os.X_OK,
        ):
            raise Error(f"ffprobe inválido: {probe}")

        self.fifo.prepare()
        if self.recording_config.enabled:
            assert self.catalog is not None
            self.catalog.prepare()

    def _probe_segment(self, segment: pathlib.Path, timeout: float) -> float | None:
        probe = self.config.get("ffprobe", {}).get("executable", "ffprobe")
        process = subprocess.Popen(
            [
                probe,
                "-v",
                "error",
                "-select_streams",
                "v:0",
                "-show_entries",
                "stream=codec_type:format=duration",
                "-of",
                "json",
                str(segment),
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            shell=False,
            start_new_session=True,
        )
        try:
            stdout, _stderr = process.communicate(timeout=timeout)
        except subprocess.TimeoutExpired:
            if process.poll() is None:
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                process.wait()
            return None
        if process.returncode != 0:
            return None
        try:
            data = json.loads(stdout)
            if not data.get("streams"):
                return None
            return float(data["format"]["duration"])
        except (KeyError, TypeError, ValueError, json.JSONDecodeError):
            return None

    def segmenter_ready(self, child: Child) -> None:
        assert self.catalog is not None
        deadline = time.monotonic() + self.recording_config.segmenter_ready_timeout_seconds
        durations: dict[pathlib.Path, float] = {}
        while time.monotonic() < deadline and not self.stop.is_set():
            self.alive(child)
            for segment in self.catalog.scan():
                if segment.path in durations:
                    continue
                duration = self._probe_segment(
                    segment.path,
                    min(3.0, max(0.01, deadline - time.monotonic())),
                )
                if duration is not None and duration > 0:
                    durations[segment.path] = duration
            if sum(durations.values()) >= self.recording_config.pre_event_seconds:
                return
            self.stop.wait(0.1)
        raise Error("segmentador não ficou pronto com cobertura de pré-evento")

    def spawn(self, spec: Spec) -> Child:
        stdout = (
            self.logs / f"{spec.name}.stdout.log"
        ).open("ab", buffering=0)
        stderr = (
            self.logs / f"{spec.name}.stderr.log"
        ).open("ab", buffering=0)
        environment = os.environ.copy()
        environment.update(spec.env)

        try:
            process = subprocess.Popen(
                spec.argv,
                cwd=spec.cwd,
                env=environment,
                stdout=stdout,
                stderr=stderr,
                shell=False,
                start_new_session=True,
            )
        except BaseException:
            stdout.close()
            stderr.close()
            raise

        child = Child(
            spec,
            process,
            stdout,
            stderr,
        )
        self.children.append(child)
        return child

    def alive(self, child: Child) -> None:
        if child.rc is not None:
            raise Error(
                f"{child.spec.name} encerrou "
                f"inesperadamente (rc={child.rc})"
            )

    def port(self, child: Child) -> None:
        health = self.config["mediamtx"]["health"]
        deadline = (
            time.monotonic()
            + health["timeout_seconds"]
        )
        while (
            time.monotonic() < deadline
            and not self.stop.is_set()
        ):
            self.alive(child)
            try:
                with socket.create_connection(
                    (health["host"], health["port"]),
                    timeout=health.get(
                        "interval_seconds",
                        0.1,
                    ),
                ):
                    return
            except OSError:
                self.stop.wait(
                    health.get(
                        "interval_seconds",
                        0.1,
                    )
                )

        raise Error("porta RTSP não ficou pronta")

    def probe(self, timeout: float) -> bool:
        probe_config = self.config.get("ffprobe", {})
        command = [
            probe_config.get("executable", "ffprobe"),
            "-rtsp_transport",
            "tcp",
            "-v",
            "error",
            "-select_streams",
            "v:0",
            "-show_entries",
            "stream=codec_type",
            "-of",
            "default=noprint_wrappers=1:nokey=1",
            self.config["stream_url"],
        ]
        process = subprocess.Popen(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            shell=False,
            start_new_session=True,
        )
        try:
            stdout, _stderr = process.communicate(
                timeout=timeout
            )
        except subprocess.TimeoutExpired:
            if process.poll() is None:
                try:
                    os.killpg(
                        process.pid,
                        signal.SIGKILL,
                    )
                except ProcessLookupError:
                    pass
                process.wait()
            return False

        return (
            process.returncode == 0
            and b"video" in stdout.splitlines()
        )

    def stream(self, child: Child) -> None:
        health = self.config["publisher"]["health"]
        probe_config = self.config.get("ffprobe", {})
        deadline = (
            time.monotonic()
            + health["timeout_seconds"]
        )

        while (
            time.monotonic() < deadline
            and not self.stop.is_set()
        ):
            self.alive(child)
            attempt_timeout = min(
                probe_config.get(
                    "attempt_timeout_seconds",
                    3,
                ),
                max(
                    0.01,
                    deadline - time.monotonic(),
                ),
            )
            if self.probe(attempt_timeout):
                return
            self.stop.wait(
                health.get(
                    "interval_seconds",
                    0.25,
                )
            )

        raise Error("stream RTSP não ficou pronto")

    def detector_ready(self, child: Child) -> None:
        deadline = (
            time.monotonic()
            + self.config.get(
                "detector_startup_grace_seconds",
                1,
            )
        )
        while (
            time.monotonic() < deadline
            and not self.stop.is_set()
        ):
            self.alive(child)
            self.stop.wait(
                min(
                    0.1,
                    max(
                        0,
                        deadline - time.monotonic(),
                    ),
                )
            )
        self.alive(child)

    def start(self) -> None:
        self.preflight()
        if self.recording is not None:
            self.recording.start()
        self.fifo.start(
            self.config.get("fifo", {}).get(
                "ready_timeout_seconds",
                2,
            )
        )

        media = self.spawn(self.specs[0])
        self.port(media)

        publisher = self.spawn(self.specs[1])
        self.stream(publisher)

        if self.recording_config.enabled:
            assert self.catalog is not None
            segmenter_spec = Spec(
                name="segmenter",
                argv=build_segmenter_command(
                    self.recording_config,
                    self.config["publisher"]["argv"][0],
                    self.config["stream_url"],
                    self.catalog.next_sequence(),
                ),
                cwd=self.base,
                required=[],
                env={},
            )
            segmenter = self.spawn(segmenter_spec)
            self.catalog.segmenter_alive = lambda: segmenter.rc is None
            self.segmenter_ready(segmenter)

        detector = self.spawn(self.specs[2])
        self.detector_ready(detector)

    def monitor(self) -> None:
        interval = self.config.get(
            "monitor_interval_seconds",
            0.2,
        )
        while not self.stop.wait(interval):
            for child in self.children:
                self.alive(child)

            if (
                self.fifo.error
                or not self.fifo.thread.is_alive()
            ):
                raise Error("leitor FIFO encerrou")
            if self.recording is not None and self.recording.error is not None:
                raise Error(f"controlador de gravação encerrou: {self.recording.error}")

    def shutdown(self) -> None:
        if self.closing:
            return

        self.closing = True
        self.stop.set()
        pending = list(reversed(self.children))
        shutdown_config = self.config.get(
            "shutdown",
            {},
        )
        stages = (
            (
                signal.SIGINT,
                "sigint_timeout_seconds",
                5,
            ),
            (
                signal.SIGTERM,
                "sigterm_timeout_seconds",
                3,
            ),
            (
                signal.SIGKILL,
                "sigkill_timeout_seconds",
                1,
            ),
        )

        for current_signal, key, default in stages:
            for child in pending:
                if child.rc is not None:
                    continue
                try:
                    os.killpg(
                        child.proc.pid,
                        current_signal,
                    )
                except ProcessLookupError:
                    pass

            deadline = (
                time.monotonic()
                + shutdown_config.get(key, default)
            )
            while pending and time.monotonic() < deadline:
                time.sleep(0.02)
                pending = [
                    child
                    for child in pending
                    if child.rc is None
                ]

            if not pending:
                break

        recording_error: BaseException | None = None
        try:
            if self.recording is not None:
                self.recording.shutdown(
                    shutdown_config.get("recording_timeout_seconds", 30)
                )
            self.fifo.join(shutdown_config.get("thread_timeout_seconds", 2))
        except (RecordingError, OSError) as exc:
            recording_error = exc
        finally:
            for child in self.children:
                child.close()

        if pending:
            raise Error("processos não encerraram")
        if recording_error is not None:
            raise Error(str(recording_error))


def load_config(
    config_path: pathlib.Path,
) -> tuple[dict[str, Any], pathlib.Path]:
    try:
        with config_path.open(encoding="utf-8") as source:
            config = json.load(source)
    except (OSError, json.JSONDecodeError) as exc:
        raise Error(
            f"configuração inválida: {exc}"
        ) from exc

    for key in (
        "stream_url",
        "mediamtx",
        "publisher",
        "detectors",
    ):
        if key not in config:
            raise Error(f"campo ausente: {key}")

    return config, config_path.resolve().parent


def install_signal_handlers(
    stop: threading.Event,
) -> dict[signal.Signals, Any]:
    previous: dict[signal.Signals, Any] = {}

    def request_stop(
        signum: int,
        _frame: FrameType | None,
    ) -> None:
        print(
            f"[orchestrator] "
            f"{signal.Signals(signum).name}",
            flush=True,
        )
        stop.set()

    for current_signal in (
        signal.SIGINT,
        signal.SIGTERM,
    ):
        previous[current_signal] = signal.signal(
            current_signal,
            request_stop,
        )

    return previous


def restore_signal_handlers(
    previous: dict[signal.Signals, Any],
) -> None:
    for current_signal, handler in previous.items():
        signal.signal(current_signal, handler)


def parse_args(
    argv: list[str] | None = None,
) -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--config",
        type=pathlib.Path,
        default=pathlib.Path(
            "orchestrator.example.json"
        ),
    )
    parser.add_argument(
        "--detector",
        choices=("mog2", "diff"),
        required=True,
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    previous_handlers: dict[signal.Signals, Any] = {}

    try:
        config, base = load_config(args.config)
        lock_path = path(
            base,
            config.get(
                "lock_file",
                "/tmp/tcc-motion-orchestrator.lock",
            ),
        )
        with Lock(lock_path):
            supervisor = Supervisor(
                config,
                base,
                args.detector,
            )
            previous_handlers = install_signal_handlers(
                supervisor.stop
            )
            try:
                supervisor.start()
                supervisor.monitor()
            finally:
                supervisor.shutdown()
        return 0
    except (
        Error,
        OSError,
        KeyError,
        TypeError,
        ValueError,
    ) as exc:
        print(
            f"[orchestrator][erro] {exc}",
            file=sys.stderr,
        )
        return 1
    finally:
        restore_signal_handlers(previous_handlers)


if __name__ == "__main__":
    raise SystemExit(main())
