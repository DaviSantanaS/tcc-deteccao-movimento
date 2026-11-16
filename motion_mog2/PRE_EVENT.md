# Pré-evento configurável e gravação por evento

O `main` converte segundos em contagens com `ceil(segundos * FPS)`, usando
`calculatePreEventFrameCount` de `src/config/PreEventConfig.cpp`. No caminho de
retenção, essa contagem é usada em pacotes codificados de vídeo. O MOG2 analisa
os frames decodificados na GPU; os MP4 são escritos por remux dos pacotes
originais preservados por `FfmpegEncodedVideoSource`, sem transcoding.

## Execução

Na raiz do repositório, depois do build:

```text
motion_mog2/build/motion_mog2 [fonte] [diretorio_saida] [pre_evento_s] [limiar_%] [inicio_frames] [fim_frames] [pos_evento_s] [rtsp_transporte]
```

Padrões: `rtsp://127.0.0.1:8554/video ./recordings 5 1.0 2 3 5 tcp`.
O transporte aceita `tcp`, `udp` ou `auto` e é ignorado para arquivo local.
Os tempos devem ser finitos e não negativos; use ponto para frações, como `2.5`.
O limiar deve ser finito e pertencer a `(0, 100]`; os contadores de início e fim
são inteiros positivos.

Exemplo com arquivo local e pós-evento de cinco segundos:

```bash
motion_mog2/build/motion_mog2 "$HOME/Vídeos/tcc/video_h264_fhd_60fps.mp4" /tmp/tcc_eventos 5 1.0 2 3 5
```

O inicializador RTSP para arquivo ou webcam é:

```bash
RTSP_TRANSPORT=tcp SHOW_PREVIEW=0 bash run_rtsp_recording.sh "$HOME/Vídeos/tcc/video_h264_fhd_60fps.mp4" /tmp/tcc_eventos 5
```

Ele preserva os logs em uma pasta `.rtsp-run.*` dentro do diretório de saída,
incluindo `recording.log`. Ctrl+C encerra os processos iniciados pelo script.

O inicializador `run_mog2_test.sh` também permanece disponível:

```text
bash run_mog2_test.sh [video-ou-pasta] [pre_event_seconds] [motion_threshold_percent] [motion_start_frames] [motion_end_frames]
```

Nesse script, o pré-evento padrão é zero. O primeiro argumento aceita arquivo
ou pasta, com seleção interativa quando há vários vídeos. `VIDEO_DIR` altera a
pasta padrão (`~/Vídeos/tcc`); `OPENCV_DIR` e `MEDIAMTX_BIN` alteram os caminhos
de instalação. A chamada ao executável inclui fonte, diretório
`.run_mog2/recordings`, pré-evento, limiar e contadores, nessa ordem. Pós-evento
e transporte usam os padrões do executável. O log é `.run_mog2/motion_mog2.log`,
sobrescrito em cada execução.

## Fila única e fronteira

`EncodedPacketQueue` pertence à fonte FFmpeg e mantém uma `std::deque<AvPacketPtr>`
protegida por mutex. `pushPacket()` insere no final sem avançar a fronteira
`counted_packet_count_`. A região anterior à fronteira contém os pacotes já
contabilizados; a região posterior contém os pacotes lidos antecipadamente.

`VideoStreamReader` configura `allowFrameDrop = false`, `rawMode = true` e
`udpSource = true`. A cada `read()` bem-sucedido, recupera um frame na GPU e
consulta `PROP_NUMBER_OF_RAW_PACKAGES_SINCE_LAST_GRAB` para chamar
`advanceCountedPackets(N)`. Esse N pode ser diferente de um: o índice do frame
decodificado não substitui a fronteira dos pacotes.

Retiradas e descartes normais atuam somente na região contabilizada. No
encerramento, o `main` chama `stopDecoding()` antes de `takeAllPackets()`; esse
último método também retira os pacotes ainda não contabilizados. A escrita dos
lotes ocorre fora do lock da fila, por `EventRecorder` e `Mp4EventPacketSink`.

## Janela de retenção e limites

O `main` calcula:

```text
pre_event_packet_count = ceil(pre_evento_s * FPS)
start_margin_packet_count = 32 + inicio_frames - 1
lookback_packet_count = pre_event_packet_count + start_margin_packet_count
maximum_idle_packet_count = lookback_packet_count + ceil(20 * FPS)
post_event_packet_count = ceil(pos_evento_s * FPS)
```

A margem é a escolhida no código para cobrir o avanço do decoder e os frames
usados para confirmar o início. Os 20 segundos são uma suposição sobre o GOP,
pendente de ajuste para a câmera real.

