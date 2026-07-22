"""Gravação orientada a eventos sobre segmentos de vídeo comprimido."""

from __future__ import annotations

import datetime as dt
import json
import os
import pathlib
import queue
import re
import signal
import subprocess
import threading
import time
import uuid
from dataclasses import dataclass
from enum import Enum
from typing import Any, Callable


class RecordingError(RuntimeError):
    """Erro de configuração ou finalização da gravação."""


class RecordingState(Enum):
    IDLE = "IDLE"
    MOTION_ACTIVE = "MOTION_ACTIVE"
    POST_EVENT = "POST_EVENT"
    FINALIZING = "FINALIZING"


@dataclass(frozen=True)
class RecordingConfig:
    enabled: bool
    segments_dir: pathlib.Path
    recordings_dir: pathlib.Path
    segment_duration_seconds: float
    pre_event_seconds: float
    post_event_seconds: float
    idle_retention_seconds: float
    finalization_margin_seconds: float
    container: str
    segmenter_ready_timeout_seconds: float
    command_timeout_seconds: float
    max_ring_bytes: int | None

    @classmethod
    def load(
        cls,
        data: Any,
        base: pathlib.Path,
    ) -> RecordingConfig:
        if data is None:
            data = {"enabled": False}
        if not isinstance(data, dict):
            raise RecordingError("recording deve ser um objeto")
        enabled = data.get("enabled", False)
        if not isinstance(enabled, bool):
            raise RecordingError("recording.enabled deve ser booleano")

        def resolve(value: str) -> pathlib.Path:
            candidate = pathlib.Path(value).expanduser()
            if candidate.is_absolute():
                return candidate
            return (base / candidate).resolve()

        def positive(name: str, default: float) -> float:
            value = data.get(name, default)
            if isinstance(value, bool) or not isinstance(value, (int, float)):
                raise RecordingError(f"recording.{name} deve ser numérico")
            value = float(value)
            if value <= 0:
                raise RecordingError(f"recording.{name} deve ser positivo")
            return value

        segments_value = data.get("segments_dir", "./runtime/segments")
        recordings_value = data.get("recordings_dir", "./recordings")
        if not isinstance(segments_value, str) or not isinstance(recordings_value, str):
            raise RecordingError("diretórios de recording devem ser strings")

        pre_event = positive("pre_event_seconds", 5)
        retention = positive("idle_retention_seconds", 10)
        if retention < pre_event:
            raise RecordingError(
                "recording.idle_retention_seconds deve ser maior ou igual a pre_event_seconds"
            )

        container = data.get("container", "mkv")
        if container not in {"mkv"}:
            raise RecordingError("recording.container suportado: mkv")

        max_ring_bytes = data.get("max_ring_bytes")
        if max_ring_bytes is not None:
            if isinstance(max_ring_bytes, bool) or not isinstance(max_ring_bytes, int):
                raise RecordingError("recording.max_ring_bytes deve ser inteiro")
            if max_ring_bytes <= 0:
                raise RecordingError("recording.max_ring_bytes deve ser positivo")

        return cls(
            enabled=enabled,
            segments_dir=resolve(segments_value),
            recordings_dir=resolve(recordings_value),
            segment_duration_seconds=positive("segment_duration_seconds", 1),
            pre_event_seconds=pre_event,
            post_event_seconds=positive("post_event_seconds", 5),
            idle_retention_seconds=retention,
            finalization_margin_seconds=positive("finalization_margin_seconds", 2),
            container=container,
            segmenter_ready_timeout_seconds=positive(
                "segmenter_ready_timeout_seconds", 30
            ),
            command_timeout_seconds=positive("command_timeout_seconds", 30),
            max_ring_bytes=max_ring_bytes,
        )


@dataclass(frozen=True)
class Segment:
    sequence: int
    path: pathlib.Path
    mtime: float
    size: int


