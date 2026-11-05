# Spec 06 — Encerramento (EOF e Ctrl+C) sem perder pacotes do evento

## Objetivo

Hoje, ao sair do laço, os pacotes ainda não contabilizados (lidos à frente do decoder) nunca são escritos. Se o encerramento acontecer **durante um evento**, esses pacotes devem entrar no MP4. Sem evento ativo, são descartados com log.

## Fundamentos (para os comentários)

- O destrutor do `VideoReaderImpl` (OpenCV 4.10) chama `frameQueue_->endDecode()` e depois `videoSource_->stop()` (`video_reader.cpp`, `~VideoReaderImpl`). O `stop()` espera a thread de leitura terminar (`video_source.cpp`, `RawVideoSourceWrapper::stop`: `thread_->wait()`). O `endDecode()` libera as esperas internas da fila de frames.
- Depois disso, ninguém mais chama `getNextPacket`, então a fila não recebe novos pacotes e `takeAllPackets()` é seguro.
- O `VideoStreamReader` guarda a fonte num `cv::Ptr` próprio. Por isso a fila sobrevive à destruição do `VideoReader` do OpenCV.

## Escopo permitido

- `include/input/VideoStreamReader.hpp`, `src/input/VideoStreamReader.cpp`
- `src/main.cpp`

## Alterações

### 1. `VideoStreamReader::stopDecoding()`

- Faz `this->nvdec_video_reader_.reset()`.
- Pode ser chamado mais de uma vez sem efeito extra.
- Depois de chamado, `read()` lança `std::logic_error`.
- `getEncodedPacketQueue()` continua válido.
- Comentário: se a leitura RTSP estiver bloqueada em `av_read_frame` (fonte parada), o `reset` pode demorar até a leitura retornar. Não há timeout nesta etapa (limitação conhecida).

### 2. `main`, depois do laço

Substitui o comportamento provisório da spec 05.

```
video_reader.stopDecoding()
restantes = queue.takeAllPackets()     // contabilizados + à frente, em ordem

if event_active:
    if waiting_for_keyframe:
        descartar de 'restantes' tudo antes do primeiro keyframe
        if sobrou algo: abrir MP4
    if MP4 aberto e 'restantes' não vazio: escrever restantes
    if MP4 aberto: finalizar
    else: log EVENT_DISCARDED_NO_KEYFRAME
    log SHUTDOWN_FLUSHED packets=<escritos>
else:
    log SHUTDOWN_DISCARDED_IDLE_PACKETS packets=<restantes.size()>
```

- Os pacotes à frente da fronteira não passaram pelo detector. Mesmo assim pertencem ao evento, porque estão em sequência contínua com ele. Registre isso em comentário.
- `stopDecoding()` deve ser chamado também no caminho de saída por `break` (EOF), não só no Ctrl+C. A sequência acima roda depois do laço nos dois casos.
- Em exceção, não é preciso salvar nada (fora de escopo).

## Aceitação

**Agente:** compilar, se possível.

**Davi (GPU):**

1. **EOF durante o movimento:** use um vídeo local que termine com movimento (corte um com `ffmpeg -ss/-t -c copy`).
   - O MP4 do evento termina no último pacote do arquivo de entrada.
   - Confira: o último `pts_time` do MP4 (relativo) mais o início do evento bate com o último pacote da entrada (`ffprobe -show_entries packet=pts_time`).
   - Antes desta spec faltavam pacotes no fim; registre a diferença no relatório do TCC.
2. **Ctrl+C durante o movimento (RTSP local):** o processo encerra em poucos segundos, o MP4 é válido (`ffmpeg -v error -i x.mp4 -f null -`) e aparece `SHUTDOWN_FLUSHED`.
3. **Ctrl+C sem movimento:** aparece `SHUTDOWN_DISCARDED_IDLE_PACKETS` e nenhum MP4 novo.
4. **Ctrl+C logo após `EVENT_WAITING_KEYFRAME`** (GOP longo; pode exigir um vídeo com keyframe raro): sem crash. Sai um MP4 ou `EVENT_DISCARDED_NO_KEYFRAME`.

## Commit

`feat: grava pacotes restantes do evento no encerramento`

## Relatório

- Diff resumido.
- O que foi executado e o que fica para o Davi.
- Quanto tempo o encerramento levou no teste 2, se tiver sido executado.
