import json
import os
import pathlib
import signal
import stat
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from unittest import mock

import orchestrator


ROOT = pathlib.Path(__file__).resolve().parents[1]
FAKE = ROOT / "tests" / "helpers" / "fake_process.py"


def config(root: pathlib.Path) -> dict:
    process = {
        "argv": [sys.executable, str(FAKE)],
        "cwd": str(root),
    }
    return {
        "stream_url": "rtsp://127.0.0.1:8554/video",
        "logs_dir": str(root / "logs"),
        "fifo": {
            "path": str(root / "bus"),
            "ready_timeout_seconds": 1,
            "poll_interval_seconds": 0.01,
        },
        "ffprobe": {
            "executable": sys.executable,
        },
        "mediamtx": {**process, "health": {"host": "127.0.0.1", "port": 8554, "timeout_seconds": 0.1}},
        "publisher": {**process, "health": {"timeout_seconds": 0.1, "interval_seconds": 0.01}},
        "detectors": {
            "mog2": dict(process),
            "diff": dict(process),
        },
        "shutdown": {
            "sigint_timeout_seconds": 0.15,
            "sigterm_timeout_seconds": 0.15,
            "sigkill_timeout_seconds": 0.3,
            "thread_timeout_seconds": 1,
        },
    }


def child_mock(
    name: str,
    returncode: int | None = None,
) -> orchestrator.Child:
    spec = orchestrator.Spec(
        name,
        [sys.executable, str(FAKE)],
        ROOT,
        [],
        {},
    )
    process = mock.Mock(pid=99)
    process.poll.return_value = returncode
    return orchestrator.Child(
        spec,
        process,
        mock.Mock(),
        mock.Mock(),
    )