Sem início de movimento, `EventRecorder` apara a região contabilizada por GOPs,
preservando pelo menos o `lookback` quando possível. O teto prevalece sobre esse
mínimo: pode descartar um GOP e, sem outro keyframe, esvazia a região
contabilizada. Esse descarte gera `IDLE_CEILING_DISCARDED`.

Ao iniciar o evento, `takeCountedPacketsFromLookback()` busca o último keyframe
até `max(counted_packet_count - lookback, 0)`. Sem um keyframe nessa faixa,
aceita o primeiro posterior disponível na região contabilizada, com histórico
menor. Sem nenhum keyframe, descarta essa região e o recorder passa a
`waiting_keyframe`. Se o movimento terminar durante a espera, descarta o evento.
Mesmo com pré-evento zero, a margem e o alinhamento ao keyframe permanecem.

Durante o evento, os lotes contabilizados seguem para o MP4. Após o fim do
movimento, o recorder conta os pacotes escritos de pós-evento; a contagem pode
chegar a zero no meio de um lote, que é escrito inteiro. Depois fecha antes do
próximo keyframe, deixando esse keyframe na fila. Um novo início durante o
pós-evento ou a espera de fechamento retoma o evento no mesmo arquivo.

A contagem não é um teto de bytes nem uma garantia de duração exata: histórico
inicial insuficiente, descarte pelo teto e alinhamento ao GOP afetam o trecho.
A fonte também limita a região não contabilizada a `ceil(FPS * 40) + 32`
pacotes por padrão; atingir esse teto encerra a leitura como `decoder_stalled`
(código 6). Nenhum desses limites representa um teto global de memória em bytes.

## Logs atuais

A aproximadamente 59,94 FPS, com pré-evento e pós-evento de cinco segundos e
`inicio_frames=2`:

```text
PRE_EVENT seconds=5 packets=300 margin_packets=33 idle_cap_packets=1532
POST_EVENT seconds=5 packets=300
```

`MOTION_STARTED` e `MOTION_ENDED` registram os índices analisados pelo detector.
`PRE_EVENT_WRITTEN packets=N` conta o lote inicial efetivamente escrito.
`EVENT_WAITING_KEYFRAME` e `EVENT_DISCARDED_NO_KEYFRAME` indicam espera e descarte.
`MP4_STARTED` e `MP4_SAVED` registram os arquivos; `encoded_packets` conta os
pacotes escritos, enquanto `decoded_reads` conta leituras durante a gravação.
`CAPTURE_END` informa o motivo do encerramento; `CAPTURE_COMPLETE` inclui o
contador `recordings_without_idr` da observabilidade de E11.

## Testes da conversão sem CUDA

`pre_event_tests` usa apenas `tests/pre_event_tests.cpp` e
`src/config/PreEventConfig.cpp`. O alvo é registrado no CTest quando
`BUILD_TESTING=ON`:

```bash
ctest --test-dir motion_mog2/build -R '^pre_event_tests$' --output-on-failure
```

O teste cobre conversão, arredondamento, valores fracionários, entradas
inválidas e estouro de contagem. A retenção e a máquina de estados são testadas
separadamente por `encoded_packet_queue_tests` e `event_recorder_tests`.
Esses testes sintéticos não comprovam a decodificação nem o funcionamento RTSP.

## Decisões e limitações da execução do plano v2

As medições abaixo foram registradas nas etapas anteriores e fornecidas para o alinhamento documental; esta etapa não as repete.

- E06: carimbo inválido durante o movimento perde até um GOP; se o movimento terminar antes do próximo keyframe, o pós-evento também se perde (`src/recording/EventRecorder.cpp`, transição para `waiting_keyframe`).
- E07: queda abrupta da fonte pode deixar o último quadro do MP4 incompleto (medido: 2 de 3 quedas; sem o último pacote, os arquivos ficam sãos).
- E08 adiada: item 2.5 não reproduzível com o MediaMTX local (SPS chega embutido: MED-1 PUB-NOSPS=5, webcam real=3); relevante só para câmera ligada direto que envie SPS/PPS apenas no SDP; E09 cobre a consequência (`decoder_stalled`, código 6).
- E10: medido em G1, PUB-COPY e webcam real: `key_without_idr=0` em todas; corte pela flag KEY mantido. O utilitário `H264NalUnits` foi antecipado da E08.
- E11: só observabilidade (`CUT_POINT_WITHOUT_IDR` e contador); regra rígida de IDR pendente de câmera real.
