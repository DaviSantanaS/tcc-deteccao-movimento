# Spec 05 — Gravação por evento com pré-evento, usando só a fila

## Objetivo

Trocar a gravação contínua por gravação acionada pelo MOG2. Cada evento vira um MP4 que:

- começa num keyframe pelo menos `pre_evento_s` antes do início do movimento;
- termina no frame em que o detector sinaliza `ended`.

A `EncodedPacketQueue` da fonte passa a ser o **único** buffer. Saem do `main` o vetor `pending_encoded_packets`, o `EncodedVideoBuffer` e o corte por N pacotes.

## Fundamentos (para os comentários do código)

- **1 pacote = 1 frame codificado:** contrato de `av_read_frame` (`libavformat/avformat.h`: "For video, the packet contains exactly one frame"). Por isso, a janela em frames vira janela em pacotes.
- **A fronteira (soma dos N) fica no frame atual ou à frente dele:** no OpenCV 4.10, o `dequeue` esvazia toda a fila de pacotes brutos (`opencv_contrib/modules/cudacodec/src/frame_queue.cpp`, `dequeue`). A folga é limitada pelas superfícies de decodificação, ≤ 32 (`video_parser.cpp`, `HandleVideoSequence`). Daí a margem.
- **Ancorar na fronteira, não no índice do frame:** pacotes que não geram frame exibido (início no meio do GOP) deslocariam o índice. A fronteira não sofre esse deslocamento.

## Escopo permitido

- `include/input/VideoStreamReader.hpp`, `src/input/VideoStreamReader.cpp`
- `include/input/FfmpegEncodedVideoSource.hpp`, `src/input/FfmpegEncodedVideoSource.cpp`
- `src/main.cpp`
- `tests/video_stream_reader_tests.cpp`: só a adaptação à nova assinatura de `read()`.
- `CMakeLists.txt`: remover `src/buffers/EncodedVideoBuffer.cpp` **somente** do executável `motion_mog2`.

**Não apagar** `EncodedVideoBuffer.*`, nem seus testes, nem `dequeueEncodedPackets*` (usados por `ffmpeg_encoded_video_packets_tests`).

## Alterações

### 1. Fonte

Acrescentar `EncodedPacketQueue& getEncodedPacketQueue();`, que retorna `this->encoded_packet_queue_`.

### 2. Reader

- Nova assinatura: `bool read(DecodedFrame& decoded_frame, cv::cuda::Stream& cuda_stream);`
- Depois de obter N, chama `this->ffmpeg_video_source_->getEncodedPacketQueue().advanceCountedPackets(N)` em vez de `dequeueEncodedPackets`. Não retira pacotes.
- Acrescentar `EncodedPacketQueue& getEncodedPacketQueue();`, que delega à fonte.
- `tests/video_stream_reader_tests.cpp`: após cada `read(decoded_frame, cuda_stream)`, obter os pacotes com `video_reader.getEncodedPacketQueue().takeCountedPackets()`. O resto do teste não muda.

### 3. `main`: parâmetros derivados

Calcular após construir reader e detector, com comentários que citem os fundamentos acima:

```
pre_event_packet_count      = calculatePreEventFrameCount(pre_evento_s, fps)
start_margin_packet_count   = maximum_decoder_lookahead_frame_count (32) + (inicio_frames - 1)
lookback_packet_count       = pre_event_packet_count + start_margin_packet_count
maximum_idle_packet_count   = lookback_packet_count + ceil(maximum_expected_gop_seconds (20.0) * fps)
```

- Constantes nomeadas em `snake_case` e `constexpr`, com comentário. Em `maximum_expected_gop_seconds`, o comentário diz que é uma suposição a ajustar depois de medir a câmera.
- Verificar overflow na soma e na conversão para `std::size_t`. Se houver, lançar `std::invalid_argument`.
- Logar `PRE_EVENT seconds=... packets=... margin_packets=... idle_cap_packets=...`.
- Remover a constante temporária `continuous_recording_packet_target` e todo o código de gravação contínua: corte por N, `next_keyframe_boundary_pending`, `skipped_initial_packet_count`, vetor pendente e `EncodedVideoBuffer`.

### 4. `main`: auxiliares

Em namespace anônimo ou como lambdas, sem mudar o formato atual do nome do arquivo:

