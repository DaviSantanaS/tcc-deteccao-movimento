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
from collections import deque
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
    max_fragment_seconds: float
    decode_timeout_seconds: float

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

        container = data.get("container", "mp4")
        if container != "mp4":
            raise RecordingError("recording.container suportado: mp4")

        segment_duration = positive("segment_duration_seconds", 1)
        max_fragment = positive("max_fragment_seconds", 90)
        if pre_event >= max_fragment:
            raise RecordingError(
                "recording.pre_event_seconds deve ser menor que max_fragment_seconds"
            )
        if segment_duration >= max_fragment:
            raise RecordingError(
                "recording.segment_duration_seconds deve ser menor que "
                "max_fragment_seconds"
            )

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
            segment_duration_seconds=segment_duration,
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
            max_fragment_seconds=max_fragment,
            decode_timeout_seconds=positive("decode_timeout_seconds", 180),
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
    first_sequence: int | None = None
    part_first_sequence: int | None = None
    part_index: int = 1
    generation: int = 0

    def __post_init__(self) -> None:
        if self.preserved is None:
            self.preserved = set()


@dataclass(frozen=True)
class MotionInput:
    sequence: int
    line: str


@dataclass(frozen=True)
class DeferredMotionEvent:
    kind: str
    timestamp_ms: int

    @property
    def line(self) -> str:
        return f"{self.kind} {self.timestamp_ms}"


@dataclass(frozen=True)
class FinalizationJob:
    job_id: str
    event_id: str
    generation: int
    part_index: int
    expected_first_sequence: int
    segments: tuple[Segment, ...]
    mode: str
    reason: str
    observed_input_sequence: int
    final_path: pathlib.Path
    temporary_path: pathlib.Path


