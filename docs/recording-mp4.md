# Gravação orientada a movimento em MP4

MP4 é o contêiner do arquivo final; H.264 e H.265/HEVC são os codecs de vídeo
aceitos. A gravação usa *remux*: copia o vídeo comprimido dos segmentos MPEG-TS
para MP4 com `-c:v copy`, sem decodificar e recodificar o conteúdo.

O segmentador é um único processo FFmpeg que lê continuamente o RTSP. Ele não é
reiniciado entre eventos ou partes:

```bash
ffmpeg -rtsp_transport tcp -i rtsp://127.0.0.1:8554/video \
  -an -map 0:v:0 -c:v copy -f segment -segment_format mpegts \
  -segment_time 1 -break_non_keyframes 0 -reset_timestamps 0 \
  -segment_start_number 0 runtime/segments/segment_%012d.ts
```

O muxer de segmentos fecha os arquivos em keyframes porque
`-break_non_keyframes 0` proíbe cortes arbitrários. Em H.264 e HEVC, o requisito
operacional importante é um ponto de acesso aleatório: um frame marcado como
keyframe a partir do qual a decodificação pode começar. `pict_type=I` é uma
evidência complementar, mas nem todo conceito de I-frame descreve sozinho um
ponto de acesso aleatório. Criar um keyframe novo exigiria reencode e, portanto,
não é feito.

`max_fragment_seconds` limita cada MP4 final. O controlador mede com `ffprobe`
a duração real de cada segmento fechado, seleciona o maior prefixo contíguo que
caiba no limite e deixa o próximo segmento para a parte seguinte. Como os cortes
dependem dos keyframes fornecidos pela entrada, um único segmento maior que o
limite torna incompatíveis os requisitos de duração máxima, início em keyframe
e ausência de reencode; nesse caso a gravação falha explicitamente.

Todas as partes mantêm o mesmo `event_id` e recebem `part_index` crescente. Só a
primeira contém o pré-evento. Partes intermediárias usam
`fragment_reason=max_duration`; a última usa `motion_end` ou `shutdown`. O JSON
sidecar permanece necessário para relacionar partes, eventos MotionBus,
sequências consumidas, codec, validações e falhas.

## Publicação e validação

Cada parte é remuxada para um arquivo oculto `.mp4.part` no mesmo diretório:

```bash
ffmpeg -hide_banner -loglevel error \
  -i 'concat:/caminho/segment_0001.ts|/caminho/segment_0002.ts' \
  -map 0:v:0 -c:v copy -movflags +faststart -f mp4 -y \
  .event_<id>_part_0001.mp4.part
```

Como os temporários são MPEG-TS, o protocolo `concat:` une localmente os fluxos
TS fechados na ordem das sequências. `-reset_timestamps 0` mantém a linha
temporal contínua do único capturador entre os arquivos. Isso evita recriar
timestamps em cada fronteira, preserva a continuidade observada no fluxo real e
não abre o RTSP novamente.

`+faststart` move os metadados do MP4 para o início e melhora a abertura
progressiva; não é usado como prova de integridade. A implementação não força
uma tag HEVC como `hvc1`: o codec é copiado e a compatibilidade é verificada nos
testes reais.

Antes do rename atômico para o nome público `.mp4`, são executadas as seguintes
verificações:

```bash
ffprobe -v error -select_streams v:0 \
  -show_entries stream=codec_name:format=format_name,duration \
  -of json arquivo.mp4.part

ffprobe -v error -select_streams v:0 -read_intervals '%+#1' \
  -show_packets -show_entries packet=flags -of json arquivo.mp4.part

ffprobe -v error -select_streams v:0 -read_intervals '%+#1' \
  -show_frames -show_entries frame=key_frame,pict_type \
  -of json arquivo.mp4.part

ffmpeg -v error -i arquivo.mp4.part -map 0:v:0 -f null -
```

O arquivo só é publicado se o contêiner for MP4/MOV, houver exatamente uma
stream de vídeo H.264 ou HEVC com o mesmo codec do RTSP, a duração for positiva
e não exceder o limite, o primeiro pacote e frame forem keyframes, o primeiro
`pict_type` for `I` quando disponível e a decodificação integral terminar sem
erro.
