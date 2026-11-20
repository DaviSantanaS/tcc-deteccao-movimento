# Caminho de um pacote codificado — commit 51c0e84

Linhas conferidas no código deste commit. Caminhos relativos a `motion_mog2/src/`.

```
RTSP (MediaMTX/câmera)
└─ av_read_frame()                              input/FfmpegEncodedVideoSource.cpp:218
   │  [thread interna do OpenCV, dentro de getNextPacket()]
   │  Lê para current_encoded_packet_ (buffer reutilizado a cada chamada)
   │
   ├─ ✗ Descartado: outro stream, sem dados ou size<=0     :231-236 (unref + continue)
   ├─ ✗ Erro ou EOF: retorna false → fim da leitura          :227-229 (limitação 7)
   │
   └─ Pacote de vídeo válido
      │  time_base = time_base do stream                    :238
      │
      ├─ Cópia A → NVDEC (somente decodificação)
      │  └─ filterH264PacketToAnnexB()                      :241 → ffmpeg/AvBitstreamFilter.cpp:68
      │     │  O filtro recebe um clone do pacote          AvBitstreamFilter.cpp:76
      │     │  e precisa produzir exatamente 1 pacote      :245-250 (senão, false)
      │     └─ current_encoded_packet_for_nvdec_            :255
      │        └─ data/size entregues ao OpenCV            :256-257
      │           └─ parser do OpenCV → NVDEC → frame na GPU
      │              ├─ frame → read() → MOG2               main.cpp:240-248
      │              └─ O NVDEC informa N (PROP_NUMBER_OF_RAW_PACKAGES_SINCE_LAST_GRAB)
      │                 └─ advanceCountedPackets(N)         input/VideoStreamReader.cpp:69-80
      │                    (a cópia A não é guardada; ela é liberada na
      │                     próxima chamada, em :214)
      │
      └─ Cópia B → fila (original, em AVCC, preservado para o MP4)
         └─ cloneAvPacket()                                  :252 → ffmpeg/AvPacket.cpp:20
            └─ pushPacket()                                  :253 → buffers/EncodedPacketQueue.cpp:14
               │  Entra no FIM da deque, na região à frente
               │  (o lock do mutex vale só durante o push_back)
               │
               │  ── fronteira ── atravessada quando a Cópia A é contada (N)
               │
               └─ Região contabilizada   [thread principal, main.cpp]
                  │
                  ├─ Sem evento: trimCountedPackets()                main.cpp:300
                  │  ├─ ✗ Não-keyframes na frente: descartados       EncodedPacketQueue.cpp:70
                  │  ├─ ✗ GOP inteiro, se sobrar >= lookback         :73-76
                  │  ├─ ✗ GOP acima do teto (sem log)                :81-87 (limitação 9)
                  │  └─ ✓ Permanece na fila (pré-evento candidato)
                  │
                  ├─ MOTION_STARTED: takeCountedPacketsFromLookback()  main.cpp:262
                  │  ├─ ✗ Antes do keyframe escolhido: descartado     :123
                  │  ├─ ✗ Sem keyframe: tudo descartado → WAITING_KEYFRAME  :118-121
                  │  └─ ✓ Movido para std::vector (lote) → lock liberado
                  │
                  ├─ Durante o evento: takeCountedPackets()          main.cpp:282
                  │  └─ ✓ Lote (normalmente N pacotes por read())
                  │
                  ├─ Ctrl+C com MP4 aberto: takeCountedPackets()     main.cpp:307
                  │  └─ ✗ A região à frente fica na fila e se perde (limitação 5)
                  │
                  └─ Lote → encoded_packet_writer()                 main.cpp:214-226
                     │  1º lote: a frente deve ser keyframe         :218-222
                     └─ writeMp4Packets()                           ffmpeg/AvMp4OutputContext.cpp:240 → :177
                        ├─ ✗ pts/dts ausente → exceção → MP4 sem trailer  :216-222 (limitações 6 e 1)
                        └─ Para cada pacote:
                           ├─ prepareEncodedPacketForOutput()       ffmpeg/AvPacket.cpp:30
                           │  rescale_ts, stream_index, pos = -1   :44-49
                           └─ av_interleaved_write_frame()          AvMp4OutputContext.cpp:162
                              └─ arquivo .mp4 (remux; os bytes do pacote não mudam)
                                 └─ moov só em av_write_trailer()   :271, chamado por recording_finalizer
```

## Pontos-chave

- **Um pacote, duas cópias.** A cópia A em Annex B serve só para decodificar e não é guardada. A cópia B, no formato original, é a que vai para o MP4.
- **A fronteira liga as duas cópias.** O pacote B só pode ser retirado da fila depois que o decoder conta a cópia A correspondente (N). Isso só funciona porque cada `pushPacket` corresponde a exatamente um pacote entregue ao NVDEC (verificação em `FfmpegEncodedVideoSource.cpp:245-250`).
- **Duas threads, uma em cada ponta.** A thread do OpenCV só insere no fim da fila. A thread principal só retira ou descarta na frente, dentro da região contabilizada.

## Não verificado

`av_packet_clone` (`AvPacket.cpp:21`) cria uma nova referência ao mesmo buffer
quando o pacote de origem já é refcountado (documentação do FFmpeg). Não conferi
se o buffer do `av_read_frame` é sempre refcountado; por isso não afirmo se a
cópia B duplica ou compartilha os bytes.

As limitações citadas estão em `LIMITACOES_51c0e84.md`.
