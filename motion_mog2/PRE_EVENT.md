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

## Formato do MP4

O gravador configura MP4 fragmentado com
`movflags=frag_keyframe+empty_moov+default_base_moof`
(`src/ffmpeg/AvMp4OutputContext.cpp:59-61`), `frag_duration=1000000`
microssegundos (1 s) e `flush_packets=1`. A função recebe a duração e a aplica
em `src/ffmpeg/AvMp4OutputContext.cpp:65-73`; o valor `1'000'000` é fornecido
pelo sink em `src/recording/Mp4EventPacketSink.cpp:103-104`.

O fechamento normal chama `av_write_trailer()`
(`src/recording/Mp4EventPacketSink.cpp:139-151` e
`src/ffmpeg/AvMp4OutputContext.cpp:304`). `kill -9` (SIGKILL) e queda de energia
não executam esse fechamento nem o destrutor. Por isso a gravação usa
fragmentos, que guardam pacotes e metadados juntos durante a escrita, e descarga
de I/O por pacote. Isso não garante persistência em queda de energia.
A descrição do formato está na [documentação do muxer MP4 do FFmpeg 6.1](https://github.com/FFmpeg/FFmpeg/blob/n6.1/doc/muxers.texi#L1490-L1539),
e a descarga por pacote na [documentação de AVFormatContext::flush_packets](https://ffmpeg.org/doxygen/6.1/structAVFormatContext.html).

Medição da E05, com GOP de 600 pacotes: SIGKILL após 900 pacotes recuperou 840
com `frag_duration` de 1 s, contra 600 com fragmentação só por keyframe; após
300 pacotes, ainda dentro do primeiro GOP, recuperou 240, contra 0 só por
keyframe (`specs/plano_mudancas_v2.md:140-145`). Players verificados na E05,
conforme informado na revisão documental: Totem 43.0, Chrome 153,
ffplay 6.1.1 e GStreamer 1.24.2. Estas medições e verificações não foram
repetidas nesta revisão.

## Fim da captura e códigos de saída

`CAPTURE_END` registra `reason`, `av_error` e `detail`
(`src/main.cpp:170-176`). Os nomes vêm de `getCaptureEndReasonName()`
(`src/app/CaptureEnd.cpp:40-56`); os códigos dos motivos vêm de
`getExitCodeForCaptureEnd()` (`src/app/CaptureEnd.cpp:23-38`), chamado pelo
retorno de `src/main.cpp:190`.

| `CAPTURE_END reason` ou caminho antecipado | Código | Condição e referência |
|---|---|---|
| `file_end` | 0 | Fim de stream em arquivo regular (`src/app/CaptureEnd.cpp:8-9`, `:25-27`). |
| `interrupted` | 0 | Sinal de encerramento ou leitura interrompida (`src/app/CaptureEnd.cpp:4-5`, `:10-11`, `:26-27`). |
| `live_source_closed` | 3 | Fim de stream em fonte classificada como ao vivo (`src/app/CaptureEnd.cpp:8-9`, `:28-29`). |
| `read_error` | 4 | Erro de leitura, motivo ainda `not_ended` ou fallback (`src/app/CaptureEnd.cpp:16-20`, `:30-31`, `:37`). |
| `packet_error` | 5 | Erro na preparação do pacote (`src/app/CaptureEnd.cpp:12-13`, `:32-33`). |
| `decoder_stalled` | 6 | Fonte encerrou por falta de contabilização pelo decoder (`src/app/CaptureEnd.cpp:14-15`, `:34-35`). |
| Sem `reason` próprio: exceção com `[fatal]` | 2 | Exceção de CUDA/NVDEC ou da biblioteca padrão, incluindo argumentos inválidos no parser (`src/main.cpp:52`, `:182-187`). |
| Sem `reason`: nenhuma GPU CUDA visível, com `[fatal]` | 1 | Retorno antecipado (`src/main.cpp:56-58`). |
| Sem `reason`: excesso de argumentos | 1 | Mais de nove argumentos contando o executável; imprime uso (`src/main.cpp:39-44`). |
| Sem `reason`: `--help` ou `-h` | 0 | Pedido de ajuda como único argumento; imprime uso (`src/main.cpp:37-44`). |

SIGINT e SIGTERM usam o handler que coloca `capture_active` em falso
(`src/main.cpp:32-34`, `:47-48`). Na classificação, esse sinal prevalece sobre
o motivo da fonte: `interrupted_by_signal = !::capture_active`
(`src/main.cpp:166`) é consultado antes do switch
(`src/app/CaptureEnd.cpp:4-6`). Isso não substitui os retornos antecipados nem
o código 2 de uma exceção, inclusive se ela ocorrer na finalização depois de
`CAPTURE_END` (`src/main.cpp:174-190`).

O critério de origem é `std::filesystem::is_regular_file(input_source,
file_status_error)`: arquivo regular é tratado como arquivo; o resultado
negado classifica a fonte como ao vivo, incluindo quando a consulta falha.
Não é uma distinção feita apenas pelo prefixo RTSP (`src/main.cpp:167-169`).

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

### Logs acrescentados pelo plano v2

- `MP4_SAVED_AFTER_ERROR path=... encoded_packets=N`: quando uma exceção
  interrompe a gravação, o destrutor do sink tenta finalizar o MP4 ainda
  aberto e com cabeçalho escrito. Esta linha indica que a finalização dos
  pacotes já escritos terminou sem exceção
  (`src/recording/Mp4EventPacketSink.cpp:56-68`).
- `MP4_DISCARDED_EMPTY path=...`: nesse mesmo caminho do destrutor, uma
  gravação com zero pacotes escritos tem os contextos liberados e o arquivo
  vazio removido (`src/recording/Mp4EventPacketSink.cpp:61-64`).
- `EVENT_INVALID_TIMESTAMP reason=... frame=... dropped_packets=N resumed=yes|no`:
  informa o primeiro problema do lote, o índice do frame decodificado em
  processamento, quantos pacotes foram descartados nesse lote e se a gravação
  ficou aberta após o tratamento (`src/recording/EventRecorder.cpp:253-267`).
  Os motivos são `missing_timestamp` (PTS ou DTS ausente), `pts_before_dts`
  (PTS menor que DTS) e `non_increasing_dts` (DTS menor ou igual ao último DTS
  escrito), conforme `src/recording/PacketTimestampCheck.cpp:14-21`.
- `CUT_POINT_WITHOUT_IDR path=... first_packet_pts=... first_packet_dts=...`:
  registra que o primeiro pacote do MP4, aceito pela flag KEY, não contém NAL
  tipo 5 (IDR); incrementa o contador `recordings_without_idr` mostrado em
  `CAPTURE_COMPLETE`, sem impor uma regra rígida de corte por IDR
  (`src/recording/Mp4EventPacketSink.cpp:120-131`, `src/main.cpp:179-181`).
- `IDLE_CEILING_DISCARDED packets=N ceiling_packets=T`: registra somente
  quando houve descarte pelo teto durante idle; `packets` é a parcela
  descartada pelo teto e `ceiling_packets` é o teto configurado
  (`src/recording/EventRecorder.cpp:58-63`).

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