@dataclass
class Event:
    event_id: str
    motion_on_ms: int
    motion_off_ms: int | None = None
    post_started_ms: int | None = None
    post_started_monotonic: float | None = None
    post_deadline_monotonic: float | None = None
    post_end_ms: int | None = None
    preserved: set[pathlib.Path] | None = None
    finalized_by_shutdown: bool = False

    def __post_init__(self) -> None:
        if self.preserved is None:
            self.preserved = set()


class SegmentCatalog:
    _pattern = re.compile(r"^segment_(\d{12})\.ts$")

    def __init__(
        self,
        config: RecordingConfig,
        monotonic: Callable[[], float] = time.monotonic,
    ) -> None:
        self.config = config
        self.monotonic = monotonic
        self.finalized_seen: dict[pathlib.Path, float] = {}
        self.segmenter_alive: Callable[[], bool] = lambda: False

    def prepare(self) -> None:
        self.config.segments_dir.mkdir(parents=True, exist_ok=True)
        self.config.recordings_dir.mkdir(parents=True, exist_ok=True)
        if not self.config.segments_dir.is_dir():
            raise RecordingError(f"diretório inválido: {self.config.segments_dir}")
        if not self.config.recordings_dir.is_dir():
            raise RecordingError(f"diretório inválido: {self.config.recordings_dir}")

    def next_sequence(self) -> int:
        segments = self.scan(include_active=True)
        if not segments:
            return 0
        return segments[-1].sequence + 1

    def scan(self, include_active: bool = False) -> list[Segment]:
        segments: list[Segment] = []
        if not self.config.segments_dir.exists():
            return segments
        for candidate in self.config.segments_dir.iterdir():
            match = self._pattern.match(candidate.name)
            if not match or not candidate.is_file():
                continue
            info = candidate.stat()
            if info.st_size <= 0:
                continue
            segments.append(
                Segment(int(match.group(1)), candidate, info.st_mtime, info.st_size)
            )
        segments.sort(key=lambda item: item.sequence)
        if segments and self.segmenter_alive() and not include_active:
            segments = segments[:-1]
        now = self.monotonic()
        for segment in segments:
            self.finalized_seen.setdefault(segment.path, now)
        return segments

    def select_pre_event(self, motion_on_ms: int) -> list[Segment]:
        cutoff = (
            motion_on_ms / 1000.0
            - self.config.pre_event_seconds
            - self.config.segment_duration_seconds
        )
        return [segment for segment in self.scan() if segment.mtime >= cutoff]

    def select_event(self, event: Event) -> list[Segment]:
        segments = self.scan()
        if not event.preserved:
            return []
        first_sequence = min(
            segment.sequence
            for segment in segments
            if segment.path in event.preserved
        )
        return [segment for segment in segments if segment.sequence >= first_sequence]

    def retain(
        self,
        preserved: set[pathlib.Path],
        log: Callable[[str], None],
    ) -> None:
        now = self.monotonic()
        segments = self.scan()
        removable = [
            segment
            for segment in segments
            if segment.path not in preserved
            and now - self.finalized_seen.get(segment.path, now)
            > self.config.idle_retention_seconds
        ]

        if self.config.max_ring_bytes is not None:
            total = sum(segment.size for segment in segments)
            for segment in segments:
                if total <= self.config.max_ring_bytes:
                    break
                if segment.path in preserved or segment in removable:
                    continue
                removable.append(segment)
                total -= segment.size

        for segment in removable:
            try:
                segment.path.unlink()
                self.finalized_seen.pop(segment.path, None)
                log(f"segmento removido: {segment.path}")
            except FileNotFoundError:
                pass


