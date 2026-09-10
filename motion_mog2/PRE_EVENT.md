# Pre-evento configuravel

O usuario informa segundos; o `main` calcula `ceil(segundos * FPS)` e passa a
quantidade de frames para `EncodedVideoBuffer`. O detector MOG2 continua
responsavel por identificar o movimento.

## Inicializador

Na raiz do repositorio:

```bash
bash run_mog2_test.sh video/source_timer.mp4 5
```

O segundo argumento e o tempo de pre-evento em segundos. O valor padrao e zero.
Use ponto para valores fracionarios, por exemplo `2.5`.

Todos os argumentos do inicializador:

```text
bash run_mog2_test.sh [video.mp4] [pre_event_seconds] [motion_threshold_percent] [motion_start_frames] [motion_end_frames]
```

Exemplo com todos os valores explicitos:

```bash
bash run_mog2_test.sh video/source_timer.mp4 5 1.0 2 3
```

O script encaminha os argumentos ao executavel nesta ordem:

```text
motion_mog2 RTSP_URL motion_threshold_percent motion_start_frames motion_end_frames pre_event_seconds
```

Os caminhos de instalacao existentes podem ser sobrescritos:

```bash
OPENCV_DIR=/caminho/opencv/lib/cmake/opencv4 \
MEDIAMTX_BIN=/caminho/mediamtx \
bash run_mog2_test.sh /caminho/video.mp4 5
```

O inicializador mostra a saida do detector no terminal e a salva em
`.run_mog2/motion_mog2.log`. Esse arquivo e sobrescrito em cada execucao.
Ctrl+C encerra a execucao e limpa os processos iniciados pelo script.

## O que conferir nos logs

A 59.94 FPS, cinco segundos sao convertidos para 300 frames:

```text
PRE_EVENT seconds=5 frames=300 reference=decoded_frame_index
```

Em `MOTION_BUFFER_START`:

- `requested_pre_event_frames`: quantidade solicitada apos a conversao.
- `extra_frames_before_motion`: diferenca entre os indices observados de inicio
  do evento e do inicio selecionado para o buffer.
- `starts_with_key_frame`: indica se o primeiro pacote esta marcado com keyframe.
- `pre_event_history_sufficient`: 1 quando ha inicio por keyframe e a diferenca
  de indices cobre a quantidade solicitada; 0 quando esse historico nao existe.

Os primeiros eventos podem informar historico insuficiente enquanto o leitor
ainda esta acumulando dados. Os pacotes disponiveis continuam sendo usados.

## Politica de retencao

O buffer conserva GOPs anteriores e o GOP corrente. O inicio desejado e o indice
de `MOTION_ON` menos a quantidade configurada, limitado a zero. Selecionamos o
ultimo keyframe observado ate esse limite e conservamos os GOPs posteriores.
O GOP corrente ja inclui os pacotes do ciclo de `MOTION_ON`, evitando duplica-los.

Com zero segundos, permanece o comportamento anterior: comecar no GOP corrente.
Na ausencia de qualquer keyframe, permanece o fallback dos pacotes do ciclo,
com `starts_with_key_frame=0` e `pre_event_history_sufficient=0`.

Os indices registram ciclos de processamento, nao timestamps de midia nem uma
correspondencia exata entre pacote e frame. Portanto, a duracao e aproximada e
pode incluir uma margem anterior por causa do alinhamento ao keyframe.
`duration_seconds` continua medindo diferenca entre horarios locais de observacao.

A janela em frames nao e um teto de bytes: a memoria depende do bitrate e do
intervalo entre keyframes. Sem novos keyframes, o GOP corrente pode continuar
crescendo; o buffer de um evento ativo tambem cresce ate o fim do evento.

Esta etapa conserva os dados em memoria e registra estatisticas. A gravacao MP4
depende da implementacao futura do remuxer.

## Testes da logica sem CUDA

Os pacotes codificados ficam em `EncodedPacket.hpp`, sem dependencia de OpenCV.
Isso permite testar o buffer e a conversao usando somente um compilador C++17:

```bash
g++ -std=c++17 -Wall -Wextra -Werror -pedantic \
  -I motion_mog2/include \
  motion_mog2/tests/pre_event_tests.cpp \
  motion_mog2/src/EncodedVideoBuffer.cpp \
  motion_mog2/src/PreEventConfig.cpp \
  -o /tmp/tcc-pre-event-tests
/tmp/tcc-pre-event-tests
```

Os dez grupos cobrem conversao, entradas invalidas, zero segundos, varios GOPs,
limites exatos, historico insuficiente, keyframe ausente/tardio, lotes vazios,
varios keyframes no mesmo lote e continuidade entre eventos. Os pacotes dos
testes sao sinteticos; os testes nao comprovam a decodificacao do video.

Com o projeto configurado pelo CMake e `BUILD_TESTING` habilitado, o mesmo
executavel tambem e registrado no CTest como `pre_event_tests`.
