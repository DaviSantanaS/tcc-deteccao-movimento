#!/usr/bin/env python3
import os
import time
import signal
import subprocess
import pathlib
import threading

# ========= CONFIG (MVP) =========
HOME      = str(pathlib.Path.home())
MEDIAMTX  = "/usr/local/bin/mediamtx"
MTXCFG    = f"{HOME}/mediamtx/mediamtx.yml"

RTSP_URL  = "rtsp://127.0.0.1:8554/video"
VIDEO     = f"{HOME}/tcc/video/video_hevc_fhd_25fps.mp4"

RING_DIR  = "/dev/shm/ring"
CLIPS_DIR = f"{HOME}/tcc/clips"
FIFO      = "/tmp/motion_bus"

PRE_SEC   = 5
POST_SEC  = 5
SEG_TIME  = 1
SEG_NAME_FMT = "%Y%m%d-%H%M%S"   # nomes do ring: 20260122-235316.ts

COOLDOWN_SEC = 10

ENABLE_DETECTOR = False
DETECTOR_BIN    = f"{HOME}/tcc/cpp_motion_headless_silent/build/motion_headless_silent"
# args: <url> [fps] [threshold] [start_frames] [end_frames]
DETECTOR_ARGS   = [RTSP_URL, "30", "9000", "2", "8"]
# ================================

# Verbosidade mínima:
# - Só imprime spawn/encerramento/collector.
# - Saída do ffmpeg/mediamtx só aparece em caso de erro (e ainda assim filtrada).
PRINT_PROC_ERRORS_ONLY = True
PRINT_PROC_STDOUT_FOR = set(["detector"])  # ex.: {"detector"} se quiser ver algo dele
SHOW_FULL_CMD_ON_SPAWN = False             # True se quiser ver o comando completo no spawn

procs = []  # [(name, Popen), ...]

def ensure_dirs():
    pathlib.Path(RING_DIR).mkdir(parents=True, exist_ok=True)
    pathlib.Path(CLIPS_DIR).mkdir(parents=True, exist_ok=True)
    pathlib.Path(f"{HOME}/mediamtx").mkdir(parents=True, exist_ok=True)

    if not pathlib.Path(MTXCFG).exists():
        pathlib.Path(MTXCFG).write_text(
            "rtsp: yes\n"
            "rtspAddress: :8554\n\n"
            "paths:\n"
            "  video: {}\n"
        )

    # garante FIFO
    if os.path.exists(FIFO) and not stat_is_fifo(FIFO):
        os.remove(FIFO)
    if not os.path.exists(FIFO):
        os.mkfifo(FIFO)

def stat_is_fifo(path: str) -> bool:
    try:
        st = os.stat(path)
        return (st.st_mode & 0o170000) == 0o010000  # S_IFIFO
    except Exception:
        return False

def is_ffmpeg_cmd(cmd):
    return len(cmd) > 0 and cmd[0] == "ffmpeg"

def with_quiet_ffmpeg(cmd):
    # minimiza o spam do ffmpeg, deixando só erros
    if is_ffmpeg_cmd(cmd):
        # evita duplicar se já tiver
        if "-hide_banner" not in cmd:
            cmd = cmd[:1] + ["-hide_banner"] + cmd[1:]
        # se tiver loglevel, não mexe; senão, seta error
        if "-loglevel" not in cmd:
            cmd = cmd[:1] + ["-loglevel", "error"] + cmd[1:]
    return cmd

def spawn(cmd, name):
    cmd = with_quiet_ffmpeg(cmd)

    if SHOW_FULL_CMD_ON_SPAWN:
        print(f"[spawn] {name}: {' '.join(cmd)}", flush=True)
    else:
        print(f"[spawn] {name}", flush=True)

    p = subprocess.Popen(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1
    )
    procs.append((name, p))

    def reader():
        try:
            for line in p.stdout:
                line = line.rstrip()
                if not line:
                    continue

                # Se quiser ver stdout completo de algum processo específico
                if name in PRINT_PROC_STDOUT_FOR:
                    print(f"[{name}] {line}", flush=True)
                    continue

                # Por padrão, só mostra erros (mínimo)
                if PRINT_PROC_ERRORS_ONLY:
                    low = line.lower()
                    # heurística simples: mostra linhas que parecem erro/aviso sério
                    if ("error" in low) or ("fatal" in low) or ("invalid" in low) or ("failed" in low):
                        print(f"[{name}] {line}", flush=True)
                else:
                    print(f"[{name}] {line}", flush=True)

        except Exception:
            pass

    threading.Thread(target=reader, daemon=True).start()
    return p

