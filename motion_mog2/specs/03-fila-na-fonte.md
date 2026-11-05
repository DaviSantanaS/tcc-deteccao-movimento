# Spec 03 — `FfmpegEncodedVideoSource` passa a usar `EncodedPacketQueue` (paridade)

## Objetivo

Trocar a deque e o mutex internos da fonte pela `EncodedPacketQueue`, **sem mudar o comportamento observável**. Os MP4s gravados devem ser idênticos, pacote a pacote, aos de antes. Esta etapa valida a troca da estrutura isoladamente.

## Contexto (código atual)

- `include/input/FfmpegEncodedVideoSource.hpp`: membros `encoded_packets_queue_mutex_` e `encoded_packets_queue_`.
- `src/input/FfmpegEncodedVideoSource.cpp`:
  - `getNextPacket()` enfileira o clone do pacote original **depois** de o filtro Annex B dar certo (bloco com `std::lock_guard` e `push_back`);
  - `dequeueEncodedPackets(packet_count)` lança `std::runtime_error` se faltar pacote e retira N da frente.
- `src/input/VideoStreamReader.cpp`, `read()`: chama `dequeueEncodedPackets(N)`.

## Escopo permitido

- `include/input/FfmpegEncodedVideoSource.hpp`
- `src/input/FfmpegEncodedVideoSource.cpp`
- `CMakeLists.txt`: acrescentar `src/buffers/EncodedPacketQueue.cpp` ao executável `motion_mog2` e a **todo** alvo de teste que já compila `src/input/FfmpegEncodedVideoSource.cpp`:
  - `ffmpeg_video_stream_parameters_tests`
  - `ffmpeg_encoded_video_packets_tests`
  - `video_stream_reader_tests`

## Alterações

1. Header: remover `encoded_packets_queue_mutex_`, `encoded_packets_queue_` e os `#include <deque>` e `<mutex>`, se ficarem sem uso. Acrescentar o membro `EncodedPacketQueue encoded_packet_queue_;` e o include correspondente.
2. `getNextPacket()`: substituir o bloco de lock + `push_back` por `this->encoded_packet_queue_.pushPacket(std::move(encoded_packet_for_queue));`, **no mesmo ponto** do fluxo (depois de validar a saída do filtro e antes de expor o ponteiro ao NVDEC).
3. `dequeueEncodedPackets(packet_count)`: mantenha assinatura e semântica. Implemente com `advanceCountedPackets(packet_count)` seguido de `return takeCountedPackets();`. Acrescente um comentário explicando a pré-condição: a região contabilizada está vazia na entrada, porque todo chamador retira tudo o que avança. A exceção por falta de pacotes passa a vir da fila, com a mesma mensagem.
4. `dequeueEncodedPacketsWithStreamParameters`: sem mudança.
5. `VideoStreamReader` e `main`: sem mudança.

## Aceitação

**Agente (se o ambiente permitir):** configurar e compilar todos os alvos e rodar `encoded_packet_queue_tests`. Se não houver OpenCV com CUDA, diga isso e compile ao menos `src/input/FfmpegEncodedVideoSource.cpp` com `-fsyntax-only` contra os headers disponíveis. Se nem isso for possível, declare explicitamente.

**Davi (GPU), paridade de MP4.** Usar o **mesmo arquivo de entrada e o mesmo alvo de pacotes** antes (commit anterior) e depois desta spec:

```bash
./build/motion_mog2 /caminho/video.mp4 /tmp/antes 300    # no commit anterior
./build/motion_mog2 /caminho/video.mp4 /tmp/depois 300   # neste commit
for f in /tmp/antes/*.mp4;  do ffmpeg -v error -i "$f" -map 0:v -c copy -f framemd5 -; done > /tmp/antes.md5
for f in /tmp/depois/*.mp4; do ffmpeg -v error -i "$f" -map 0:v -c copy -f framemd5 -; done > /tmp/depois.md5
diff /tmp/antes.md5 /tmp/depois.md5 && echo PARIDADE_OK
```

Com `-c copy`, o `framemd5` gera um hash por **pacote** e não decodifica nada. Diferença zero significa os mesmos bytes e timestamps. Repita com o RTSP local (MediaMTX). Nesse caso, compare só a contagem de pacotes por arquivo (`ffprobe -v error -count_packets -show_entries stream=nb_read_packets`), porque o conteúdo depende do momento da conexão.

Rode também `video_stream_reader_tests` e `ffmpeg_encoded_video_packets_tests`.

## Commit

`refactor: fonte FFmpeg usa EncodedPacketQueue`

## Relatório

- Diff resumido.
- O que foi compilado ou executado, com a saída.
- O que fica para o Davi validar.