- `buildRecordingFilePath(output_directory)`: extrai a geração atual do nome com data em português e sufixo `_NNN`.
- Abrir MP4: mesmo procedimento atual (`allocateMp4OutputContext`, `avoid_negative_ts = AVFMT_AVOID_NEG_TS_MAKE_ZERO`, `createMp4VideoStream`, `openOutputFile`, `writeMp4Header`) e log `MP4_STARTED path=...`.
- Escrever lote: se for o **primeiro lote do arquivo**, verificar que `front()` tem `AV_PKT_FLAG_KEY`; senão, lançar `std::logic_error`. Depois chama `writeMp4Packets(ctx, std::move(lote), video_stream_parameters)` e soma o contador de pacotes do arquivo.
- Finalizar: `writeMp4Trailer`, liberar contextos e log `MP4_SAVED path=... decoded_reads=... encoded_packets=...`.

A escrita acontece **sempre fora do lock da fila**: as funções `take*` já retornam o vetor por valor.

### 5. `main`: laço

Estado local: `bool event_active`, `bool waiting_for_keyframe` e contexto MP4 aberto ou não.

```
while (::capture_active):
    if !video_reader.read(decoded_frame, cuda_stream): break
    cuda_stream.waitForCompletion()
    motion_state = detector.detectMotion(...)
    queue = video_reader.getEncodedPacketQueue()

    if motion_state.started:
        event_active = true
        log EVENT_STARTED frame=<índice>
        lote = queue.takeCountedPacketsFromLookback(lookback_packet_count)
        if lote vazio: waiting_for_keyframe = true; log EVENT_WAITING_KEYFRAME
        else: abrir MP4; escrever lote; log PRE_EVENT_WRITTEN packets=<tamanho do lote>
    else if event_active:
        if waiting_for_keyframe:
            queue.discardCountedPacketsBeforeFirstKeyframe()
            lote = queue.takeCountedPackets()
            if não vazio: waiting_for_keyframe = false; abrir MP4; escrever lote
        else:
            lote = queue.takeCountedPackets(); se não vazio, escrever

    if motion_state.ended and event_active:
        log EVENT_ENDED frame=<índice>
        if MP4 aberto: finalizar
        else: log EVENT_DISCARDED_NO_KEYFRAME
        event_active = false; waiting_for_keyframe = false

    if !event_active:
        queue.trimCountedPackets(lookback_packet_count, maximum_idle_packet_count)
```

- Os pacotes do frame que gerou `ended` já foram escritos no ramo `else if event_active`, antes de finalizar.
- O pós-evento é o próprio `motion_end_frame_count`.
- Depois do laço, comportamento provisório até a spec 06: se houver MP4 aberto, escrever `takeCountedPackets()` e finalizar. A spec 06 trata dos pacotes não contabilizados.
- `CAPTURE_COMPLETE decoded_reads=... saved_files=...` no final, como hoje, sem `skipped_initial_packets`.

**Fora de escopo:** thread de escrita separada. Deixe um comentário: a escrita na mesma thread do `read()`, com `allowFrameDrop=false`, pode atrasar a leitura em RTSP. É uma limitação conhecida.

## Aceitação

**Agente:**
- compilar, se possível;
- confirmar por `grep` que `main.cpp` não referencia mais `EncodedVideoBuffer`, `pending_encoded_packets` nem `next_keyframe_boundary_pending`.

**Davi (GPU):**

1. Vídeo local com um movimento conhecido no instante T e GOP conhecido (meça com `ffprobe -select_streams v -show_entries packet=pts_time,flags -of csv`):
   - sai exatamente 1 MP4;
   - o primeiro pacote é keyframe;
   - a duração vai de ≤ T − pre_evento_s até o fim do movimento + fim_frames.
2. Validade do arquivo: `ffmpeg -v error -i evento.mp4 -f null -` sem erros.
3. Sem duplicação: concatenar os `framemd5 -c copy` de todos os MP4s de um vídeo com dois movimentos separados. Nenhum hash de pacote se repete.
4. Dois movimentos separados por menos que pre_evento_s: o segundo MP4 começa no primeiro keyframe depois do fim do primeiro. É o comportamento esperado, porque os pacotes já escritos saíram da fila.
5. Memória sem movimento: rodar 10 min no RTSP local sem movimento. A memória do processo (`ps -o rss`) fica estável.
6. `video_stream_reader_tests` passa.

## Commit

`feat: grava MP4 por evento com pre-evento usando fila unica`

## Relatório

- Diff resumido.
- Valores calculados de `lookback` e teto para 30 e 59,94 fps com pre_evento_s = 5.
- O que foi executado e o que fica para o Davi.