@dataclass
class FinalizationResult:
    job: FinalizationJob
    segments: list[Segment]
    durations: dict[pathlib.Path, float]
    temporary_path: pathlib.Path | None = None
    validation: dict[str, Any] | None = None
    error: Exception | None = None


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
        self.session_first_sequence: int | None = None
        self.duration_cache: dict[pathlib.Path, float] = {}

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

    def begin_session(self, first_sequence: int) -> None:
        if first_sequence < 0:
            raise RecordingError("sequência inicial da sessão deve ser não negativa")
        self.session_first_sequence = first_sequence

    def current_session(self, include_active: bool = False) -> list[Segment]:
        if self.session_first_sequence is None:
            raise RecordingError("sessão do segmentador não foi registrada")
        return [
            segment
            for segment in self.scan(include_active=include_active)
            if segment.sequence >= self.session_first_sequence
        ]

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
        return [
            segment
            for segment in self.current_session()
            if segment.mtime >= cutoff
        ]

    def select_event(self, event: Event) -> list[Segment]:
        first_sequence = event.part_first_sequence
        if first_sequence is None:
            first_sequence = event.first_sequence
        if first_sequence is None:
            raise RecordingError("evento sem âncora de sequência")
        return [
            segment
            for segment in self.current_session()
            if segment.sequence >= first_sequence
        ]

    def cache_duration(self, segment: Segment, duration: float) -> None:
        if duration <= 0:
            raise RecordingError(
                f"duração inválida para segmento {segment.sequence}: {duration}"
            )
        self.duration_cache[segment.path] = duration

    def cached_duration(self, segment: Segment) -> float | None:
        return self.duration_cache.get(segment.path)

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
            total -= sum(segment.size for segment in removable)
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
                self.duration_cache.pop(segment.path, None)
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
        input_codec: str | None = None,
    ) -> None:
        self.config = config
        self.detector = detector
        self.ffmpeg = ffmpeg
        self.ffprobe = ffprobe
        self.monotonic = monotonic
        self.wall_time = wall_time
        self.input_codec = input_codec
        self.catalog = catalog or SegmentCatalog(config, monotonic)
        self.state = RecordingState.IDLE
        self.event: Event | None = None
        self.events: queue.Queue[MotionInput] = queue.Queue()
        self.deferred_events: deque[DeferredMotionEvent] = deque()
        self._input_lock = threading.Lock()
        self._input_sequence = 0
        self._processed_input_sequence = 0
        self.stop = threading.Event()
        self.shutdown_requested = threading.Event()
        self.thread = threading.Thread(target=self.run, name="recording-controller")
        self.worker_stop = threading.Event()
        self.jobs: queue.Queue[FinalizationJob | None] = queue.Queue(maxsize=1)
        self.results: queue.Queue[FinalizationResult] = queue.Queue()
        self.worker_thread = threading.Thread(
            target=self.run_worker,
            name="recording-finalizer",
        )
        self.pending_job: FinalizationJob | None = None
        self.retry_final_reason: str | None = None
        self.last_evaluated_sequence: int | None = None
        self._process_lock = threading.Lock()
        self._active_process: subprocess.Popen[bytes] | None = None
        self.error: Exception | None = None
        self.log_path = self.config.recordings_dir.parent / "logs" / "recording.log"
        self._log_file: Any = None

    def set_log_path(self, log_path: pathlib.Path) -> None:
        self.log_path = log_path

    def set_input_codec(self, codec: str) -> None:
        if codec not in {"h264", "hevc"}:
            raise RecordingError(f"codec de entrada não suportado: {codec}")
        self.input_codec = codec

    def start(self) -> None:
        self.catalog.prepare()
        self.log_path.parent.mkdir(parents=True, exist_ok=True)
        self._log_file = self.log_path.open("a", encoding="utf-8", buffering=1)
        self.worker_thread.start()
        self.thread.start()

    def enqueue(self, line: str) -> None:
        if not self.stop.is_set():
            with self._input_lock:
                self._input_sequence += 1
                self.events.put_nowait(MotionInput(self._input_sequence, line))

    def log(self, message: str) -> None:
        if self._log_file is not None:
            timestamp = dt.datetime.fromtimestamp(
                self.wall_time(), dt.timezone.utc
            ).isoformat()
            self._log_file.write(f"{timestamp} {message}\n")

    def transition(self, new_state: RecordingState, reason: str) -> None:
        self.log(f"estado {self.state.value} -> {new_state.value}: {reason}")
        self.state = new_state

    def handle_line(self, line: str, from_deferred: bool = False) -> None:
        parts = line.split()
        if len(parts) != 2 or parts[0] not in {"MOTION_ON", "MOTION_OFF"}:
            self.log(f"evento inválido ignorado: {line!r}")
            return
        try:
            timestamp_ms = int(parts[1])
        except ValueError:
            self.log(f"timestamp inválido ignorado: {line!r}")
            return

        kind = parts[0]
        if (
            self.state is RecordingState.FINALIZING
            and self.event is not None
            and (
                self.deferred_events
                or kind == "MOTION_OFF"
                or self.event.post_end_ms is None
                or timestamp_ms > self.event.post_end_ms
            )
        ):
            deferred = DeferredMotionEvent(kind, timestamp_ms)
            if from_deferred:
                self.deferred_events.appendleft(deferred)
            else:
                self.deferred_events.append(deferred)
            self.log(f"evento adiado durante FINALIZING: {deferred.line}")
            return

        if kind == "MOTION_ON":
            self.motion_on(timestamp_ms, from_deferred)
        else:
            self.motion_off(timestamp_ms)

    def motion_on(
        self,
        timestamp_ms: int,
        from_deferred: bool = False,
    ) -> None:
        if self.state is RecordingState.IDLE:
            selected = self.catalog.select_pre_event(timestamp_ms)
            if not selected:
                raise RecordingError(
                    "nenhum segmento da sessão atual disponível para o pré-evento"
                )
            self.event = Event(uuid.uuid4().hex, timestamp_ms)
            self.event.first_sequence = selected[0].sequence
            self.event.part_first_sequence = selected[0].sequence
            self.event.preserved.update(item.path for item in selected)
            self.log(f"segmentos pré-evento preservados: {len(selected)}")
            self.transition(RecordingState.MOTION_ACTIVE, "MOTION_ON")
            return
        if self.state is RecordingState.MOTION_ACTIVE:
            self.log("MOTION_ON duplicado ignorado")
            return
        if self.state is RecordingState.POST_EVENT:
            if self.event is not None:
                if (
                    self.event.post_end_ms is not None
                    and timestamp_ms > self.event.post_end_ms
                ):
                    deferred = DeferredMotionEvent("MOTION_ON", timestamp_ms)
                    if from_deferred:
                        self.deferred_events.appendleft(deferred)
                    else:
                        self.deferred_events.append(deferred)
                    self.transition(
                        RecordingState.FINALIZING,
                        "MOTION_ON posterior ao pós-evento",
                    )
                    self.schedule_job("final", "motion_end")
                    return
                self.event.generation += 1
                self.event.motion_off_ms = None
                self.event.post_started_ms = None
                self.event.post_started_monotonic = None
                self.event.post_deadline_monotonic = None
                self.event.post_end_ms = None
                self.retry_final_reason = None
            self.transition(RecordingState.MOTION_ACTIVE, "movimento retornou")
            return
        if self.state is RecordingState.FINALIZING and self.event is not None:
            if (
                self.event.post_end_ms is not None
                and timestamp_ms <= self.event.post_end_ms
            ):
                self.event.generation += 1
                self.event.motion_off_ms = None
                self.event.post_started_ms = None
                self.event.post_started_monotonic = None
                self.event.post_deadline_monotonic = None
                self.event.post_end_ms = None
                self.retry_final_reason = None
                self.transition(RecordingState.MOTION_ACTIVE, "movimento retornou")
                return
        self.log("MOTION_ON ignorado durante FINALIZING")

    def replay_deferred_events(self) -> None:
        while self.deferred_events and self.state is not RecordingState.FINALIZING:
            deferred = self.deferred_events.popleft()
            self.log(f"reproduzindo evento adiado: {deferred.line}")
            self.handle_line(deferred.line, from_deferred=True)

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

        if self.event is None or self.pending_job is not None:
            return
        if self.state is RecordingState.FINALIZING:
            with self._input_lock:
                has_unprocessed_input = (
                    self._input_sequence > self._processed_input_sequence
                )
            if has_unprocessed_input:
                return
            reason = self.retry_final_reason or (
                "shutdown" if self.shutdown_requested.is_set() else "motion_end"
            )
            self.retry_final_reason = None
            self.schedule_job("final", reason)
            return
        if self.state is RecordingState.POST_EVENT:
            deadline = self.event.post_deadline_monotonic
            assert deadline is not None
            if self.monotonic() >= deadline + self.config.finalization_margin_seconds:
                finalized = self.catalog.current_session()
                if (
                    finalized
                    and self.event.post_end_ms is not None
                    and finalized[-1].mtime >= self.event.post_end_ms / 1000.0
                ):
                    self.transition(RecordingState.FINALIZING, "pós-evento concluído")
                    self.schedule_job("final", "motion_end")
                    return
        if self.state in {RecordingState.MOTION_ACTIVE, RecordingState.POST_EVENT}:
            segments = self.catalog.select_event(self.event)
            if (
                segments
                and segments[-1].sequence != self.last_evaluated_sequence
            ):
                self.schedule_job("evaluate", "max_duration")

    def run(self) -> None:
        try:
            while True:
                if (
                    self.worker_thread.ident is not None
                    and not self.worker_thread.is_alive()
                    and not self.worker_stop.is_set()
                ):
                    raise RecordingError(
                        "worker de gravação encerrou inesperadamente"
                    )
                processed_event = False
                try:
                    motion_input = self.events.get(timeout=0.02)
                except queue.Empty:
                    pass
                else:
                    self._processed_input_sequence = motion_input.sequence
                    self.handle_line(motion_input.line)
                    processed_event = True
                while True:
                    try:
                        motion_input = self.events.get_nowait()
                    except queue.Empty:
                        break
                    self._processed_input_sequence = motion_input.sequence
                    self.handle_line(motion_input.line)
                    processed_event = True
                self.apply_results()
                if self.error is not None:
                    break
                if self.shutdown_requested.is_set():
                    if self.error is not None:
                        break
                    if self.event is not None and self.pending_job is None:
                        self.event.finalized_by_shutdown = True
                        self.transition(RecordingState.FINALIZING, "shutdown")
                        self.schedule_job("final", "shutdown")
                    if (
                        self.event is None
                        and self.pending_job is None
                        and not self.deferred_events
                        and self.events.empty()
                    ):
                        break
                    continue
                self.tick()
                if self.stop.is_set() and not processed_event:
                    break
        except Exception as exc:
            self.error = exc
            self.log(f"falha do controlador: {exc}")

    def _run_command(
        self,
        command: list[str],
        timeout: float | None = None,
    ) -> tuple[int, bytes, bytes]:
        process = subprocess.Popen(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            shell=False,
            start_new_session=True,
        )
        with self._process_lock:
            self._active_process = process
        try:
            stdout, stderr = process.communicate(
                timeout=timeout or self.config.command_timeout_seconds
            )
        except subprocess.TimeoutExpired:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            stdout, stderr = process.communicate()
            return 124, stdout, stderr
        finally:
            with self._process_lock:
                if self._active_process is process:
                    self._active_process = None
        return process.returncode, stdout, stderr

    def _metadata_path(self, clip_path: pathlib.Path) -> pathlib.Path:
        return clip_path.with_suffix(".json")

    def _write_metadata(
        self,
        target: pathlib.Path,
        metadata: dict[str, Any],
    ) -> None:
        target.write_text(
            json.dumps(metadata, indent=2, ensure_ascii=False) + "\n",
            encoding="utf-8",
        )

    def _segment_duration(self, segment: Segment) -> float:
        cached = self.catalog.cached_duration(segment)
        if cached is not None:
            return cached
        command = [
            self.ffprobe,
            "-v",
            "error",
            "-select_streams",
            "v:0",
            "-show_entries",
            "stream=codec_type:format=duration",
            "-of",
            "json",
            str(segment.path),
        ]
        returncode, stdout, stderr = self._run_command(command)
        try:
            data = json.loads(stdout) if returncode == 0 else {}
            streams = data.get("streams", [])
            duration = float(data.get("format", {}).get("duration", 0))
        except (TypeError, ValueError, json.JSONDecodeError):
            streams = []
            duration = 0.0
        if not streams or duration <= 0:
            detail = stderr.decode(errors="replace")[-2000:]
            raise RecordingError(
                f"não foi possível medir o segmento {segment.sequence}: {detail}"
            )
        return duration

    def _prefix_within_limit(
        self,
        segments: list[Segment],
        durations: dict[pathlib.Path, float] | None = None,
    ) -> list[Segment]:
        selected: list[Segment] = []
        total = 0.0
        for segment in segments:
            duration = (
                durations[segment.path]
                if durations is not None
                else self._segment_duration(segment)
            )
            if total + duration > self.config.max_fragment_seconds:
                break
            selected.append(segment)
            total += duration
        return selected

    def schedule_job(self, mode: str, reason: str) -> None:
        if self.event is None or self.pending_job is not None:
            return
        segments = self.catalog.select_event(self.event)
        if not segments:
            if mode == "evaluate":
                return
            error = "nenhum segmento disponível para finalização"
            self._record_failure(self.event, [], error, reason)
            raise RecordingError(error)
        expected = self.event.part_first_sequence
        if expected is None:
            raise RecordingError("evento sem âncora da parte atual")
        job = FinalizationJob(
            job_id=uuid.uuid4().hex,
            event_id=self.event.event_id,
            generation=self.event.generation,
            part_index=self.event.part_index,
            expected_first_sequence=expected,
            segments=tuple(segments),
            mode=mode,
            reason=reason,
            observed_input_sequence=self._processed_input_sequence,
            final_path=self._event_paths(
                self.event,
                self.event.part_index,
            )[0],
            temporary_path=self._event_paths(
                self.event,
                self.event.part_index,
            )[1],
        )
        self.pending_job = job
        self.jobs.put_nowait(job)

    def run_worker(self) -> None:
        while not self.worker_stop.is_set():
            try:
                job = self.jobs.get(timeout=0.05)
            except queue.Empty:
                continue
            if job is None:
                return
            try:
                result = self.prepare_job(job)
            except Exception as exc:
                self.cleanup_temporary(job.temporary_path, "worker")
                result = FinalizationResult(
                    job,
                    [],
                    {},
                    temporary_path=job.temporary_path,
                    error=exc,
                )
            self.results.put(result)

    def cleanup_temporary(
        self,
        path: pathlib.Path,
        context: str,
    ) -> tuple[bool, str | None]:
        failure: str | None = None
        try:
            path.unlink(missing_ok=True)
        except OSError as exc:
            failure = str(exc)
            self.log(f"falha ao limpar temporário ({context}): {exc}")
        try:
            absent = not path.exists()
        except OSError as exc:
            absent = False
            detail = str(exc)
            failure = f"{failure}; {detail}" if failure else detail
            self.log(f"falha ao verificar cleanup ({context}): {exc}")
        if not absent and failure is None:
            failure = "arquivo permaneceu após unlink"
            self.log(f"falha ao limpar temporário ({context}): {failure}")
        return absent, failure

    def prepare_job(self, job: FinalizationJob) -> FinalizationResult:
        segments = list(job.segments)
        if not segments:
            raise RecordingError("lista de segmentos vazia")
        if segments[0].sequence != job.expected_first_sequence:
            raise RecordingError(
                "fronteira inválida: "
                f"esperada={job.expected_first_sequence}, "
                f"encontrada={segments[0].sequence}, "
                f"event_id={job.event_id}, part_index={job.part_index}"
            )
        if any(
            current.sequence != previous.sequence + 1
            for previous, current in zip(segments, segments[1:])
        ):
            raise RecordingError("lacuna interna na sequência de segmentos")
        durations = {
            segment.path: self._segment_duration(segment)
            for segment in segments
        }
        total = sum(durations.values())
        if job.mode == "evaluate" and total <= self.config.max_fragment_seconds:
            return FinalizationResult(job, [], durations)
        candidate = self._prefix_within_limit(segments, durations)
        if not candidate:
            raise RecordingError(
                "um único segmento excede max_fragment_seconds; "
                "espaçamento de keyframes incompatível"
            )
        return self.prepare_part(job, candidate, durations)

    def apply_results(self) -> None:
        while True:
            try:
                result = self.results.get_nowait()
            except queue.Empty:
                return
            if self.pending_job is None or result.job.job_id != self.pending_job.job_id:
                if result.temporary_path is not None:
                    self.cleanup_temporary(result.temporary_path, "job desconhecido")
                continue
            self.pending_job = None
            if self.result_is_obsolete(result):
                if result.temporary_path is not None:
                    self.cleanup_temporary(result.temporary_path, "job obsoleto")
                self.log(
                    "resultado obsoleto descartado, inclusive erro: "
                    f"job={result.job.job_id}"
                )
                continue
            for path, duration in result.durations.items():
                segment = next(
                    (item for item in result.job.segments if item.path == path),
                    None,
                )
                if segment is not None:
                    self.catalog.cache_duration(segment, duration)
            if result.error is not None:
                self.handle_job_error(result)
                return
            if not result.segments:
                self.last_evaluated_sequence = result.job.segments[-1].sequence
                if self.state is RecordingState.FINALIZING:
                    self.schedule_job("final", result.job.reason)
                continue
            assert self.event is not None
            is_final = (
                result.job.mode == "final"
                and result.job.generation == self.event.generation
                and len(result.segments) == len(result.job.segments)
            )
            reason = result.job.reason if is_final else "max_duration"
            published = self.publish_result(result, reason, is_final)
            if not published:
                if self.error is None:
                    self.retry_final_reason = result.job.reason
                continue
            if is_final:
                self.retry_final_reason = None
                self.replay_deferred_events()
            elif self.state is RecordingState.FINALIZING:
                self.schedule_job("final", result.job.reason)

    def result_is_obsolete(self, result: FinalizationResult) -> bool:
        if self.event is None:
            return True
        job = result.job
        if (
            self.event.event_id != job.event_id
            or self.event.part_index != job.part_index
            or self.event.part_first_sequence != job.expected_first_sequence
        ):
            return True
        return job.mode == "final" and self.event.generation != job.generation

    def handle_job_error(self, result: FinalizationResult) -> None:
        if result.temporary_path is not None:
            self.cleanup_temporary(
                result.temporary_path,
                "resultado com erro",
            )
        error = result.error
        assert error is not None
        if self.event is not None and self.event.event_id == result.job.event_id:
            try:
                self._record_failure(
                    self.event,
                    result.segments or list(result.job.segments),
                    str(error),
                    result.job.reason,
                )
            except OSError as metadata_error:
                error = RecordingError(
                    f"{error}; falha ao registrar metadata: {metadata_error}"
                )
        self.error = (
            error
            if isinstance(error, RecordingError)
            else RecordingError(f"worker de gravação falhou: {error}")
        )
        self.log(f"falha do worker: {self.error}")

    def publish_result(
        self,
        result: FinalizationResult,
        reason: str,
        is_final: bool,
    ) -> bool:
        assert self.event is not None
        assert result.temporary_path is not None
        assert result.validation is not None
        job = result.job
        metadata = self._base_metadata(
            self.event,
            result.segments,
            job.final_path,
            reason,
            is_final,
        )
        metadata.update({"status": "completed", **result.validation})
        metadata_path = self._metadata_path(job.final_path)
        metadata_temporary = metadata_path.with_suffix(".json.tmp")
        try:
            self._write_metadata(metadata_temporary, metadata)
            if is_final:
                with self._input_lock:
                    sequence_before_rename = self._input_sequence
                if sequence_before_rename > job.observed_input_sequence:
                    self.cleanup_temporary(
                        result.temporary_path,
                        "commit final adiado",
                    )
                    self.cleanup_temporary(
                        metadata_temporary,
                        "metadata final adiada",
                    )
                    self.log(
                        "commit final adiado por evento MotionBus mais novo: "
                        f"job={job.job_id}"
                    )
                    return False
                result.temporary_path.replace(job.final_path)
                metadata_temporary.replace(metadata_path)
                if not self._commit_published_result(
                    result,
                    is_final=True,
                    expected_input_sequence=sequence_before_rename,
                ):
                    rollback_errors = self._rollback_published_files(
                        job.final_path,
                        metadata_path,
                    )
                    if rollback_errors:
                        detail = "; ".join(rollback_errors)
                        self.error = RecordingError(
                            "rollback concorrente incompleto; artefatos "
                            f"publicados permaneceram: {detail}"
                        )
                        self.log(f"falha fatal no rollback concorrente: {detail}")
                    else:
                        self.log(
                            "commit final revertido por evento MotionBus durante "
                            f"os renames: job={job.job_id}"
                        )
                    return False
            else:
                result.temporary_path.replace(job.final_path)
                metadata_temporary.replace(metadata_path)
                self._commit_published_result(
                    result,
                    is_final=False,
                    expected_input_sequence=None,
                )
        except OSError as exc:
            cleanup_errors = self._rollback_paths(
                (
                    (result.temporary_path, "rollback MP4 temporário"),
                    (metadata_temporary, "rollback JSON temporário"),
                    (job.final_path, "rollback MP4 final"),
                    (metadata_path, "rollback JSON final"),
                )
            )
            cleanup_detail = (
                f"; falhas de cleanup: {'; '.join(cleanup_errors)}"
                if cleanup_errors
                else ""
            )
            raise RecordingError(
                f"falha na publicação atômica da parte {job.part_index}: "
                f"{exc}{cleanup_detail}"
            ) from exc
        self.log(
            f"parte criada: {job.final_path} "
            f"codec={result.validation['codec']} "
            f"duração={result.validation['duration']}"
        )
        return True

    def _commit_published_result(
        self,
        result: FinalizationResult,
        is_final: bool,
        expected_input_sequence: int | None,
    ) -> bool:
        with self._input_lock:
            if (
                is_final
                and self._input_sequence != expected_input_sequence
            ):
                return False
            assert self.event is not None
            consumed = {segment.path for segment in result.segments}
            self.event.preserved.difference_update(consumed)
            self.event.part_first_sequence = result.segments[-1].sequence + 1
            self.event.part_index += 1
            self.last_evaluated_sequence = None
            if is_final:
                self.retry_final_reason = None
                self.state = RecordingState.IDLE
                self.event = None
            return True

    def _rollback_paths(
        self,
        paths: tuple[tuple[pathlib.Path, str], ...],
    ) -> list[str]:
        errors: list[str] = []
        for path, context in paths:
            absent, failure = self.cleanup_temporary(path, context)
            if not absent:
                errors.append(f"{path}: {failure or 'artefato permaneceu'}")
        return errors

    def _rollback_published_files(
        self,
        mp4_path: pathlib.Path,
        metadata_path: pathlib.Path,
    ) -> list[str]:
        return self._rollback_paths(
            (
                (mp4_path, "rollback MP4 final concorrente"),
                (metadata_path, "rollback JSON final concorrente"),
            )
        )

    def _event_paths(
        self,
        event: Event,
        part_index: int,
    ) -> tuple[pathlib.Path, pathlib.Path]:
        event_time = dt.datetime.fromtimestamp(event.motion_on_ms / 1000.0).astimezone()
        day = self.config.recordings_dir / event_time.strftime("%Y-%m-%d")
        day.mkdir(parents=True, exist_ok=True)
        stamp = event_time.strftime("%Y%m%d_%H%M%S_%f")[:-3]
        stem = f"event_{stamp}_{event.event_id[:8]}_part_{part_index:04d}"
        final_path = day / f"{stem}.mp4"
        temporary_path = day / f".{stem}.mp4.part"
        return final_path, temporary_path

    def _probe_json(self, command: list[str], label: str) -> dict[str, Any]:
        returncode, stdout, stderr = self._run_command(command)
        if returncode != 0:
            detail = stderr.decode(errors="replace")[-2000:]
            raise RecordingError(f"{label} falhou (rc={returncode}): {detail}")
        try:
            value = json.loads(stdout)
        except json.JSONDecodeError as exc:
            raise RecordingError(f"{label} retornou JSON inválido") from exc
        if not isinstance(value, dict):
            raise RecordingError(f"{label} retornou estrutura inválida")
        return value

    def _validate_mp4(self, path: pathlib.Path) -> dict[str, Any]:
        if self.input_codec not in {"h264", "hevc"}:
            raise RecordingError("codec de entrada não foi validado")
        if not path.exists() or path.stat().st_size <= 0:
            raise RecordingError("arquivo MP4 temporário ausente ou vazio")

        media = self._probe_json(
            [
                self.ffprobe,
                "-v",
                "error",
                "-select_streams",
                "v:0",
                "-show_entries",
                "stream=codec_name:format=format_name,duration",
                "-of",
                "json",
                str(path),
            ],
            "ffprobe do MP4",
        )
        streams = media.get("streams")
        if not isinstance(streams, list) or len(streams) != 1:
            raise RecordingError("MP4 deve possuir exatamente uma stream de vídeo")
        codec = streams[0].get("codec_name")
        if codec not in {"h264", "hevc"}:
            raise RecordingError(f"codec MP4 não suportado: {codec}")
        if codec != self.input_codec:
            raise RecordingError(
                f"codec do MP4 ({codec}) difere do RTSP ({self.input_codec})"
            )
        format_name = media.get("format", {}).get("format_name", "")
        formats = set(str(format_name).split(","))
        if not formats.intersection({"mov", "mp4", "m4a", "3gp", "3g2", "mj2"}):
            raise RecordingError(f"contêiner MP4/MOV inválido: {format_name}")
        try:
            duration = float(media.get("format", {}).get("duration", 0))
        except (TypeError, ValueError) as exc:
            raise RecordingError("duração inválida no MP4") from exc
        if duration <= 0:
            raise RecordingError("duração do MP4 deve ser maior que zero")

        packets = self._probe_json(
            [
                self.ffprobe,
                "-v",
                "error",
                "-select_streams",
                "v:0",
                "-read_intervals",
                "%+#1",
                "-show_packets",
                "-show_entries",
                "packet=flags",
                "-of",
                "json",
                str(path),
            ],
            "ffprobe do primeiro pacote",
        ).get("packets", [])
        if not packets or "K" not in str(packets[0].get("flags", "")):
            raise RecordingError("primeiro pacote de vídeo não é keyframe")

        frames = self._probe_json(
            [
                self.ffprobe,
                "-v",
                "error",
                "-select_streams",
                "v:0",
                "-read_intervals",
                "%+#1",
                "-show_frames",
                "-show_entries",
                "frame=key_frame,pict_type",
                "-of",
                "json",
                str(path),
            ],
            "ffprobe do primeiro frame",
        ).get("frames", [])
        if not frames or frames[0].get("key_frame") != 1:
            raise RecordingError("primeiro frame decodificado não é keyframe")
        pict_type = frames[0].get("pict_type")
        if pict_type is not None and pict_type != "I":
            raise RecordingError(f"primeiro frame possui pict_type={pict_type}, esperado I")

        decode_rc, _stdout, decode_stderr = self._run_command(
            [
                self.ffmpeg,
                "-v",
                "error",
                "-i",
                str(path),
                "-map",
                "0:v:0",
                "-f",
                "null",
                "-",
            ],
            timeout=self.config.decode_timeout_seconds,
        )
        if decode_rc == 124:
            raise RecordingError(
                "timeout na decodificação integral do MP4 "
                f"após {self.config.decode_timeout_seconds}s"
            )
        if decode_rc != 0 or decode_stderr.strip():
            detail = decode_stderr.decode(errors="replace")[-2000:]
            raise RecordingError(
                f"decodificação integral do MP4 falhou (rc={decode_rc}): {detail}"
            )
        return {
            "codec": codec,
            "duration": duration,
            "starts_with_keyframe": True,
            "first_frame_pict_type": pict_type,
        }

    def prepare_part(
        self,
        job: FinalizationJob,
        segments: list[Segment],
        durations: dict[pathlib.Path, float],
    ) -> FinalizationResult:
        candidate = list(segments)
        while candidate:
            temporary_path = job.temporary_path
            completed = False
            concat_input = "concat:" + "|".join(
                str(segment.path.resolve()) for segment in candidate
            )
            command = [
                self.ffmpeg,
                "-hide_banner",
                "-loglevel",
                "error",
                "-i",
                concat_input,
                "-map",
                "0:v:0",
                "-c:v",
                "copy",
                "-movflags",
                "+faststart",
                "-f",
                "mp4",
                "-y",
                str(temporary_path),
            ]
            self.log("início da finalização da parte: " + " ".join(command))
            try:
                returncode, _stdout, stderr = self._run_command(command)
                if returncode != 0:
                    detail = stderr.decode(errors="replace")[-2000:]
                    raise RecordingError(
                        f"remux falhou (rc={returncode}): {detail}"
                    )
                validation = self._validate_mp4(temporary_path)
                if validation["duration"] > self.config.max_fragment_seconds:
                    if len(candidate) == 1:
                        raise RecordingError(
                            "um único segmento produz MP4 acima de "
                            "max_fragment_seconds; espaçamento de keyframes "
                            "incompatível"
                        )
                    temporary_path.unlink(missing_ok=True)
                    candidate.pop()
                    continue
                result = FinalizationResult(
                    job=job,
                    segments=candidate,
                    durations=durations,
                    temporary_path=temporary_path,
                    validation=validation,
                )
                completed = True
                return result
            finally:
                if not completed:
                    self.cleanup_temporary(temporary_path, "prepare_part")
        raise RecordingError("nenhum segmento coube no fragmento")

    def _base_metadata(
        self,
        event: Event,
        segments: list[Segment],
        clip_path: pathlib.Path | None,
        reason: str,
        is_final: bool,
    ) -> dict[str, Any]:
        return {
            "event_id": event.event_id,
            "part_index": event.part_index,
            "part_count_known": event.part_index if is_final else None,
            "is_first_part": event.part_index == 1,
            "is_final_part": is_final,
            "fragment_reason": reason,
            "detector": self.detector,
            "motion_on_ms": event.motion_on_ms,
            "motion_off_ms": event.motion_off_ms,
            "post_end_ms": event.post_end_ms,
            "first_sequence": segments[0].sequence if segments else event.part_first_sequence,
            "last_sequence": segments[-1].sequence if segments else None,
            "segment_count": len(segments),
            "segments": [str(segment.path) for segment in segments],
            "clip_path": str(clip_path) if clip_path else None,
            "configured_max_fragment_seconds": self.config.max_fragment_seconds,
            "container": "mp4",
            "input_codec": self.input_codec,
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
        reason: str,
    ) -> None:
        clip_path, _temporary_path = self._event_paths(
            event, event.part_index
        )
        metadata = self._base_metadata(event, segments, clip_path, reason, False)
        metadata.update(
            {
                "status": "failed",
                "codec": None,
                "duration": None,
                "starts_with_keyframe": False,
                "first_frame_pict_type": None,
            }
        )
        metadata["errors"] = [error]
        metadata_path = self._metadata_path(clip_path)
        temporary = metadata_path.with_suffix(".json.tmp")
        try:
            self._write_metadata(temporary, metadata)
            temporary.replace(metadata_path)
        except OSError:
            temporary.unlink(missing_ok=True)
            raise

    def shutdown(self, timeout: float = 10) -> None:
        self.shutdown_requested.set()
        try:
            if self.thread.ident is not None:
                self.thread.join(timeout)
                if self.thread.is_alive():
                    self.cancel_active_process()
                    self.thread.join(2)
                    if self.thread.is_alive():
                        raise RecordingError("thread de gravação não finalizou")
        finally:
            self.worker_stop.set()
            try:
                self.jobs.put_nowait(None)
            except queue.Full:
                pass
            if self.worker_thread.ident is not None:
                self.worker_thread.join(2)
                if self.worker_thread.is_alive():
                    self.cancel_active_process()
                    self.worker_thread.join(2)
            if self._log_file is not None:
                self._log_file.close()
                self._log_file = None
        if self.worker_thread.ident is not None and self.worker_thread.is_alive():
            raise RecordingError("worker de gravação não finalizou")
        if self.error is not None:
            raise RecordingError(f"controlador de gravação falhou: {self.error}")

    def cancel_active_process(self) -> None:
        with self._process_lock:
            process = self._active_process
        if process is None or process.poll() is not None:
            return
        try:
            os.killpg(process.pid, signal.SIGTERM)
            process.wait(timeout=1)
        except (ProcessLookupError, subprocess.TimeoutExpired):
            if process.poll() is None:
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass


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
        "-break_non_keyframes",
        "0",
        "-reset_timestamps",
        "0",
        "-segment_start_number",
        str(start_number),
        str(config.segments_dir / "segment_%012d.ts"),
    ]