def ffmpeg_concat(paths, outmp4):
    with open("/tmp/files.txt", "w") as f:
        for p in paths:
            f.write(f"file '{p}'\n")

    # concat também com log mínimo
    return subprocess.call([
        "ffmpeg", "-hide_banner", "-loglevel", "error",
        "-y",
        "-f", "concat", "-safe", "0",
        "-i", "/tmp/files.txt",
        "-c", "copy",
        "-movflags", "+faststart",
        outmp4
    ])

def existing_segments(t0, t1):
    # converte epoch(seg) -> nome do arquivo no padrão do ring (%Y%m%d-%H%M%S.ts)
    segs = []
    for s in range(int(t0), int(t1) + 1):
        name = time.strftime(SEG_NAME_FMT, time.localtime(s))
        p = f"{RING_DIR}/{name}.ts"
        if os.path.exists(p):
            segs.append(p)
    return segs

def collector_loop():
    on_ms = None
    last_clip_end_s = 0

    while True:
        try:
            # abre FIFO em RDWR|NONBLOCK para nunca travar o writer
            fd = os.open(FIFO, os.O_RDWR | os.O_NONBLOCK)
            with os.fdopen(fd, "r") as f:
                while True:
                    line = f.readline()
                    if not line:
                        time.sleep(0.1)
                        continue

                    parts = line.strip().split()
                    if len(parts) < 2:
                        continue

                    ev, ts_str = parts[0], parts[1]
                    try:
                        ts = int(ts_str)  # epoch_ms
                    except Exception:
                        continue

                    if ev == "MOTION_ON":
                        on_ms = ts
                        print(f"[collector] ON @ {ts}", flush=True)

                    elif ev == "MOTION_OFF" and on_ms is not None:
                        print(f"[collector] OFF @ {ts}", flush=True)

                        on_s  = on_ms // 1000
                        off_s = ts    // 1000
                        t0 = max(0, on_s - PRE_SEC)
                        t1 = off_s + POST_SEC

                        # cooldown: evita clipes duplicados muito próximos
                        if t0 <= last_clip_end_s + COOLDOWN_SEC:
                            print("[collector] cooldown ativo, ignorando evento", flush=True)
                            on_ms = None
                            continue

                        # espera pós-roll existir
                        while int(time.time()) < t1 + 1:
                            time.sleep(0.25)

                        segs = existing_segments(t0, t1)
                        if not segs:
                            print("[collector] nenhum segmento encontrado", flush=True)
                            on_ms = None
                            continue

                        stamp = time.strftime("%Y%m%d-%H%M%S", time.localtime(t0))
                        out = f"{CLIPS_DIR}/clip_{stamp}_{t0}-{t1}.mp4"

                        rc = ffmpeg_concat(segs, out)
                        print(f"[collector] {'OK' if rc==0 else 'ERRO'} -> {out} ({len(segs)} segs)", flush=True)
                        on_ms = None
                        if rc == 0:
                            last_clip_end_s = t1

        except Exception as e:
            print(f"[collector][ex] {e}", flush=True)
            time.sleep(0.5)

def main():
    ensure_dirs()

    spawn([MEDIAMTX, MTXCFG], "mediamtx")
    time.sleep(0.5)

    spawn([
        "ffmpeg", "-re", "-stream_loop", "-1",
        "-i", VIDEO,
        "-an",
        "-c:v", "copy",
        "-fflags", "+genpts",
        "-rtsp_transport", "tcp",
        "-f", "rtsp",
        RTSP_URL
    ], "publisher")

    # espera RTSP ficar disponível antes de iniciar o ring (evita ring morrer na largada)
    for _ in range(60):  # ~30s (60 * 0.5s)
        rc = subprocess.call(
            ["ffprobe", "-rtsp_transport", "tcp", "-v", "error", RTSP_URL],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL
        )
        if rc == 0:
            break
        time.sleep(0.5)

    seg_time = str(SEG_TIME)
    spawn([
        "ffmpeg",
        "-rtsp_transport", "tcp",
        "-i", RTSP_URL,
        "-an",
        "-c:v", "copy",
        "-bsf:v", "hevc_mp4toannexb",
        "-f", "segment",
        "-segment_format", "mpegts",
        "-segment_time", seg_time,
        "-reset_timestamps", "1",
        "-strftime", "1",
        f"{RING_DIR}/{SEG_NAME_FMT}.ts"
    ], "ring")

    threading.Thread(target=collector_loop, daemon=True).start()

    if ENABLE_DETECTOR:
        spawn([DETECTOR_BIN] + DETECTOR_ARGS, "detector")

    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        print("[orchestrator] encerrando...", flush=True)
        for name, p in procs:
            try:
                p.send_signal(signal.SIGINT)
            except Exception:
                pass
        for name, p in procs:
            try:
                p.wait(timeout=3)
            except Exception:
                pass
        print("[orchestrator] bye.", flush=True)

if __name__ == "__main__":
    main()