import json
import os
import pathlib
import signal
import tempfile
import threading
import time
import unittest
from unittest import mock

import orchestrator
from recording import (
    Event,
    RecordingConfig,
    RecordingController,
    RecordingError,
    RecordingState,
    Segment,
    SegmentCatalog,
    build_segmenter_command,
)


class Clock:
    def __init__(self, value: float = 100.0) -> None:
        self.value = value

    def __call__(self) -> float:
        return self.value

    def advance(self, seconds: float) -> None:
        self.value += seconds


def recording_config(root: pathlib.Path, **overrides: object) -> RecordingConfig:
    data = {
        "enabled": True,
        "segments_dir": str(root / "segments"),
        "recordings_dir": str(root / "recordings"),
        "segment_duration_seconds": 1,
        "pre_event_seconds": 5,
        "post_event_seconds": 5,
        "idle_retention_seconds": 10,
        "finalization_margin_seconds": 2,
        "container": "mp4",
        "max_fragment_seconds": 90,
    }
    data.update(overrides)
    return RecordingConfig.load(data, root)


class FakeCatalog:
    def __init__(self, segments: list[Segment] | None = None) -> None:
        self.segments = segments or []
        self.retained: list[set[pathlib.Path]] = []
        self.durations: dict[pathlib.Path, float] = {
            segment.path: 1.0 for segment in self.segments
        }

    def prepare(self) -> None:
        pass

    def select_pre_event(self, _timestamp: int) -> list[Segment]:
        return list(self.segments)

    def select_event(self, _event: Event) -> list[Segment]:
        first = _event.part_first_sequence
        if first is None:
            first = _event.first_sequence
        return [
            segment
            for segment in self.segments
            if first is not None and segment.sequence >= first
        ]

    def scan(self, include_active: bool = False) -> list[Segment]:
        return list(self.segments)

    def current_session(self, include_active: bool = False) -> list[Segment]:
        return list(self.segments)

    def retain(self, preserved: set[pathlib.Path], _log: object) -> None:
        self.retained.append(set(preserved))

    def cached_duration(self, segment: Segment) -> float | None:
        return self.durations.get(segment.path, 1.0)

    def cache_duration(self, segment: Segment, duration: float) -> None:
        self.durations[segment.path] = duration


class ValidationRunner:
    def __init__(
        self,
        codec: str = "hevc",
        duration_per_segment: float = 1.0,
        packet_flags: str = "K_",
        key_frame: int = 1,
        pict_type: str | None = "I",
        format_name: str = "mov,mp4,m4a,3gp,3g2,mj2",
        decode_rc: int = 0,
        decode_stderr: bytes = b"",
        stream_count: int = 1,
    ) -> None:
        self.codec = codec
        self.duration_per_segment = duration_per_segment
        self.packet_flags = packet_flags
        self.key_frame = key_frame
        self.pict_type = pict_type
        self.format_name = format_name
        self.decode_rc = decode_rc
        self.decode_stderr = decode_stderr
        self.stream_count = stream_count
        self.current_duration = duration_per_segment
        self.commands: list[list[str]] = []
        self.final_existed_during_validation = False

    def __call__(self, command: list[str]) -> tuple[int, bytes, bytes]:
        self.commands.append(command)
        if command[0] == "ffmpeg" and command[-1] == "-":
            return self.decode_rc, b"", self.decode_stderr
        if command[0] == "ffmpeg":
            concat_input = command[command.index("-i") + 1]
            count = len(concat_input.removeprefix("concat:").split("|"))
            self.current_duration = count * self.duration_per_segment
            temporary = pathlib.Path(command[-1])
            temporary.write_bytes(b"valid mp4")
            final = temporary.with_name(temporary.name[1:-5])
            self.final_existed_during_validation = final.exists()
            return 0, b"", b""
        if "-show_packets" in command:
            payload = {"packets": [{"flags": self.packet_flags}]}
            return 0, json.dumps(payload).encode(), b""
        if "-show_frames" in command:
            frame: dict[str, object] = {"key_frame": self.key_frame}
            if self.pict_type is not None:
                frame["pict_type"] = self.pict_type
            return 0, json.dumps({"frames": [frame]}).encode(), b""
        payload = {
            "streams": [
                {"codec_name": self.codec}
                for _index in range(self.stream_count)
            ],
            "format": {
                "duration": str(self.current_duration),
                "format_name": self.format_name,
            },
        }
        return 0, json.dumps(payload).encode(), b""


