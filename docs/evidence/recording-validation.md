# Evidência de validação da gravação

Data: 2026-07-26
Commit-base testado: `476ae70e5379dd61a4bf6bf09dffb38667330246`,
com as alterações não commitadas descritas no diff desta etapa.

Os ensaios usaram MediaMTX e um único FFmpeg segmentador contínuo por sessão.
O detector foi substituído por um processo neutro e os eventos foram escritos
deterministicamente no único FIFO `/tmp/motion_bus`. Os arquivos finais foram
gerados pelo caminho real RTSP → MPEG-TS → MP4, sempre com `-c:v copy`.

## Configuração relevante

- contêiner final: MP4;
- segmentos temporários: MPEG-TS;
- `pre_event_seconds`: 2;
- `post_event_seconds`: 2;
- `segment_duration_seconds`: 1;
- `max_fragment_seconds`: 12 somente nos ensaios rápidos;
- `decode_timeout_seconds`: 180;
- configuração principal mantida com `max_fragment_seconds`: 90.

As fontes foram `video_h264_fhd_60fps.mp4` (H.264, 19,815411 s, em loop) e
`video_hevc_fhd_25fps.mp4` (HEVC, 55,157300 s, em loop).

## Comandos de validação

Para cada MP4:

```text
ffprobe -v error -select_streams v:0 \
  -show_entries stream=codec_name -show_entries format=duration,format_name \
  -of json arquivo.mp4

ffprobe -v error -select_streams v:0 -read_intervals '%+#1' \
  -show_packets -show_entries packet=flags -of json arquivo.mp4

ffprobe -v error -select_streams v:0 -read_intervals '%+#1' \
  -show_frames -show_entries frame=key_frame,pict_type -of json arquivo.mp4

ffmpeg -v error -i arquivo.mp4 -map 0:v:0 -f null -
sha256sum arquivo.mp4
```

## H.264

Evento `0d0b3e240aed48e08d224fa9e8dbb011`:

| JSON | Sequências | Duração (s) | SHA-256 do MP4 |
| --- | --- | ---: | --- |
| `event_20260726_140109_889_0d0b3e24_part_0001.json` | 0–10 | 11,041156 | `c5db8a48431a9e0791f44ac4173dff904b46178bc22d46719d135b0840132d33` |
| `event_20260726_140109_889_0d0b3e24_part_0002.json` | 11–17 | 7,735522 | `232298c954e6fd5af3481d645383ec7c23a9f9e9365eeaa84b4f1718a0f592d5` |

Em ambas as partes: `codec_name=h264`, primeiro pacote `K__`, primeiro frame
`key_frame=1` e `pict_type=I`. A decodificação integral retornou 0 com stderr
vazio.

## HEVC e shutdown

Evento `a5fd80dcb5a843cab396e774b9861f4c`:

| JSON | Sequências | Duração (s) | SHA-256 do MP4 |
| --- | --- | ---: | --- |
| `event_20260726_140207_508_a5fd80dc_part_0001.json` | 0–9 | 10,411178 | `acd6e04313eb85df1f1769ac27289e623f7a5feca7401bc5e872e9be37320d6a` |

O sidecar registra `codec=hevc`, `input_codec=hevc`,
`fragment_reason=shutdown`, `finalized_by_shutdown=true` e `status=completed`.
O primeiro pacote foi `K__`, o primeiro frame teve `key_frame=1` e
`pict_type=I`; a decodificação integral retornou 0 com stderr vazio.

## Três partes e retorno do movimento

Evento H.264 `37498518fe7d41bf872bb3c292f12b81`:

| Parte | Motivo | Sequências | Duração (s) | SHA-256 do MP4 |
| ---: | --- | --- | ---: | --- |
| 1 | `max_duration` | 0–10 | 11,041156 | `c5db8a48431a9e0791f44ac4173dff904b46178bc22d46719d135b0840132d33` |
| 2 | `max_duration` | 11–21 | 11,756578 | `501adadc63c029a0e9c310cfbdd28e4a341f1cf47e55c048ca277c79631f7d5c` |
| 3 | `motion_end` | 22–32 | 11,057822 | `bea89c4a617d0a657e046ac01abeed9d06edefc24ff7dfcaed5640d70036f8ea` |

Os JSONs são
`event_20260726_140256_583_37498518_part_0001.json`,
`event_20260726_140256_583_37498518_part_0002.json` e
`event_20260726_140256_583_37498518_part_0003.json`. As partes intermediárias
têm `part_count_known=null`; a final tem `part_count_known=3`.

O segundo `MOTION_ON` ocorreu 1,002 s após `MOTION_OFF`, dentro da janela
configurada de 2 s. O log registrou `POST_EVENT -> MOTION_ACTIVE`, manteve o
mesmo `event_id` e não repetiu nem perdeu sequências. Todas as partes tiveram
primeiro pacote `K__`, primeiro frame `key_frame=1`, `pict_type=I` e
decodificação integral com retorno 0 e stderr vazio.

Ao final de cada ensaio, o Orchestrator retornou 0 após SIGINT. A busca por
MediaMTX, publisher, segmentador, detector neutro e Orchestrator não encontrou
processos remanescentes. `git status` mostrou somente os arquivos-fonte,
testes, exemplos e documentação desta etapa, além da configuração local
preexistente e não versionada `orchestrator.webcam.json`.
