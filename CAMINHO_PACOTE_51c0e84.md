# Caminho de um pacote codificado — commit 51c0e84

Linhas conferidas no código deste commit. Caminhos relativos a `motion_mog2/src/`.
Cada referência abre a linha no GitHub, fixada no commit `51c0e84` (os números
não mudam com commits posteriores).

<pre>
RTSP (MediaMTX/câmera)
└─ av_read_frame()                              <a href="https://github.com/DaviSantanaS/tcc-deteccao-movimento/blob/51c0e84e3c930c07d7924a17c33a740f25fbe04c/motion_mog2/src/input/FfmpegEncodedVideoSource.cpp#L218">input/FfmpegEncodedVideoSource.cpp:218</a>
   │  [thread interna do OpenCV, dentro de getNextPacket()]
   │  Lê para current_encoded_packet_ (buffer reutilizado a cada chamada)
   │
   ├─ ✗ Descartado: outro stream, sem dados ou size&lt;=0     <a href="https://github.com/DaviSantanaS/tcc-deteccao-movimento/blob/51c0e84e3c930c07d7924a17c33a740f25fbe04c/motion_mog2/src/input/FfmpegEncodedVideoSource.cpp#L231-L236">:231-236</a> (unref + continue)
   ├─ ✗ Erro ou EOF: retorna false → fim da leitura          <a href="https://github.com/DaviSantanaS/tcc-deteccao-movimento/blob/51c0e84e3c930c07d7924a17c33a740f25fbe04c/motion_mog2/src/input/FfmpegEncodedVideoSource.cpp#L227-L229">:227-229</a> (limitação 7)
   │
   └─ Pacote de vídeo válido
      │  time_base = time_base do stream                    <a href="https://github.com/DaviSantanaS/tcc-deteccao-movimento/blob/51c0e84e3c930c07d7924a17c33a740f25fbe04c/motion_mog2/src/input/FfmpegEncodedVideoSource.cpp#L238">:238</a>
      │
      ├─ Cópia A → NVDEC (somente decodificação)
      │  └─ filterH264PacketToAnnexB()                      <a href="https://github.com/DaviSantanaS/tcc-deteccao-movimento/blob/51c0e84e3c930c07d7924a17c33a740f25fbe04c/motion_mog2/src/input/FfmpegEncodedVideoSource.cpp#L241">:241</a> → <a href="https://github.com/DaviSantanaS/tcc-deteccao-movimento/blob/51c0e84e3c930c07d7924a17c33a740f25fbe04c/motion_mog2/src/ffmpeg/AvBitstreamFilter.cpp#L68">ffmpeg/AvBitstreamFilter.cpp:68</a>
      │     │  O filtro recebe um clone do pacote          <a href="https://github.com/DaviSantanaS/tcc-deteccao-movimento/blob/51c0e84e3c930c07d7924a17c33a740f25fbe04c/motion_mog2/src/ffmpeg/AvBitstreamFilter.cpp#L76">AvBitstreamFilter.cpp:76</a>
      │     │  e precisa produzir exatamente 1 pacote      <a href="https://github.com/DaviSantanaS/tcc-deteccao-movimento/blob/51c0e84e3c930c07d7924a17c33a740f25fbe04c/motion_mog2/src/input/FfmpegEncodedVideoSource.cpp#L245-L250">:245-250</a> (senão, false)
      │     └─ current_encoded_packet_for_nvdec_            <a href="https://github.com/DaviSantanaS/tcc-deteccao-movimento/blob/51c0e84e3c930c07d7924a17c33a740f25fbe04c/motion_mog2/src/input/FfmpegEncodedVideoSource.cpp#L255">:255</a>
      │        └─ data/size entregues ao OpenCV            <a href="https://github.com/DaviSantanaS/tcc-deteccao-movimento/blob/51c0e84e3c930c07d7924a17c33a740f25fbe04c/motion_mog2/src/input/FfmpegEncodedVideoSource.cpp#L256-L257">:256-257</a>
      │           └─ parser do OpenCV → NVDEC → frame na GPU
      │              ├─ frame → read() → MOG2               <a href="https://github.com/DaviSantanaS/tcc-deteccao-movimento/blob/51c0e84e3c930c07d7924a17c33a740f25fbe04c/motion_mog2/src/main.cpp#L240-L248">main.cpp:240-248</a>
      │              └─ O NVDEC informa N (PROP_NUMBER_OF_RAW_PACKAGES_SINCE_LAST_GRAB)
      │                 └─ advanceCountedPackets(N)         <a href="https://github.com/DaviSantanaS/tcc-deteccao-movimento/blob/51c0e84e3c930c07d7924a17c33a740f25fbe04c/motion_mog2/src/input/VideoStreamReader.cpp#L69-L80">input/VideoStreamReader.cpp:69-80</a>
      │                    (a cópia A não é guardada; ela é liberada na
      │                     próxima chamada, em <a href="https://github.com/DaviSantanaS/tcc-deteccao-movimento/blob/51c0e84e3c930c07d7924a17c33a740f25fbe04c/motion_mog2/src/input/FfmpegEncodedVideoSource.cpp#L214">:214</a>)
      │
      └─ Cópia B → fila (original, em AVCC, preservado para o MP4)
         └─ cloneAvPacket()                                  <a href="https://github.com/DaviSantanaS/tcc-deteccao-movimento/blob/51c0e84e3c930c07d7924a17c33a740f25fbe04c/motion_mog2/src/input/FfmpegEncodedVideoSource.cpp#L252">:252</a> → <a href="https://github.com/DaviSantanaS/tcc-deteccao-movimento/blob/51c0e84e3c930c07d7924a17c33a740f25fbe04c/motion_mog2/src/ffmpeg/AvPacket.cpp#L20">ffmpeg/AvPacket.cpp:20</a>
            └─ pushPacket()                                  <a href="https://github.com/DaviSantanaS/tcc-deteccao-movimento/blob/51c0e84e3c930c07d7924a17c33a740f25fbe04c/motion_mog2/src/input/FfmpegEncodedVideoSource.cpp#L253">:253</a> → <a href="https://github.com/DaviSantanaS/tcc-deteccao-movimento/blob/51c0e84e3c930c07d7924a17c33a740f25fbe04c/motion_mog2/src/buffers/EncodedPacketQueue.cpp#L14">buffers/EncodedPacketQueue.cpp:14</a>
               │  Entra no FIM da deque, na região à frente
               │  (o lock do mutex vale só durante o push_back)
               │
               │  ── fronteira ── atravessada quando a Cópia A é contada (N)
               │
               └─ Região contabilizada   [thread principal, main.cpp]
                  │
                  ├─ Sem evento: trimCountedPackets()                <a href="https://github.com/DaviSantanaS/tcc-deteccao-movimento/blob/51c0e84e3c930c07d7924a17c33a740f25fbe04c/motion_mog2/src/main.cpp#L300">main.cpp:300</a>
                  │  ├─ ✗ Não-keyframes na frente: descartados       <a href="https://github.com/DaviSantanaS/tcc-deteccao-movimento/blob/51c0e84e3c930c07d7924a17c33a740f25fbe04c/motion_mog2/src/buffers/EncodedPacketQueue.cpp#L70">EncodedPacketQueue.cpp:70</a>
                  │  ├─ ✗ GOP inteiro, se sobrar &gt;= lookback         <a href="https://github.com/DaviSantanaS/tcc-deteccao-movimento/blob/51c0e84e3c930c07d7924a17c33a740f25fbe04c/motion_mog2/src/buffers/EncodedPacketQueue.cpp#L73-L76">:73-76</a>
                  │  ├─ ✗ GOP acima do teto (sem log)                <a href="https://github.com/DaviSantanaS/tcc-deteccao-movimento/blob/51c0e84e3c930c07d7924a17c33a740f25fbe04c/motion_mog2/src/buffers/EncodedPacketQueue.cpp#L81-L87">:81-87</a> (limitação 9)
                  │  └─ ✓ Permanece na fila (pré-evento candidato)
                  │
                  ├─ MOTION_STARTED: takeCountedPacketsFromLookback()  <a href="https://github.com/DaviSantanaS/tcc-deteccao-movimento/blob/51c0e84e3c930c07d7924a17c33a740f25fbe04c/motion_mog2/src/main.cpp#L262">main.cpp:262</a>
                  │  ├─ ✗ Antes do keyframe escolhido: descartado     <a href="https://github.com/DaviSantanaS/tcc-deteccao-movimento/blob/51c0e84e3c930c07d7924a17c33a740f25fbe04c/motion_mog2/src/buffers/EncodedPacketQueue.cpp#L123">:123</a>
                  │  ├─ ✗ Sem keyframe: tudo descartado → WAITING_KEYFRAME  <a href="https://github.com/DaviSantanaS/tcc-deteccao-movimento/blob/51c0e84e3c930c07d7924a17c33a740f25fbe04c/motion_mog2/src/buffers/EncodedPacketQueue.cpp#L118-L121">:118-121</a>
                  │  └─ ✓ Movido para std::vector (lote) → lock liberado
                  │
                  ├─ Durante o evento: takeCountedPackets()          <a href="https://github.com/DaviSantanaS/tcc-deteccao-movimento/blob/51c0e84e3c930c07d7924a17c33a740f25fbe04c/motion_mog2/src/main.cpp#L282">main.cpp:282</a>
                  │  └─ ✓ Lote (normalmente N pacotes por read())
                  │
                  ├─ Ctrl+C com MP4 aberto: takeCountedPackets()     <a href="https://github.com/DaviSantanaS/tcc-deteccao-movimento/blob/51c0e84e3c930c07d7924a17c33a740f25fbe04c/motion_mog2/src/main.cpp#L307">main.cpp:307</a>
                  │  └─ ✗ A região à frente fica na fila e se perde (limitação 5)
                  │
                  └─ Lote → encoded_packet_writer()                 <a href="https://github.com/DaviSantanaS/tcc-deteccao-movimento/blob/51c0e84e3c930c07d7924a17c33a740f25fbe04c/motion_mog2/src/main.cpp#L214-L226">main.cpp:214-226</a>
                     │  1º lote: a frente deve ser keyframe         <a href="https://github.com/DaviSantanaS/tcc-deteccao-movimento/blob/51c0e84e3c930c07d7924a17c33a740f25fbe04c/motion_mog2/src/main.cpp#L218-L222">:218-222</a>
                     └─ writeMp4Packets()                           <a href="https://github.com/DaviSantanaS/tcc-deteccao-movimento/blob/51c0e84e3c930c07d7924a17c33a740f25fbe04c/motion_mog2/src/ffmpeg/AvMp4OutputContext.cpp#L240">ffmpeg/AvMp4OutputContext.cpp:240</a> → <a href="https://github.com/DaviSantanaS/tcc-deteccao-movimento/blob/51c0e84e3c930c07d7924a17c33a740f25fbe04c/motion_mog2/src/ffmpeg/AvMp4OutputContext.cpp#L177">:177</a>
                        ├─ ✗ pts/dts ausente → exceção → MP4 sem trailer  <a href="https://github.com/DaviSantanaS/tcc-deteccao-movimento/blob/51c0e84e3c930c07d7924a17c33a740f25fbe04c/motion_mog2/src/ffmpeg/AvMp4OutputContext.cpp#L216-L222">:216-222</a> (limitações 6 e 1)
                        └─ Para cada pacote:
                           ├─ prepareEncodedPacketForOutput()       <a href="https://github.com/DaviSantanaS/tcc-deteccao-movimento/blob/51c0e84e3c930c07d7924a17c33a740f25fbe04c/motion_mog2/src/ffmpeg/AvPacket.cpp#L30">ffmpeg/AvPacket.cpp:30</a>
                           │  rescale_ts, stream_index, pos = -1   <a href="https://github.com/DaviSantanaS/tcc-deteccao-movimento/blob/51c0e84e3c930c07d7924a17c33a740f25fbe04c/motion_mog2/src/ffmpeg/AvPacket.cpp#L44-L49">:44-49</a>
                           └─ av_interleaved_write_frame()          <a href="https://github.com/DaviSantanaS/tcc-deteccao-movimento/blob/51c0e84e3c930c07d7924a17c33a740f25fbe04c/motion_mog2/src/ffmpeg/AvMp4OutputContext.cpp#L162">AvMp4OutputContext.cpp:162</a>
                              └─ arquivo .mp4 (remux; os bytes do pacote não mudam)
                                 └─ moov só em av_write_trailer()   <a href="https://github.com/DaviSantanaS/tcc-deteccao-movimento/blob/51c0e84e3c930c07d7924a17c33a740f25fbe04c/motion_mog2/src/ffmpeg/AvMp4OutputContext.cpp#L271">:271</a>, chamado por recording_finalizer