class RecordingController:
    def __init__(
        self,
        config: RecordingConfig,
        detector: str,
        ffmpeg: str,
        ffprobe: str,
        catalog: SegmentCatalog | None = None,
        monotonic: Callable[[], float] = time.monotonic,
        wall_time: Callable[[], float] = time.time,
    ) -> None:
        self.config = config
        self.detector = detector
        self.ffmpeg = ffmpeg
        self.ffprobe = ffprobe
        self.monotonic = monotonic
        self.wall_time = wall_time
        self.catalog = catalog or SegmentCatalog(config, monotonic)
        self.state = RecordingState.IDLE
        self.event: Event | None = None
        self.events: queue.Queue[str] = queue.Queue()
        self.stop = threading.Event()
        self.thread = threading.Thread(target=self.run, name="recording-controller")
        self.error: BaseException | None = None
        self.log_path = self.config.recordings_dir.parent / "logs" / "recording.log"
        self._log_file: Any = None

    def set_log_path(self, log_path: pathlib.Path) -> None:
        self.log_path = log_path

    def start(self) -> None:
        self.catalog.prepare()
        self.log_path.parent.mkdir(parents=True, exist_ok=True)
        self._log_file = self.log_path.open("a", encoding="utf-8", buffering=1)
        self.thread.start()

    def enqueue(self, line: str) -> None:
        if not self.stop.is_set():
            self.events.put_nowait(line)

    def log(self, message: str) -> None:
        if self._log_file is not None:
            timestamp = dt.datetime.fromtimestamp(
                self.wall_time(), dt.timezone.utc
            ).isoformat()
            self._log_file.write(f"{timestamp} {message}\n")

    def transition(self, new_state: RecordingState, reason: str) -> None:
        self.log(f"estado {self.state.value} -> {new_state.value}: {reason}")
        self.state = new_state

    def handle_line(self, line: str) -> None:
        parts = line.split()
        if len(parts) != 2 or parts[0] not in {"MOTION_ON", "MOTION_OFF"}:
            self.log(f"evento inválido ignorado: {line!r}")
            return
        try:
            timestamp_ms = int(parts[1])
        except ValueError:
            self.log(f"timestamp inválido ignorado: {line!r}")
            return

        if parts[0] == "MOTION_ON":
            self.motion_on(timestamp_ms)
        else:
            self.motion_off(timestamp_ms)

    def motion_on(self, timestamp_ms: int) -> None:
        if self.state is RecordingState.IDLE:
            selected = self.catalog.select_pre_event(timestamp_ms)
            self.event = Event(uuid.uuid4().hex, timestamp_ms)
            self.event.preserved.update(item.path for item in selected)
            self.log(f"segmentos pré-evento preservados: {len(selected)}")
            self.transition(RecordingState.MOTION_ACTIVE, "MOTION_ON")
            return
        if self.state is RecordingState.MOTION_ACTIVE:
            self.log("MOTION_ON duplicado ignorado")
            return
        if self.state is RecordingState.POST_EVENT:
            if self.event is not None:
                self.event.motion_off_ms = None
                self.event.post_started_ms = None
                self.event.post_started_monotonic = None
                self.event.post_deadline_monotonic = None
                self.event.post_end_ms = None
            self.transition(RecordingState.MOTION_ACTIVE, "movimento retornou")
            return
        self.log("MOTION_ON ignorado durante FINALIZING")

    def motion_off(self, timestamp_ms: int) -> None:
        if self.state is RecordingState.IDLE:
            self.log("MOTION_OFF inesperado em IDLE")
            return
        if self.state is not RecordingState.MOTION_ACTIVE:
            self.log(f"MOTION_OFF inesperado em {self.state.value}")
            return
        assert self.event is not None
        now = self.monotonic()
        self.event.motion_off_ms = timestamp_ms
        self.event.post_started_ms = timestamp_ms
        self.event.post_started_monotonic = now
        self.event.post_deadline_monotonic = now + self.config.post_event_seconds
        self.event.post_end_ms = timestamp_ms + int(self.config.post_event_seconds * 1000)
        self.transition(RecordingState.POST_EVENT, "MOTION_OFF")

    def update_preserved(self) -> None:
        if self.event is None:
            return
        segments = self.catalog.select_event(self.event)
        before = len(self.event.preserved)
        self.event.preserved.update(segment.path for segment in segments)
        added = len(self.event.preserved) - before
        if added:
            self.log(f"segmentos preservados durante evento: +{added}")

    def tick(self) -> None:
        if self.state is not RecordingState.IDLE:
            self.update_preserved()
        preserved = self.event.preserved if self.event is not None else set()
        self.catalog.retain(preserved, self.log)

        if self.state is not RecordingState.POST_EVENT or self.event is None:
            return
        deadline = self.event.post_deadline_monotonic
        assert deadline is not None
        if self.monotonic() < deadline + self.config.finalization_margin_seconds:
            return
        finalized = self.catalog.scan()
        if not finalized or self.event.post_end_ms is None:
            return
        if finalized[-1].mtime < self.event.post_end_ms / 1000.0:
            return
        self.transition(RecordingState.FINALIZING, "pós-evento concluído")
        self.finalize()

    def run(self) -> None:
        try:
            while not self.stop.is_set():
                try:
                    line = self.events.get(timeout=0.05)
                except queue.Empty:
                    self.tick()
                    continue
                self.handle_line(line)
                self.tick()
        except BaseException as exc:
            self.error = exc
            self.log(f"falha do controlador: {exc}")

    def _run_command(self, command: list[str]) -> tuple[int, bytes, bytes]:
        process = subprocess.Popen(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            shell=False,
            start_new_session=True,
        )
        try:
            stdout, stderr = process.communicate(timeout=self.config.command_timeout_seconds)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            stdout, stderr = process.communicate()
            return 124, stdout, stderr
        return process.returncode, stdout, stderr

    def _metadata_path(self, clip_path: pathlib.Path) -> pathlib.Path:
        return clip_path.with_suffix(".json")

    def _write_metadata(self, clip_path: pathlib.Path, metadata: dict[str, Any]) -> None:
        target = self._metadata_path(clip_path)
        temporary = target.with_suffix(".json.tmp")
        temporary.write_text(
            json.dumps(metadata, indent=2, ensure_ascii=False) + "\n",
            encoding="utf-8",
        )
        temporary.replace(target)

    def finalize(self, shutdown: bool = False) -> bool:
        if self.event is None:
            self.state = RecordingState.IDLE
            return False
        event = self.event
        event.finalized_by_shutdown = shutdown
        self.update_preserved()
        segments = self.catalog.select_event(event)
        if not segments:
            self.log("finalização falhou: nenhum segmento")
            self._record_failure(event, [], "nenhum segmento")
            self.state = RecordingState.IDLE
            self.event = None
            return False

        event_time = dt.datetime.fromtimestamp(event.motion_on_ms / 1000.0).astimezone()
        day = self.config.recordings_dir / event_time.strftime("%Y-%m-%d")
        day.mkdir(parents=True, exist_ok=True)
        stamp = event_time.strftime("%Y%m%d_%H%M%S_%f")[:-3]
        clip_path = day / f"event_{stamp}_{event.event_id[:8]}.{self.config.container}"
        list_path = day / f".{event.event_id}.segments.txt"
        list_path.write_text(
            "".join(f"file '{segment.path.resolve()}'\n" for segment in segments),
            encoding="utf-8",
        )
        command = [
            self.ffmpeg,
            "-hide_banner",
            "-loglevel",
            "error",
            "-f",
            "concat",
            "-safe",
            "0",
            "-i",
            str(list_path),
            "-map",
            "0:v:0",
            "-c:v",
            "copy",
            "-y",
            str(clip_path),
        ]
        self.log("início da finalização: " + " ".join(command))
        returncode, _stdout, stderr = self._run_command(command)
        list_path.unlink(missing_ok=True)
        if returncode != 0:
            error = stderr.decode(errors="replace")[-2000:]
            self.log(f"remux falhou rc={returncode}: {error}")
            self._record_failure(event, segments, error, clip_path)
            return False

        probe_command = [
            self.ffprobe,
            "-v",
            "error",
            "-select_streams",
            "v:0",
            "-show_entries",
            "stream=codec_name:format=duration",
            "-of",
            "json",
            str(clip_path),
        ]
        probe_rc, probe_stdout, probe_stderr = self._run_command(probe_command)
        try:
            probe = json.loads(probe_stdout) if probe_rc == 0 else {}
            streams = probe.get("streams", [])
            duration = float(probe.get("format", {}).get("duration", 0))
            codec = streams[0]["codec_name"] if streams else None
        except (ValueError, KeyError, TypeError, json.JSONDecodeError):
            streams, duration, codec = [], 0.0, None
        if not streams or duration <= 0 or not clip_path.exists() or clip_path.stat().st_size <= 0:
            error = probe_stderr.decode(errors="replace") or "ffprobe não validou vídeo"
            self.log(f"ffprobe inválido: {error}")
            self._record_failure(event, segments, error, clip_path)
            return False

        metadata = self._base_metadata(event, segments, clip_path)
        metadata.update({"status": "completed", "codec": codec, "duration": duration})
        self._write_metadata(clip_path, metadata)
        self.log(f"evento final criado: {clip_path} codec={codec} duração={duration}")
        self.state = RecordingState.IDLE
        self.event = None
        return True

    def _base_metadata(
        self,
        event: Event,
        segments: list[Segment],
        clip_path: pathlib.Path | None,
    ) -> dict[str, Any]:
        return {
            "event_id": event.event_id,
            "detector": self.detector,
            "motion_on_ms": event.motion_on_ms,
            "motion_off_ms": event.motion_off_ms,
            "post_started_ms": event.post_started_ms,
            "post_started_monotonic": event.post_started_monotonic,
            "post_end_ms": event.post_end_ms,
            "segments": [str(segment.path) for segment in segments],
            "clip_path": str(clip_path) if clip_path else None,
            "created_at": dt.datetime.fromtimestamp(
                self.wall_time(), dt.timezone.utc
            ).isoformat(),
            "finalized_by_shutdown": event.finalized_by_shutdown,
            "errors": [],
        }

    def _record_failure(
        self,
        event: Event,
        segments: list[Segment],
        error: str,
        clip_path: pathlib.Path | None = None,
    ) -> None:
        event_time = dt.datetime.fromtimestamp(event.motion_on_ms / 1000.0).astimezone()
        day = self.config.recordings_dir / event_time.strftime("%Y-%m-%d")
        day.mkdir(parents=True, exist_ok=True)
        if clip_path is None:
            clip_path = day / f"failed_{event.event_id}.mkv"
        metadata = self._base_metadata(event, segments, clip_path)
        metadata.update({"status": "failed", "codec": None, "duration": None})
        metadata["errors"] = [error]
        self._write_metadata(clip_path, metadata)

    def shutdown(self, timeout: float = 10) -> None:
        self.stop.set()
        if self.thread.ident is not None:
            self.thread.join(timeout)
            if self.thread.is_alive():
                raise RecordingError("thread de gravação não finalizou")
        if self.state in {RecordingState.MOTION_ACTIVE, RecordingState.POST_EVENT}:
            assert self.event is not None
            self.transition(RecordingState.FINALIZING, "shutdown")
            self.finalize(shutdown=True)
        if self._log_file is not None:
            self._log_file.close()
            self._log_file = None
        if self.error is not None:
            raise RecordingError(f"controlador de gravação falhou: {self.error}")


def build_segmenter_command(
    config: RecordingConfig,
    ffmpeg: str,
    stream_url: str,
    start_number: int,
) -> list[str]:
    return [
        ffmpeg,
        "-hide_banner",
        "-loglevel",
        "error",
        "-rtsp_transport",
        "tcp",
        "-i",
        stream_url,
        "-an",
        "-map",
        "0:v:0",
        "-c:v",
        "copy",
        "-f",
        "segment",
        "-segment_format",
        "mpegts",
        "-segment_time",
        str(config.segment_duration_seconds),
        "-reset_timestamps",
        "1",
        "-segment_start_number",
        str(start_number),
        str(config.segments_dir / "segment_%012d.ts"),
    ]
