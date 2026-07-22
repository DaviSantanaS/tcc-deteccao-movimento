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
        "container": "mkv",
    }
    data.update(overrides)
    return RecordingConfig.load(data, root)


class FakeCatalog:
    def __init__(self, segments: list[Segment] | None = None) -> None:
        self.segments = segments or []
        self.retained: list[set[pathlib.Path]] = []

    def prepare(self) -> None:
        pass

    def select_pre_event(self, _timestamp: int) -> list[Segment]:
        return list(self.segments)

    def select_event(self, _event: Event) -> list[Segment]:
        return list(self.segments)

    def scan(self, include_active: bool = False) -> list[Segment]:
        return list(self.segments)

    def retain(self, preserved: set[pathlib.Path], _log: object) -> None:
        self.retained.append(set(preserved))


class RecordingConfigurationTests(unittest.TestCase):
    def test_valid_configuration(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            config = recording_config(root)
            self.assertTrue(config.enabled)
            self.assertEqual(config.pre_event_seconds, 5)
            self.assertEqual(config.container, "mkv")

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
        )

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def test_idle_to_motion_active(self) -> None:
        self.controller.motion_on(1_000_000)
        self.assertEqual(self.controller.state, RecordingState.MOTION_ACTIVE)
        self.assertIn(self.segment_path, self.controller.event.preserved)

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
            self.assertFalse(self.controller.finalize())
        metadata = list((self.root / "recordings").rglob("*.json"))
        self.assertEqual(len(metadata), 1)
        self.assertEqual(json.loads(metadata[0].read_text())["status"], "failed")

    def test_invalid_ffprobe_records_failure(self) -> None:
        def run(command: list[str]) -> tuple[int, bytes, bytes]:
            if command[0] == "ffmpeg":
                pathlib.Path(command[-1]).write_bytes(b"clip")
                return 0, b"", b""
            return 1, b"", b"invalid"

        with mock.patch.object(self.controller, "_run_command", side_effect=run):
            self.assertFalse(self.controller.finalize())
        metadata = list((self.root / "recordings").rglob("*.json"))
        self.assertEqual(json.loads(metadata[0].read_text())["status"], "failed")

    def test_successful_finalization_records_codec_and_duration(self) -> None:
        def run(command: list[str]) -> tuple[int, bytes, bytes]:
            if command[0] == "ffmpeg":
                pathlib.Path(command[-1]).write_bytes(b"valid clip")
                return 0, b"", b""
            probe = {
                "streams": [{"codec_name": "hevc"}],
                "format": {"duration": "12.4"},
            }
            return 0, json.dumps(probe).encode(), b""

        with mock.patch.object(self.controller, "_run_command", side_effect=run):
            self.assertTrue(self.controller.finalize())
        metadata_path = next((self.root / "recordings").rglob("*.json"))
        metadata = json.loads(metadata_path.read_text())
        self.assertEqual(metadata["status"], "completed")
        self.assertEqual(metadata["codec"], "hevc")
        self.assertEqual(metadata["duration"], 12.4)

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
        self.assertIn("42", command)


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


if __name__ == "__main__":
    unittest.main()