class RecordingConfigurationTests(unittest.TestCase):
    def test_valid_configuration(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            config = recording_config(root)
            self.assertTrue(config.enabled)
            self.assertEqual(config.pre_event_seconds, 5)
            self.assertEqual(config.container, "mp4")
            self.assertEqual(config.max_fragment_seconds, 90)

    def test_invalid_configuration(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            with self.assertRaises(RecordingError):
                recording_config(
                    root,
                    idle_retention_seconds=4,
                    pre_event_seconds=5,
                )

    def test_disabled_configuration(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            config = RecordingConfig.load({"enabled": False}, root)
            self.assertFalse(config.enabled)

    def test_default_container_and_fragment_duration(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            config = RecordingConfig.load(
                {"enabled": False},
                pathlib.Path(temporary),
            )
            self.assertEqual(config.container, "mp4")
            self.assertEqual(config.max_fragment_seconds, 90)

    def test_mkv_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            with self.assertRaisesRegex(RecordingError, "mp4"):
                recording_config(pathlib.Path(temporary), container="mkv")

    def test_invalid_fragment_durations_are_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            for value in (0, -1):
                with self.subTest(value=value):
                    with self.assertRaises(RecordingError):
                        recording_config(root, max_fragment_seconds=value)
            with self.assertRaisesRegex(RecordingError, "pre_event_seconds"):
                recording_config(
                    root,
                    pre_event_seconds=10,
                    max_fragment_seconds=10,
                )
            with self.assertRaisesRegex(RecordingError, "segment_duration_seconds"):
                recording_config(
                    root,
                    pre_event_seconds=1,
                    segment_duration_seconds=10,
                    max_fragment_seconds=10,
                )


class FifoForwardingTests(unittest.TestCase):
    def test_fifo_forwards_complete_line(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            received: list[str] = []
            stop = threading.Event()
            reader = orchestrator.FIFO(
                root / "motion_bus",
                stop,
                root / "motion_bus.log",
                0.01,
                received.append,
            )
            reader.start(1)
            descriptor = os.open(root / "motion_bus", os.O_WRONLY | os.O_NONBLOCK)
            os.write(descriptor, b"MOTION_ON ")
            os.write(descriptor, b"123\n")
            os.close(descriptor)
            deadline = time.monotonic() + 1
            while not received and time.monotonic() < deadline:
                time.sleep(0.01)
            stop.set()
            reader.join(1)
            self.assertEqual(received, ["MOTION_ON 123"])

    def test_supervisor_keeps_fifo_as_single_reader(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            process = {"argv": ["/bin/true"], "cwd": str(root)}
            config = {
                "stream_url": "rtsp://example/video",
                "logs_dir": str(root / "logs"),
                "fifo": {"path": str(root / "motion_bus")},
                "ffprobe": {"executable": "/bin/true"},
                "mediamtx": dict(process),
                "publisher": dict(process),
                "detectors": {"diff": dict(process)},
                "recording": {
                    "enabled": True,
                    "segments_dir": str(root / "segments"),
                    "recordings_dir": str(root / "recordings"),
                    "pre_event_seconds": 1,
                    "idle_retention_seconds": 1,
                },
            }
            supervisor = orchestrator.Supervisor(config, root, "diff")
            self.assertIsNotNone(supervisor.fifo.sink)
            self.assertIsNotNone(supervisor.recording)
            self.assertEqual(supervisor.fifo.path, root / "motion_bus")


class StateMachineTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.temporary.name)
        self.clock = Clock()
        self.segment_path = self.root / "segment_000000000001.ts"
        self.segment_path.write_bytes(b"segment")
        self.segment = Segment(1, self.segment_path, 1000.0, 7)
        self.catalog = FakeCatalog([self.segment])
        self.controller = RecordingController(
            recording_config(self.root),
            "diff",
            "ffmpeg",
            "ffprobe",
            self.catalog,
            self.clock,
            lambda: 1000.0,
            "hevc",
        )

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def test_idle_to_motion_active(self) -> None:
        self.controller.motion_on(1_000_000)
        self.assertEqual(self.controller.state, RecordingState.MOTION_ACTIVE)
        self.assertIn(self.segment_path, self.controller.event.preserved)
        self.assertEqual(self.controller.event.first_sequence, 1)

    def test_motion_on_without_pre_event_is_propagated(self) -> None:
        self.catalog.segments = []
        with self.assertRaisesRegex(RecordingError, "pré-evento"):
            self.controller.motion_on(1_000_000)
        self.assertEqual(self.controller.state, RecordingState.IDLE)
        self.assertIsNone(self.controller.event)

    def test_controller_thread_records_pre_event_failure(self) -> None:
        self.catalog.segments = []
        self.controller.start()
        self.controller.enqueue("MOTION_ON 1000000")
        deadline = time.monotonic() + 1
        while self.controller.error is None and time.monotonic() < deadline:
            time.sleep(0.01)
        self.assertIsInstance(self.controller.error, RecordingError)
        with self.assertRaisesRegex(RecordingError, "controlador de gravação falhou"):
            self.controller.shutdown()

    def test_motion_active_to_post_event(self) -> None:
        self.controller.motion_on(1_000_000)
        self.controller.motion_off(1_002_000)
        self.assertEqual(self.controller.state, RecordingState.POST_EVENT)
        self.assertEqual(self.controller.event.motion_off_ms, 1_002_000)

    def test_post_event_returns_to_same_motion_event(self) -> None:
        self.controller.motion_on(1_000_000)
        event_id = self.controller.event.event_id
        self.controller.motion_off(1_002_000)
        self.controller.motion_on(1_003_000)
        self.assertEqual(self.controller.state, RecordingState.MOTION_ACTIVE)
        self.assertEqual(self.controller.event.event_id, event_id)
        self.assertIsNone(self.controller.event.motion_off_ms)

    def test_post_event_to_finalizing_after_timeout(self) -> None:
        self.controller.motion_on(1_000_000)
        self.controller.motion_off(1_002_000)
        final_path = self.root / "segment_000000000002.ts"
        final_path.write_bytes(b"segment")
        self.catalog.segments.append(Segment(2, final_path, 1007.0, 7))
        self.clock.advance(7.1)
        with mock.patch.object(self.controller, "finalize", return_value=True) as finalize:
            self.controller.tick()
        self.assertEqual(self.controller.state, RecordingState.FINALIZING)
        finalize.assert_called_once_with()

    def test_motion_off_in_idle_is_ignored(self) -> None:
        self.controller.motion_off(1_000_000)
        self.assertEqual(self.controller.state, RecordingState.IDLE)
        self.assertIsNone(self.controller.event)

    def test_duplicate_motion_on_keeps_event(self) -> None:
        self.controller.motion_on(1_000_000)
        event_id = self.controller.event.event_id
        self.controller.motion_on(1_001_000)
        self.assertEqual(self.controller.event.event_id, event_id)

    def test_preserves_segments_during_event(self) -> None:
        self.controller.motion_on(1_000_000)
        second_path = self.root / "segment_000000000002.ts"
        second_path.write_bytes(b"segment")
        self.catalog.segments.append(Segment(2, second_path, 1001.0, 7))
        self.controller.update_preserved()
        self.assertIn(second_path, self.controller.event.preserved)

    def test_shutdown_finalizes_open_event(self) -> None:
        self.controller.motion_on(1_000_000)
        with mock.patch.object(self.controller, "finalize", return_value=True) as finalize:
            self.controller.shutdown()
        finalize.assert_called_once_with(shutdown=True)
        self.assertEqual(self.controller.state, RecordingState.FINALIZING)


class SegmentCatalogTests(unittest.TestCase):
    def test_selects_pre_event_segments(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            config = recording_config(root)
            catalog = SegmentCatalog(config)
            catalog.prepare()
            catalog.begin_session(0)
            old = config.segments_dir / "segment_000000000001.ts"
            recent = config.segments_dir / "segment_000000000002.ts"
            old.write_bytes(b"old")
            recent.write_bytes(b"recent")
            os.utime(old, (990, 990))
            os.utime(recent, (996, 996))
            selected = catalog.select_pre_event(1_000_000)
            self.assertEqual([item.path for item in selected], [recent])

    def test_orders_segments_by_sequence(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            config = recording_config(root)
            catalog = SegmentCatalog(config)
            catalog.prepare()
            for sequence in (3, 1, 2):
                (config.segments_dir / f"segment_{sequence:012d}.ts").write_bytes(b"x")
            self.assertEqual(
                [item.sequence for item in catalog.scan()],
                [1, 2, 3],
            )

    def test_pre_event_does_not_cross_session_boundary(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            config = recording_config(root)
            catalog = SegmentCatalog(config)
            catalog.prepare()
            old = config.segments_dir / "segment_000000000001.ts"
            current = config.segments_dir / "segment_000000000002.ts"
            old.write_bytes(b"old")
            current.write_bytes(b"current")
            os.utime(old, (999, 999))
            os.utime(current, (999, 999))
            catalog.begin_session(2)
            self.assertEqual(
                [segment.sequence for segment in catalog.select_pre_event(1_000_000)],
                [2],
            )

    def test_restart_uses_new_sequence_without_deleting_old_segments(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            config = recording_config(root)
            catalog = SegmentCatalog(config)
            catalog.prepare()
            old = config.segments_dir / "segment_000000000007.ts"
            old.write_bytes(b"old")
            start_number = catalog.next_sequence()
            catalog.begin_session(start_number)
            self.assertEqual(start_number, 8)
            self.assertEqual(catalog.current_session(), [])
            self.assertTrue(old.exists())

    def test_select_event_uses_sequence_anchor_within_current_session(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            config = recording_config(root)
            catalog = SegmentCatalog(config)
            catalog.prepare()
            for sequence in (4, 5, 6, 7):
                (config.segments_dir / f"segment_{sequence:012d}.ts").write_bytes(b"x")
            catalog.begin_session(5)
            event = Event("event", 1_000_000, first_sequence=6)
            self.assertEqual(
                [segment.sequence for segment in catalog.select_event(event)],
                [6, 7],
            )

    def test_byte_limit_accounts_for_expired_segments_once(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            clock = Clock()
            config = recording_config(
                root,
                pre_event_seconds=1,
                idle_retention_seconds=5,
                max_ring_bytes=10,
            )
            catalog = SegmentCatalog(config, clock)
            catalog.prepare()
            first = config.segments_dir / "segment_000000000001.ts"
            first.write_bytes(b"123456")
            catalog.scan()
            clock.advance(6)
            second = config.segments_dir / "segment_000000000002.ts"
            third = config.segments_dir / "segment_000000000003.ts"
            active = config.segments_dir / "segment_000000000004.ts"
            for segment in (second, third, active):
                segment.write_bytes(b"123456")
            catalog.segmenter_alive = lambda: True
            catalog.scan()
            catalog.retain(set(), lambda _message: None)
            self.assertFalse(first.exists())
            self.assertFalse(second.exists())
            self.assertTrue(third.exists())
            self.assertTrue(active.exists())

    def test_removes_old_idle_segments(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            clock = Clock()
            config = recording_config(root, idle_retention_seconds=5)
            catalog = SegmentCatalog(config, clock)
            catalog.prepare()
            segment = config.segments_dir / "segment_000000000001.ts"
            segment.write_bytes(b"x")
            catalog.scan()
            clock.advance(6)
            catalog.retain(set(), lambda _message: None)
            self.assertFalse(segment.exists())

    def test_does_not_remove_active_file(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            clock = Clock()
            config = recording_config(
                root,
                pre_event_seconds=1,
                idle_retention_seconds=1,
            )
            catalog = SegmentCatalog(config, clock)
            catalog.prepare()
            first = config.segments_dir / "segment_000000000001.ts"
            active = config.segments_dir / "segment_000000000002.ts"
            first.write_bytes(b"x")
            active.write_bytes(b"active")
            catalog.segmenter_alive = lambda: True
            catalog.scan()
            clock.advance(2)
            catalog.retain(set(), lambda _message: None)
            self.assertTrue(active.exists())


class FragmentationTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.temporary.name)
        self.segments: list[Segment] = []
        for sequence in range(1, 5):
            path = self.root / f"segment_{sequence:012d}.ts"
            path.write_bytes(b"segment")
            self.segments.append(Segment(sequence, path, 1000.0 + sequence, 7))
        self.catalog = FakeCatalog(self.segments)
        self.catalog.durations = {
            segment.path: 2.0 for segment in self.segments
        }
        self.controller = RecordingController(
            recording_config(
                self.root,
                pre_event_seconds=1,
                max_fragment_seconds=5,
            ),
            "diff",
            "ffmpeg",
            "ffprobe",
            self.catalog,
            input_codec="hevc",
        )
        self.runner = ValidationRunner(duration_per_segment=2.0)

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def test_long_event_uses_real_duration_and_contiguous_parts(self) -> None:
        self.controller.motion_on(1_000_000)
        event_id = self.controller.event.event_id
        with mock.patch.object(
            self.controller,
            "_run_command",
            side_effect=self.runner,
        ):
            self.controller.tick()
            self.assertEqual(self.controller.state, RecordingState.MOTION_ACTIVE)
            self.assertEqual(self.controller.event.event_id, event_id)
            self.assertEqual(self.controller.event.part_index, 2)
            self.controller.motion_off(1_010_000)
            self.controller.state = RecordingState.FINALIZING
            self.controller.finalize()

        metadata = sorted((self.root / "recordings").rglob("*.json"))
        self.assertEqual(len(metadata), 2)
        parts = [json.loads(path.read_text()) for path in metadata]
        self.assertEqual([part["part_index"] for part in parts], [1, 2])
        self.assertEqual({part["event_id"] for part in parts}, {event_id})
        self.assertEqual(parts[0]["fragment_reason"], "max_duration")
        self.assertFalse(parts[0]["is_final_part"])
        self.assertEqual(parts[1]["fragment_reason"], "motion_end")
        self.assertTrue(parts[1]["is_final_part"])
        self.assertEqual(parts[0]["first_sequence"], 1)
        self.assertEqual(parts[0]["last_sequence"], 2)
        self.assertEqual(parts[1]["first_sequence"], 3)
        self.assertEqual(parts[1]["last_sequence"], 4)
        sequences = [
            sequence
            for part in parts
            for sequence in range(
                part["first_sequence"],
                part["last_sequence"] + 1,
            )
        ]
        self.assertEqual(sequences, [1, 2, 3, 4])
        self.assertTrue(all(part["duration"] <= 5 for part in parts))

    def test_fragmentation_works_in_post_event_and_motion_can_return(self) -> None:
        self.controller.motion_on(1_000_000)
        event_id = self.controller.event.event_id
        self.controller.motion_off(1_001_000)
        with mock.patch.object(
            self.controller,
            "_run_command",
            side_effect=self.runner,
        ):
            self.controller.tick()
        self.assertEqual(self.controller.state, RecordingState.POST_EVENT)
        self.assertEqual(self.controller.event.part_index, 2)
        self.controller.motion_on(1_002_000)
        self.assertEqual(self.controller.state, RecordingState.MOTION_ACTIVE)
        self.assertEqual(self.controller.event.event_id, event_id)
        self.assertEqual(self.controller.event.part_index, 2)
        self.assertEqual(self.controller.event.part_first_sequence, 3)

    def test_shutdown_only_finalizes_pending_part(self) -> None:
        self.controller.motion_on(1_000_000)
        with mock.patch.object(
            self.controller,
            "_run_command",
            side_effect=self.runner,
        ):
            self.controller.tick()
            self.controller.shutdown()
        metadata = [
            json.loads(path.read_text())
            for path in sorted((self.root / "recordings").rglob("*.json"))
        ]
        self.assertEqual(len(metadata), 2)
        self.assertEqual(metadata[0]["last_sequence"], 2)
        self.assertEqual(metadata[1]["first_sequence"], 3)
        self.assertEqual(metadata[1]["fragment_reason"], "shutdown")
        self.assertTrue(metadata[1]["finalized_by_shutdown"])

    def test_single_segment_over_limit_is_explicit_error(self) -> None:
        self.catalog.segments = self.segments[:1]
        self.catalog.durations = {self.segments[0].path: 6.0}
        self.controller.motion_on(1_000_000)
        with self.assertRaisesRegex(RecordingError, "keyframes"):
            self.controller.tick()

    def test_sequence_gap_is_rejected_without_publishing_part(self) -> None:
        self.catalog.segments = [self.segments[0], self.segments[2]]
        self.controller.motion_on(1_000_000)
        with self.assertRaisesRegex(RecordingError, "lacuna"):
            self.controller.finalize()
        self.assertFalse(list((self.root / "recordings").rglob("*.mp4")))

    def test_intermediate_part_without_keyframe_is_rejected(self) -> None:
        self.controller.motion_on(1_000_000)
        runner = ValidationRunner(packet_flags="__")
        with mock.patch.object(
            self.controller,
            "_run_command",
            side_effect=runner,
        ):
            with self.assertRaisesRegex(RecordingError, "primeiro pacote"):
                self.controller.tick()
        self.assertEqual(self.controller.event.part_index, 1)
        self.assertFalse(list((self.root / "recordings").rglob("*.mp4")))

    def test_validated_oversize_retries_without_last_segment(self) -> None:
        self.controller.motion_on(1_000_000)
        runner = ValidationRunner(duration_per_segment=3.0)
        with mock.patch.object(
            self.controller,
            "_run_command",
            side_effect=runner,
        ):
            self.controller.tick()
        metadata = [
            json.loads(path.read_text())
            for path in sorted((self.root / "recordings").rglob("*.json"))
        ]
        self.assertEqual(
            [(part["first_sequence"], part["last_sequence"]) for part in metadata],
            [(1, 1), (2, 2)],
        )
        self.assertTrue(all(part["duration"] == 3.0 for part in metadata))
        self.assertEqual(self.controller.event.part_first_sequence, 3)


class FinalizationTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.temporary.name)
        segment_path = self.root / "segment_000000000001.ts"
        segment_path.write_bytes(b"segment")
        self.segment = Segment(1, segment_path, 1000.0, 7)
        self.controller = RecordingController(
            recording_config(self.root),
            "diff",
            "ffmpeg",
            "ffprobe",
            FakeCatalog([self.segment]),
            input_codec="hevc",
        )
        self.controller.motion_on(1_000_000)
        self.controller.motion_off(1_002_000)
        self.controller.state = RecordingState.FINALIZING

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def test_remux_failure_records_metadata(self) -> None:
        with mock.patch.object(
            self.controller,
            "_run_command",
            return_value=(1, b"", b"remux error"),
        ):
            with self.assertRaisesRegex(RecordingError, "remux falhou"):
                self.controller.finalize()
        metadata = list((self.root / "recordings").rglob("*.json"))
        self.assertEqual(len(metadata), 1)
        self.assertEqual(json.loads(metadata[0].read_text())["status"], "failed")
        self.assertEqual(self.controller.state, RecordingState.FINALIZING)
        failed = json.loads(metadata[0].read_text())
        self.assertTrue(failed["clip_path"].endswith(".mp4"))
        self.assertFalse(pathlib.Path(failed["clip_path"]).exists())

    def test_invalid_ffprobe_records_failure(self) -> None:
        def run(command: list[str]) -> tuple[int, bytes, bytes]:
            if command[0] == "ffmpeg":
                pathlib.Path(command[-1]).write_bytes(b"clip")
                return 0, b"", b""
            return 1, b"", b"invalid"

        with mock.patch.object(self.controller, "_run_command", side_effect=run):
            with self.assertRaisesRegex(RecordingError, "ffprobe"):
                self.controller.finalize()
        metadata = list((self.root / "recordings").rglob("*.json"))
        self.assertEqual(json.loads(metadata[0].read_text())["status"], "failed")
        self.assertEqual(self.controller.state, RecordingState.FINALIZING)
        failed = json.loads(metadata[0].read_text())
        self.assertFalse(pathlib.Path(failed["clip_path"]).exists())
        self.assertFalse(list((self.root / "recordings").rglob("*.part")))

    def test_missing_segments_records_and_propagates_failure(self) -> None:
        self.controller.catalog.segments = []
        with self.assertRaisesRegex(RecordingError, "nenhum segmento"):
            self.controller.finalize()
        metadata = next((self.root / "recordings").rglob("*.json"))
        self.assertEqual(json.loads(metadata.read_text())["status"], "failed")
        self.assertEqual(self.controller.state, RecordingState.FINALIZING)

    def test_successful_finalization_records_codec_and_duration(self) -> None:
        def run(command: list[str]) -> tuple[int, bytes, bytes]:
            if command[0] == "ffmpeg":
                if command[-1] == "-":
                    return 0, b"", b""
                pathlib.Path(command[-1]).write_bytes(b"valid clip")
                return 0, b"", b""
            if "-show_packets" in command:
                return 0, json.dumps({"packets": [{"flags": "K_"}]}).encode(), b""
            if "-show_frames" in command:
                frame = {"frames": [{"key_frame": 1, "pict_type": "I"}]}
                return 0, json.dumps(frame).encode(), b""
            probe = {
                "streams": [{"codec_name": "hevc"}],
                "format": {"duration": "12.4", "format_name": "mov,mp4"},
            }
            return 0, json.dumps(probe).encode(), b""

        with mock.patch.object(self.controller, "_run_command", side_effect=run):
            self.assertTrue(self.controller.finalize())
        metadata_path = next((self.root / "recordings").rglob("*.json"))
        metadata = json.loads(metadata_path.read_text())
        self.assertEqual(metadata["status"], "completed")
        self.assertEqual(metadata["codec"], "hevc")
        self.assertEqual(metadata["duration"], 12.4)
        self.assertTrue(metadata["starts_with_keyframe"])
        self.assertEqual(metadata["container"], "mp4")
        self.assertTrue(pathlib.Path(metadata["clip_path"]).exists())
        self.assertTrue(metadata["clip_path"].endswith(".mp4"))

    def test_h264_is_preserved(self) -> None:
        self.controller.set_input_codec("h264")
        runner = ValidationRunner(codec="h264")
        with mock.patch.object(
            self.controller,
            "_run_command",
            side_effect=runner,
        ):
            self.controller.finalize()
        metadata = json.loads(
            next((self.root / "recordings").rglob("*.json")).read_text()
        )
        self.assertEqual(metadata["input_codec"], "h264")
        self.assertEqual(metadata["codec"], "h264")

    def test_final_name_is_not_visible_during_validation(self) -> None:
        runner = ValidationRunner()
        with mock.patch.object(
            self.controller,
            "_run_command",
            side_effect=runner,
        ):
            self.controller.finalize()
        self.assertFalse(runner.final_existed_during_validation)

    def test_finalizer_uses_only_local_segments(self) -> None:
        runner = ValidationRunner()
        with mock.patch.object(
            self.controller,
            "_run_command",
            side_effect=runner,
        ):
            self.controller.finalize()
        commands = [
            command
            for command in runner.commands
            if command[0] == "ffmpeg" and command[-1] != "-"
        ]
        self.assertEqual(len(commands), 1)
        self.assertNotIn("rtsp://", " ".join(commands[0]))
        self.assertIn("copy", commands[0])

    def _assert_validation_error(
        self,
        runner: ValidationRunner,
        message: str,
    ) -> None:
        candidate = self.root / "candidate.mp4.part"
        candidate.write_bytes(b"candidate")
        with mock.patch.object(
            self.controller,
            "_run_command",
            side_effect=runner,
        ):
            with self.assertRaisesRegex(RecordingError, message):
                self.controller._validate_mp4(candidate)

    def test_first_packet_must_be_keyframe(self) -> None:
        self._assert_validation_error(
            ValidationRunner(packet_flags="__"),
            "primeiro pacote",
        )

    def test_first_frame_must_be_keyframe(self) -> None:
        self._assert_validation_error(
            ValidationRunner(key_frame=0),
            "primeiro frame",
        )

    def test_first_frame_pict_type_must_be_i_when_present(self) -> None:
        self._assert_validation_error(
            ValidationRunner(pict_type="P"),
            "pict_type=P",
        )

    def test_incompatible_format_is_rejected(self) -> None:
        self._assert_validation_error(
            ValidationRunner(format_name="matroska,webm"),
            "contêiner",
        )

    def test_output_codec_must_match_input(self) -> None:
        self._assert_validation_error(
            ValidationRunner(codec="h264"),
            "difere do RTSP",
        )

    def test_unsupported_output_codec_is_rejected(self) -> None:
        self._assert_validation_error(
            ValidationRunner(codec="vp9"),
            "codec MP4 não suportado",
        )

    def test_exactly_one_selected_video_stream_is_required(self) -> None:
        self._assert_validation_error(
            ValidationRunner(stream_count=2),
            "exatamente uma stream",
        )

    def test_zero_duration_is_rejected(self) -> None:
        self._assert_validation_error(
            ValidationRunner(duration_per_segment=0),
            "duração",
        )

    def test_full_decode_failure_is_rejected(self) -> None:
        self._assert_validation_error(
            ValidationRunner(decode_rc=1, decode_stderr=b"decode error"),
            "decodificação integral",
        )

    def test_empty_file_is_rejected_before_ffprobe(self) -> None:
        candidate = self.root / "empty.mp4.part"
        candidate.touch()
        with self.assertRaisesRegex(RecordingError, "vazio"):
            self.controller._validate_mp4(candidate)

    def test_segmenter_uses_copy_and_mpegts(self) -> None:
        command = build_segmenter_command(
            recording_config(self.root),
            "ffmpeg",
            "rtsp://example/video",
            42,
        )
        self.assertIn("copy", command)
        self.assertIn("mpegts", command)
        self.assertNotIn("libx265", command)
        self.assertNotIn("libx264", command)
        self.assertNotIn("h264_nvenc", command)
        self.assertIn("42", command)
        index = command.index("-break_non_keyframes")
        self.assertEqual(command[index + 1], "0")
        reset_index = command.index("-reset_timestamps")
        self.assertEqual(command[reset_index + 1], "0")


class SupervisionTests(unittest.TestCase):
    def test_unexpected_segmenter_death_is_fatal(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            process = {"argv": ["/bin/true"], "cwd": str(root)}
            config = {
                "stream_url": "rtsp://example/video",
                "ffprobe": {"executable": "/bin/true"},
                "mediamtx": dict(process),
                "publisher": dict(process),
                "detectors": {"diff": dict(process)},
            }
            supervisor = orchestrator.Supervisor(config, root, "diff")
            child = mock.Mock()
            child.rc = 9
            child.spec.name = "segmenter"
            with self.assertRaisesRegex(orchestrator.Error, "segmenter"):
                supervisor.alive(child)

    def test_old_segments_do_not_make_new_segmenter_ready(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            process = {"argv": ["/bin/true"], "cwd": str(root)}
            config = {
                "stream_url": "rtsp://example/video",
                "ffprobe": {"executable": "/bin/true"},
                "mediamtx": dict(process),
                "publisher": dict(process),
                "detectors": {"diff": dict(process)},
                "recording": {
                    "enabled": True,
                    "segments_dir": str(root / "segments"),
                    "recordings_dir": str(root / "recordings"),
                    "pre_event_seconds": 0.01,
                    "idle_retention_seconds": 1,
                    "segmenter_ready_timeout_seconds": 0.01,
                },
            }
            supervisor = orchestrator.Supervisor(config, root, "diff")
            assert supervisor.catalog is not None
            supervisor.catalog.prepare()
            old = supervisor.catalog.config.segments_dir / "segment_000000000003.ts"
            old.write_bytes(b"old")
            supervisor.catalog.begin_session(4)
            child = mock.Mock()
            child.rc = None
            with mock.patch.object(supervisor, "_probe_segment") as probe:
                with self.assertRaisesRegex(orchestrator.Error, "não ficou pronto"):
                    supervisor.segmenter_ready(child)
            probe.assert_not_called()

    def test_supervisor_detects_recording_controller_failure(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            process = {"argv": ["/bin/true"], "cwd": str(root)}
            config = {
                "stream_url": "rtsp://example/video",
                "ffprobe": {"executable": "/bin/true"},
                "mediamtx": dict(process),
                "publisher": dict(process),
                "detectors": {"diff": dict(process)},
            }
            supervisor = orchestrator.Supervisor(config, root, "diff")
            supervisor.recording = mock.Mock()
            supervisor.recording.error = RecordingError("remux falhou")
            supervisor.fifo.thread = mock.Mock()
            supervisor.fifo.thread.is_alive.return_value = True
            supervisor.stop.wait = mock.Mock(return_value=False)
            with self.assertRaisesRegex(orchestrator.Error, "remux falhou"):
                supervisor.monitor()


if __name__ == "__main__":
    unittest.main()