</pre>

## Pontos-chave

- **Um pacote, duas cópias.** A cópia A em Annex B serve só para decodificar e não é guardada. A cópia B, no formato original, é a que vai para o MP4.
- **A fronteira liga as duas cópias.** O pacote B só pode ser retirado da fila depois que o decoder conta a cópia A correspondente (N). Isso só funciona porque cada `pushPacket` corresponde a exatamente um pacote entregue ao NVDEC (verificação em <a href="https://github.com/DaviSantanaS/tcc-deteccao-movimento/blob/51c0e84e3c930c07d7924a17c33a740f25fbe04c/motion_mog2/src/input/FfmpegEncodedVideoSource.cpp#L245-L250">FfmpegEncodedVideoSource.cpp:245-250</a>).
- **Duas threads, uma em cada ponta.** A thread do OpenCV só insere no fim da fila. A thread principal só retira ou descarta na frente, dentro da região contabilizada.

## Não verificado

`av_packet_clone` (<a href="https://github.com/DaviSantanaS/tcc-deteccao-movimento/blob/51c0e84e3c930c07d7924a17c33a740f25fbe04c/motion_mog2/src/ffmpeg/AvPacket.cpp#L21">AvPacket.cpp:21</a>) cria uma nova referência ao mesmo buffer
quando o pacote de origem já é refcountado (documentação do FFmpeg). Não conferi
se o buffer do `av_read_frame` é sempre refcountado; por isso não afirmo se a
cópia B duplica ou compartilha os bytes.

As limitações citadas estão em `LIMITACOES_51c0e84.md`.