class ConfigurationTests(unittest.TestCase):
    def test_invalid_json(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            config_path = pathlib.Path(temporary) / "config.json"
            config_path.write_text("{invalid", encoding="utf-8")

            with self.assertRaisesRegex(
                orchestrator.Error,
                "configuração inválida",
            ):
                orchestrator.load_config(config_path)

    def test_missing_required_field(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            config_path = pathlib.Path(temporary) / "config.json"
            config_path.write_text(
                json.dumps({"stream_url": "rtsp://example"}),
                encoding="utf-8",
            )

            with self.assertRaisesRegex(
                orchestrator.Error,
                "campo ausente: mediamtx",
            ):
                orchestrator.load_config(config_path)

    def test_unknown_detector(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)

            with self.assertRaisesRegex(
                orchestrator.Error,
                "detector desconhecido",
            ):
                orchestrator.Supervisor(
                    config(root),
                    root,
                    "unknown",
                )

    def test_example_arguments(self) -> None:
        example = json.loads(
            (ROOT / "orchestrator.example.json").read_text(
                encoding="utf-8"
            )
        )
        self.assertEqual(
            example["detectors"]["mog2"]["argv"][1:],
            [
                "rtsp://127.0.0.1:8554/video",
                "25",
                "9000",
                "2",
                "8",
            ],
        )
        self.assertEqual(
            example["detectors"]["diff"]["argv"][1:],
            [
                "rtsp://127.0.0.1:8554/video",
                "25",
                "9000",
                "2",
                "8",
                "30",
                "5",
                "0",
            ],
        )


class LockTests(unittest.TestCase):
    def test_exclusive_and_release(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            lock_path = pathlib.Path(temporary) / "lock"
            first = orchestrator.Lock(lock_path)
            first.__enter__()

            with self.assertRaises(orchestrator.Error):
                orchestrator.Lock(lock_path).__enter__()

            first.__exit__(None, None, None)
            second = orchestrator.Lock(lock_path)
            second.__enter__()
            second.__exit__(None, None, None)


class FIFOTests(unittest.TestCase):
    def test_create_ready_reconnect_stop_and_preserve(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            stop = threading.Event()
            reader = orchestrator.FIFO(
                root / "bus",
                stop,
                root / "events",
                0.01,
            )
            reader.start(1)

            self.assertTrue(
                stat.S_ISFIFO((root / "bus").stat().st_mode)
            )
            self.assertTrue(reader.ready.is_set())

            for message in (
                b"MOTION_ON 1\n",
                b"MOTION_OFF 2\n",
            ):
                descriptor = os.open(
                    root / "bus",
                    os.O_WRONLY | os.O_NONBLOCK,
                )
                os.write(descriptor, message)
                os.close(descriptor)
                time.sleep(0.03)

            stop.set()
            reader.join(1)

            self.assertFalse(reader.thread.is_alive())
            self.assertIsNone(reader.fd)
            self.assertEqual(
                (root / "events").read_text(
                    encoding="utf-8"
                ).splitlines(),
                ["MOTION_ON 1", "MOTION_OFF 2"],
            )
            self.assertTrue((root / "bus").exists())

    def test_regular_file_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            fifo_path = pathlib.Path(temporary) / "bus"
            fifo_path.write_text("x", encoding="utf-8")
            reader = orchestrator.FIFO(
                fifo_path,
                threading.Event(),
                fifo_path.with_name("out"),
            )

            with self.assertRaises(orchestrator.Error):
                reader.prepare()


class ProcessTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.temporary.name)
        self.supervisor = orchestrator.Supervisor(
            config(self.root),
            self.root,
            "mog2",
        )
        self.supervisor.logs.mkdir()

    def tearDown(self) -> None:
        if not self.supervisor.closing:
            self.supervisor.shutdown()
        self.temporary.cleanup()

    def test_separate_logs_no_shell_session(self) -> None:
        spec = orchestrator.Spec(
            "child",
            [
                sys.executable,
                str(FAKE),
                "--out",
                "OUT",
                "--err",
                "ERR",
                "--exit",
                "0",
            ],
            self.root,
            [],
            {},
        )
        with mock.patch(
            "orchestrator.subprocess.Popen",
            wraps=orchestrator.subprocess.Popen,
        ) as popen:
            child = self.supervisor.spawn(spec)
            child.proc.wait(2)
            arguments = popen.call_args.kwargs
            self.assertFalse(arguments["shell"])
            self.assertTrue(arguments["start_new_session"])

        child.close()
        self.assertIn(
            "OUT",
            (
                self.supervisor.logs
                / "child.stdout.log"
            ).read_text(encoding="utf-8"),
        )
        self.assertIn(
            "ERR",
            (
                self.supervisor.logs
                / "child.stderr.log"
            ).read_text(encoding="utf-8"),
        )

    def test_unexpected_exit(self) -> None:
        child = self.supervisor.spawn(
            orchestrator.Spec(
                "child",
                [
                    sys.executable,
                    str(FAKE),
                    "--exit",
                    "7",
                ],
                self.root,
                [],
                {},
            )
        )
        child.proc.wait(2)

        with self.assertRaisesRegex(
            orchestrator.Error,
            "rc=7",
        ):
            self.supervisor.alive(child)

    def test_ffprobe_timeout(self) -> None:
        process = mock.Mock(pid=123, returncode=None)
        process.communicate.side_effect = (
            subprocess.TimeoutExpired(["ffprobe"], 0.01)
        )
        process.poll.return_value = None

        with mock.patch(
            "orchestrator.subprocess.Popen",
            return_value=process,
        ), mock.patch(
            "orchestrator.os.killpg"
        ) as kill_group:
            result = self.supervisor.probe(0.01)

        self.assertFalse(result)
        kill_group.assert_called_once_with(
            123,
            signal.SIGKILL,
        )
        process.wait.assert_called_once_with()

    def test_publisher_exits_before_stream_ready(self) -> None:
        publisher = child_mock("publisher", returncode=4)

        with mock.patch.object(
            self.supervisor,
            "probe",
        ) as probe:
            with self.assertRaisesRegex(
                orchestrator.Error,
                "publisher encerrou inesperadamente",
            ):
                self.supervisor.stream(publisher)

        probe.assert_not_called()

    def test_escalates_to_sigkill(self) -> None:
        child = self.supervisor.spawn(
            orchestrator.Spec(
                "child",
                [
                    sys.executable,
                    str(FAKE),
                    "--ignore",
                ],
                self.root,
                [],
                {},
            )
        )
        time.sleep(0.08)

        with mock.patch(
            "orchestrator.os.killpg",
            wraps=os.killpg,
        ) as kill_group:
            self.supervisor.shutdown()

        self.assertEqual(
            [
                call.args[1]
                for call in kill_group.call_args_list
            ],
            [
                signal.SIGINT,
                signal.SIGTERM,
                signal.SIGKILL,
            ],
        )
        self.assertIsNotNone(child.rc)

    def test_graceful_shutdown_stops_at_sigint(self) -> None:
        child = self.supervisor.spawn(
            orchestrator.Spec(
                "child",
                [sys.executable, str(FAKE)],
                self.root,
                [],
                {},
            )
        )
        time.sleep(0.08)

        with mock.patch(
            "orchestrator.os.killpg",
            wraps=os.killpg,
        ) as kill_group:
            self.supervisor.shutdown()

        self.assertEqual(
            [
                call.args[1]
                for call in kill_group.call_args_list
            ],
            [signal.SIGINT],
        )
        self.assertIsNotNone(child.rc)


class PipelineTests(unittest.TestCase):
    def test_fifo_ready_precedes_detector(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            supervisor = orchestrator.Supervisor(
                config(root),
                root,
                "diff",
            )
            seen: list[tuple[str, bool]] = []

            def spawn(spec: orchestrator.Spec) -> orchestrator.Child:
                seen.append(
                    (spec.name, supervisor.fifo.ready.is_set())
                )
                child = child_mock(spec.name)
                supervisor.children.append(child)
                return child

            with mock.patch.object(
                supervisor,
                "preflight",
                side_effect=supervisor.fifo.prepare,
            ), mock.patch.object(
                supervisor,
                "spawn",
                side_effect=spawn,
            ), mock.patch.object(
                supervisor,
                "port",
            ), mock.patch.object(
                supervisor,
                "stream",
            ), mock.patch.object(
                supervisor,
                "detector_ready",
            ):
                supervisor.start()

            self.assertEqual(
                [name for name, _ready in seen],
                ["mediamtx", "publisher", "detector"],
            )
            self.assertTrue(
                all(ready for _name, ready in seen)
            )
            supervisor.stop.set()
            supervisor.fifo.join(1)

    def test_rolls_back_started_processes_after_failure(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            supervisor = orchestrator.Supervisor(
                config(root),
                root,
                "mog2",
            )
            started: list[orchestrator.Child] = []

            def spawn(spec: orchestrator.Spec) -> orchestrator.Child:
                child = child_mock(spec.name)
                started.append(child)
                supervisor.children.append(child)
                child.proc.poll.side_effect = [None, 0]
                return child

            with mock.patch.object(
                supervisor,
                "preflight",
                side_effect=supervisor.fifo.prepare,
            ), mock.patch.object(
                supervisor,
                "spawn",
                side_effect=spawn,
            ), mock.patch.object(
                supervisor,
                "port",
            ), mock.patch.object(
                supervisor,
                "stream",
                side_effect=orchestrator.Error("stream falhou"),
            ), mock.patch.object(
                supervisor,
                "shutdown",
                wraps=supervisor.shutdown,
            ) as shutdown:
                try:
                    supervisor.start()
                except orchestrator.Error:
                    supervisor.shutdown()

            self.assertEqual(
                [child.spec.name for child in started],
                ["mediamtx", "publisher"],
            )
            shutdown.assert_called_once_with()


class SignalTests(unittest.TestCase):
    def check_signal(self, current_signal: signal.Signals) -> None:
        stop = threading.Event()
        handlers: dict[signal.Signals, object] = {}

        def capture(
            sig: signal.Signals,
            handler: object,
        ) -> object:
            handlers[sig] = handler
            return signal.SIG_DFL

        with mock.patch(
            "orchestrator.signal.signal",
            side_effect=capture,
        ):
            previous = orchestrator.install_signal_handlers(
                stop
            )

        handler = handlers[current_signal]
        self.assertTrue(callable(handler))
        handler(current_signal, None)
        self.assertTrue(stop.is_set())
        self.assertIn(current_signal, previous)

    def test_sigint_handler(self) -> None:
        self.check_signal(signal.SIGINT)

    def test_sigterm_handler(self) -> None:
        self.check_signal(signal.SIGTERM)


if __name__ == "__main__":
    unittest.main()
