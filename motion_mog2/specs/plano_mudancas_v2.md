# Plano de mudanças — `motion_mog2/` (versão 2)

Base: repositório `DaviSantanaS/tcc-deteccao-movimento`, branch `feat/fila-unica`, commit `bc806b8`. Escopo: somente `motion_mog2/`. Este documento é plano; nenhum arquivo do repositório foi alterado.

**Seções alteradas na versão 2** (o que não está listado ficou como na versão 1)

| Seção | O que mudou |
|---|---|
| 0. Resumo executivo | Itens 1, 3 e 5 reescritos; item 9 novo. |
| 2. Itens confirmados | Linhas 2.4 e 2.5: nova medição do modo webcam. |
| 3. D1 | Confirmada (opção C). Critério (1) passa a ser os três players. Medição de `frag_duration` incluída; recomendação passa a incluir `frag_duration` de 1 s. Tabela do que fazer se um player falhar. |
| 3. D2, D3, D5 | Marcadas como confirmadas pelo Davi. Sem mudança de conteúdo. |
| 3. D4 | Reescrita: não há câmera alvo; mantém o corte pela flag `KEY` com observabilidade; IDR real vira pendência. |
| 4. Tabela de etapas | Título e tipo de E11. |
| 4.0 Preparos comuns | Vídeo G7 (GOP longo) e publicador PUB-WEBCAM-EQ; MED-1 com o valor medido para ele. |
| E05 | Reescrita: `frag_duration`, medição comparativa da perda, um caso de teste por player e por arquivo, regra de recuo, TASK. |
| E08 | Não pedida, mas necessária pela medição nova: uma nota, uma medição informativa (E08-T14) e um item a mais em OUT O2. Desenho e checks inalterados. |
| E09 | Valor de 40 s aprovado; texto do comentário obrigatório no código. |
| E10 | Mede as duas fontes disponíveis; ferramenta preparada para câmera futura; TASK. |
| E11 | Substituída: deixa de impor IDR; registra `CUT_POINT_WITHOUT_IDR` e o contador em `CAPTURE_COMPLETE`; TASK. |
| E14 | Dependências (E11 não edita mais o teste da fonte). |
| 5. Matriz | Linhas do item 2.4 (medir, observabilidade, regra rígida); intervalos de casos de teste de 2.1 e 2.5. |
| 6. Pendências | Nova pendência "IDR real como ponto de corte"; linha de `max_evento_s` ajustada. |
| 7. Riscos | Itens sobre E11, players e etapas que editam o mesmo teste. |

Convenções deste documento:

- **[Certo]** fato verificado em `arquivo:linha`, em documentação oficial ou por execução descrita aqui. **[Provável]** inferência com fundamento. **[Palpite]** hipótese sem evidência suficiente.
- `ocv:` aponta para `opencv_contrib` tag 4.10.0, pasta `modules/cudacodec/src`. `ff:` aponta para o FFmpeg tag n6.1.
- "Medido nesta análise" significa executado em um ambiente **sem GPU**, com FFmpeg 6.1.1 (pacote do Ubuntu) e MediaMTX v1.9.3, usando harnesses fora do repositório. Vale como evidência do comportamento do FFmpeg e do MediaMTX nessas versões, não do NVDEC nem da máquina do Davi.
- Termos: *remux* é copiar pacotes codificados de um contêiner para outro sem recodificar. *Keyframe* é o pacote marcado com `AV_PKT_FLAG_KEY`. *IDR* é o quadro H.264 a partir do qual a decodificação não depende de nada anterior (NAL tipo 5). *NAL* é a unidade em que o H.264 empacota cada parte do fluxo; SPS e PPS (NAL 7 e 8) são os conjuntos de parâmetros sem os quais o decoder não inicia. *GOP* é o trecho de um keyframe até o pacote anterior ao próximo. *Annex B* é o formato de H.264 com códigos de início `00 00 01`; *AVCC* é o formato com prefixo de tamanho usado em MP4. *Extradata* são os bytes de configuração do codec que o FFmpeg guarda fora dos pacotes. `dts` e `pts` são os carimbos de decodificação e de exibição de cada pacote; `AV_NOPTS_VALUE` indica carimbo ausente.

## 0. Resumo executivo

1. O plano tem 14 etapas, uma por commit, cobrindo os itens P1 a P6. As decisões D2, D3 e D5 estão confirmadas pelo Davi. D1 está confirmada com um critério de players que só fecha nos testes de E05. D4 foi revista porque não há câmera alvo.
2. Reordenei uma coisa em relação à prioridade pedida: a extração da máquina de estados e da histerese (P6) vem antes de P1. São três refatorações sem mudança de comportamento, e depois delas os casos de P1, P2, P5 passam a ter teste automático sem GPU. Justificativa e alternativa em D5.
3. P1 fica em três commits: finalizar o MP4 quando há exceção; gravar em MP4 fragmentado com fragmentos de até 1 s e descarga por pacote, para que um `kill -9` perca cerca de 1 s qualquer que seja o GOP; e uma política para pacote com carimbo inválido que nunca derruba o processo.
4. Medições novas mudaram o peso de três itens. O primeiro pacote de toda sessão RTSP chega sem `pts` e `dts`. Toda queda da fonte RTSP chega como `AVERROR_EOF`, igual ao fim de arquivo. Um MP4 do libx264 publicado com `-c copy` chega sem SPS/PPS no fluxo, que é exatamente o cenário do item 2.5.
5. P4 não impõe mais IDR. E10 mede as duas fontes disponíveis (arquivo via MediaMTX e webcam) e fica pronta para uma câmera futura. E11 mantém o corte pela flag `KEY` e registra quando um MP4 começa em keyframe sem IDR. A regra rígida fica como pendência até haver câmera real.
6. Cada etapa traz casos de teste, regressão obrigatória contra o baseline e um bloco TASK no Protocolo compacto do `AGENTS.md`.
7. Ficam fora: timeout de I/O, TEARDOWN, thread de escrita, `max_evento_s` e entrelaçado.
8. Nada aqui foi executado com GPU. As etapas que dependem do NVDEC têm um passo PRE que reproduz a falha antes de editar; se a falha não se reproduzir, a etapa para.
9. Medição nova na versão 2: a codificação do modo `--webcam` também chega sem SPS/PPS no fluxo. Se esse modo já funcionou com o gravador na máquina do Davi, o item 2.5 não se confirma e E08 para no PRE. É o teste mais barato desse item.

## 1. Histórico e decisões já tomadas

Esta seção descreve o que existe hoje e por que existe. Quem implementar deve tratar cada ponto como restrição.

### 1.1 Fila única (`EncodedPacketQueue`)

Antes havia três estruturas guardando pacotes: uma `deque` dentro da fonte FFmpeg, um vetor pendente no `main` e a classe `EncodedVideoBuffer`. Um mesmo pacote passava de uma para outra, e a regra de quem podia descartá-lo ficava espalhada.

Hoje existe uma só `std::deque<AvPacketPtr>` protegida por um mutex, com uma **fronteira** que divide a fila em duas regiões: da frente até a fronteira ficam os pacotes que o decoder já contabilizou; da fronteira até o fim ficam os que a thread de leitura já leu mas o decoder ainda não contabilizou (`include/buffers/EncodedPacketQueue.hpp:10-25`). A thread interna do OpenCV só insere (`pushPacket`). A thread principal avança a fronteira e só retira ou descarta da região contabilizada; a única exceção é `takeAllPackets()`, usada no encerramento.

A fronteira é um contador, e não um iterador, porque `push_back` em `std::deque` invalida iteradores, e a inserção acontece em outra thread.

Evidência registrada pelo Davi: paridade de `framemd5` depois da troca de estrutura.

### 1.2 `rawMode = true` e o valor N

O MOG2 analisa quadros decodificados; a gravação usa pacotes codificados. O que liga um ao outro é N, o valor de `PROP_NUMBER_OF_RAW_PACKAGES_SINCE_LAST_GRAB`: quantos pacotes o decoder consumiu desde a leitura anterior. A cada `read()`, `VideoStreamReader` lê N e chama `advanceCountedPackets(N)` (`src/input/VideoStreamReader.cpp:77-88`).

A alternativa "quadro k corresponde ao pacote k" foi rejeitada com medição: `av_read_frame` garante um quadro **codificado** por pacote, não um quadro **exibido** por pacote. Em RTSP, o primeiro `read()` consumiu 4 pacotes para entregar 1 quadro. Com `rawMode = false` o OpenCV não guarda os pacotes e N vale 0 sem avisar [Certo: `ocv: video_parser.cpp:78-79`, `video_reader.cpp:392-394`].

### 1.3 Corte por GOP, lookback e teto

Um MP4 só pode começar em keyframe, então a fila corta sempre em GOP inteiro. Ao confirmar um movimento, o `main` retira pacotes a partir de um keyframe situado pelo menos `lookback` pacotes atrás da fronteira. `lookback` = pré-evento + margem, e a margem é 32 + `inicio_frames` − 1 (`src/main.cpp:136-154`). O 32 existe porque o OpenCV esvazia toda a sua fila de pacotes crus a cada quadro retirado [Certo: `ocv: frame_queue.cpp:161-164`], então a fronteira pode estar até 32 superfícies de decodificação à frente do quadro analisado [Certo: `ocv: video_parser.cpp:141-142`]. O `inicio_frames − 1` cobre os quadros usados para confirmar o início.

Sem movimento, a fila é aparada a cada leitura, mantendo pelo menos `lookback` pacotes. O teto é `lookback` + 20 s de pacotes, e os 20 s são uma suposição sobre o maior GOP da câmera (`src/main.cpp:139-140, 155-157`).

### 1.4 Pós-evento e fechamento antes de keyframe

Depois que o movimento termina, a gravação continua por `pos_evento_s` e então fecha o arquivo imediatamente antes do próximo keyframe, deixando esse keyframe na fila. Sem isso, um movimento que voltasse logo depois do fim perdia o início: os pacotes seguintes dependiam de um keyframe que já estava no arquivo anterior e eram descartados.

### 1.5 Transporte RTSP configurável, padrão `tcp`

Com opções nulas o FFmpeg tentava UDP primeiro. Medido pelo Davi: TCP sem perdas; UDP com 396 pacotes perdidos numa rajada. A opção `rtsp_transport` só é aplicada a fontes `rtsp://` (`src/input/FfmpegEncodedVideoSource.cpp:75-90`).

### 1.6 Encerramento

`capture_active` é um `std::atomic<bool>` global escrito pelo handler de sinal. O mesmo ponteiro alimenta o callback de interrupção do FFmpeg, que faz `av_read_frame` retornar mesmo com a fonte parada (`src/input/FfmpegEncodedVideoSource.cpp:21-26, 96-99`). O `main` chama `stopDecoding()` antes de `takeAllPackets()`: destruir o leitor do OpenCV espera a thread de leitura terminar, então depois disso ninguém mais insere na fila (`src/input/VideoStreamReader.cpp:95-101`). Medido pelo Davi: cerca de 0,2 s para encerrar, inclusive com fonte travada, e os pacotes ainda não contabilizados entram no MP4 do evento em curso.

### 1.7 Invariantes que nenhuma etapa pode quebrar

| ID | Invariante |
|---|---|
| I1 | Fila única: nenhum outro contêiner de pacotes codificados no projeto. Lotes em trânsito para a escrita não contam como buffer. |
| I2 | `rawMode = true`. |
| I3 | Retirada e descarte só da região contabilizada; exceção única: `takeAllPackets()` no encerramento. |
| I4 | Escrita do MP4 fora do lock da fila. |
| I5 | Remux sem transcoding no caminho de gravação. Recodificar para **gerar vídeo de teste** é permitido. |
| I6 | Cada pacote entregue ao NVDEC corresponde a exatamente um pacote inserido na fila, na mesma ordem. É o que mantém N coerente. |
| I7 | Convenções do `AGENTS.md`: `snake_case`, membros privados com `_` no final, `this->` em métodos de instância, `std::` qualificado, globais com `::`, RAII. |

## 2. Itens confirmados da avaliação

| Item | Resumo | Status | O que mudou desde a avaliação |
|---|---|---|---|
| 2.1 | Exceção durante a gravação deixa o MP4 sem trailer e ilegível | [Certo], conferido pelo revisor e reproduzido | Nada. |
| 2.2 | Um pacote com carimbo inválido encerra o processo | Mecanismo [Certo]; ocorrência agora [Certo] neste arranjo | Medido: o primeiro pacote de toda sessão RTSP chega com `pts` e `dts` ausentes (TCP e UDP, em todas as 13 sessões medidas). Com quadros B, 3 pacotes chegaram sem `dts` (provavelmente os 3 primeiros; não conferi a posição). Em 45 s atravessando a emenda do `-stream_loop` não houve `dts` repetido. Na avaliação eu tinha marcado a ocorrência como [Palpite]. |
| 2.3 | Erro de leitura sai como fim normal, código 0 | [Certo], conferido | Medido: publicador encerrado com SIGTERM, publicador morto com SIGKILL, servidor morto e publicador suspenso por 10 s devolvem todos `AVERROR_EOF`. Ctrl+C devolve `AVERROR_EXIT`. O código de retorno sozinho não separa fim de arquivo de queda da fonte. |
| 2.4 | `AV_PKT_FLAG_KEY` não garante IDR | Mecanismo [Certo]; ocorrência na fonte alvo não medida | Não há câmera alvo. E10 mede as duas fontes disponíveis; E11 só registra a ocorrência (D4 revista). Medido na versão 2, com codificação equivalente à do modo webcam: 5 de 5 keyframes continham IDR. |
| 2.5 | Sem SPS/PPS no fluxo o decoder não inicia e a fila cresce | [Provável], conferido no fonte do OpenCV | Medido: MP4 do libx264 publicado com `-c copy` chega com 0 pacotes contendo SPS; a extradata (38 bytes, Annex B) vem só do SDP. O MediaMTX 1.9.3 não reinsere. Como o baseline RTSP do Davi funciona, o vídeo dele deve trazer SPS/PPS dentro do fluxo [Provável]; a medição MED-1 confirma. O travamento do NVDEC continua sem execução. Medido na versão 2: a codificação equivalente ao modo `--webcam` (libx264 `ultrafast` `zerolatency` para RTSP) também chega com 0 pacotes contendo SPS (MED-1 = 1). Se o modo webcam já funcionou com o gravador, o travamento não ocorre nessa máquina. |
| 2.6 | GOP acima da suposição: evento perdido sem aviso | [Certo], conta do teto conferida | Nada. |
| 2.8 | Limiar `nan` aceito | [Certo], conferido | Nada. |
| 2.10 | Máquina de estados e histerese sem teste sem GPU | [Certo] | Medido: uma simulação da lógica do `main` sobre a fila real, com N = 1 por leitura, reproduz os trechos do baseline ([0,779] e [780,1180]; com pós-evento 0, [0,239], [240,479], [540,1139]). Dá para testar sem GPU. |
| 2.11 e código morto | API legada, `PRE_EVENT.md` defasado, arquivos sem alvo | [Certo], conferido | Nada. |
| 2.7 | Sem timeout de I/O | Fora do plano | Medido: com MediaMTX o travamento vira `AVERROR_EOF` em cerca de 10 s, pelo `readTimeout` do servidor. Com câmera real não há esse limite. |
| 2.9 | TEARDOWN não enviado após Ctrl+C | Fora do plano | Medido: em UDP o servidor registra `session timed out`; em TCP a sessão cai junto com a conexão. |

## 3. Decisões D1 a D5

### D1 — Como não perder o MP4: finalizar na exceção, MP4 fragmentado, ou ambos

**Confirmada pelo Davi: opção C, condicionada ao critério (1) abaixo, que só fecha com os testes de players de E05.** E04 não depende desta decisão.

Opções:

| Opção | O que cobre | O que não cobre | Custo |
|---|---|---|---|
| A. Finalizar no caminho de exceção (RAII) | Qualquer exceção C++ durante o evento. O arquivo sai como MP4 comum. | `kill -9`, falha de segmentação, falta de energia: nenhum destrutor roda. | Pequeno; nenhuma mudança de formato. |
| B. MP4 fragmentado | Interrupção em qualquer ponto, inclusive `kill -9`. | Sozinho não basta: precisa de descarga por pacote (ver medições). Falta de energia continua sem garantia. | Muda o formato do arquivo. |
| C. Ambos | Exceção fecha o arquivo completo; morte abrupta perde no máximo o último fragmento. | Falta de energia. | A + B. |

Evidência sobre o formato:

- [Certo] A documentação do muxer diz que o arquivo fragmentado "é decodificável mesmo se a escrita for interrompida", que o MP4 normal "é indecodificável se não for finalizado" e que a desvantagem é ser "menos compatível com outras aplicações" (`ff: doc/muxers.texi:1626-1650`). `frag_keyframe` inicia um fragmento a cada keyframe (`:1666`). `empty_moov` grava um `moov` inicial sem amostras (`:1673-1680`); sem ele, o primeiro trecho é gravado como MP4 normal e só existe depois de completo. `default_base_moof` facilita a leitura dos fragmentos (`:1708-1714`).
- [Certo] `frag_duration` cria fragmentos com a duração dada em microssegundos (`ff: doc/muxers.texi:1657-1658`). Com mais de uma condição, o fragmento é cortado quando qualquer uma se cumpre (`:1647-1652`). No código, o corte acontece quando chega um pacote cujo `dts` está a pelo menos essa duração do primeiro pacote do fragmento, ou quando chega um keyframe (`ff: libavformat/movenc.c:6492-6500`).
- [Certo] Medido, 600 pacotes, GOP de 1 s: normal 187 897 bytes; fragmentado por keyframe 189 159 bytes (+0,7 %). `framemd5 -c copy` idêntico linha a linha, e os hashes coincidem com os pacotes [240,839] do fonte.
- [Certo] Medido: no fragmentado, o `ffprobe` não reporta `nb_frames` no cabeçalho (N/A); a contagem com `-count_frames` funciona. A duração reportada é a correta nos arquivos completos e nos interrompidos.
- [Certo] Com `avoid_negative_ts = MAKE_ZERO`, que o projeto já define (`src/main.cpp:190`), o muxer fragmentado desliga a lista de edição e desloca os carimbos (`ff: libavformat/movenc.c:7028-7039`). Medido: `start_time` 0 em arquivo iniciado no pacote 240.
- [Certo] `hybrid_fragmented` não existe no FFmpeg 6.1 (nenhuma ocorrência em `movenc.c`).

Evidência sobre a descarga:

- [Certo] Medido: fragmentado, processo morto com SIGKILL, **sem** descarga por pacote: arquivo com 28 bytes, ilegível. Causa: o protocolo `file` do FFmpeg acumula 262 144 bytes antes de escrever (`ff: libavformat/file.c:310-313`).
- [Certo] Medido: o mesmo com `AVFormatContext::flush_packets = 1` (`ff: libavformat/avformat.h:1528-1533`; descarga em `ff: libavformat/mux.c:438-440`): o arquivo fica legível até o último fragmento completo. MP4 normal com `flush_packets = 1` e SIGKILL continua ilegível.
- [Certo] Medido: arquivo fragmentado cortado no meio de um fragmento abre, mas o decoder acusa erro na última amostra. Com descarga por pacote o arquivo termina em fronteira de fragmento.

Evidência sobre `frag_duration` (medição pedida na versão 2). Vídeo H.264 de 1798 pacotes a ~59,94 fps com **GOP de 600 pacotes (10 s)**, gravado com as funções do projeto, `flush_packets = 1`, processo morto com SIGKILL:

| Cenário | Só `frag_keyframe` | `frag_keyframe` + `frag_duration` de 1 s |
|---|---|---|
| SIGKILL após 900 pacotes (1,5 GOP) | 600 recuperados; **300 perdidos (5 s)** | 840 recuperados; **60 perdidos (1 s)** |
| SIGKILL após 300 pacotes (dentro do 1º GOP) | Arquivo de 798 bytes, **sem nenhum quadro** | 240 recuperados; 60 perdidos |
| Primeiro pacote | keyframe | keyframe |
| Erros de decodificação | 0 | 0 |
| Arquivo completo, tamanho | 453 182 bytes (+0,08 % sobre o normal) | 456 611 bytes (+0,84 %) |
| `framemd5 -c copy` do arquivo completo | idêntico ao normal | idêntico ao normal |

Com GOP de 1 s (o do baseline) as duas configurações perdem a mesma coisa; a diferença aparece com GOP longo. Efeito lateral de `frag_duration`: fragmentos do meio do arquivo passam a começar em quadro que não é keyframe. O **arquivo** continua começando em keyframe, porque o primeiro fragmento começa no primeiro pacote escrito. Isso só importa para transmissão segmentada (DASH, HLS), que não é o uso aqui [Provável].

Evidência sobre players (sem tela, portanto parcial):

- [Certo] Medido com GStreamer 1.24.2 (`qtdemux` + `avdec_h264`, e `playbin` com saída descartada): arquivos completos (normal, fragmentado, fragmentado com `frag_duration`) decodificam os 1798 quadros sem erro; os interrompidos decodificam 600, 840 e 240 quadros sem erro. É o mesmo motor do GNOME Vídeos, mas não é o GNOME Vídeos.
- [Certo] Medido com `ffplay` 6.1.1 e driver de vídeo `dummy`: os arquivos interrompidos com `frag_duration` tocam até o fim e aceitam busca (`-ss`), código 0.
- [Palpite] Chrome e Edge com arquivo local: não testei.

Recomendação: **C, em dois commits**. E04 implementa A. E05 implementa B com `frag_keyframe+empty_moov+default_base_moof`, `frag_duration` de 1 s e `flush_packets = 1`.

Critérios que confirmam B, todos verificados em E05:

1. Os MP4 abrem e tocam do início ao fim em **`ffplay`**, no **player padrão do Linux (GNOME Vídeos/Totem, GStreamer)** e no **Chrome ou Edge com arquivo local**. Vale para um arquivo do baseline e para um arquivo produzido após `kill -9` (E05-T09 a T14).
2. `kill -9` durante evento produz arquivo são (E05-T05).
3. R3 e R4 dão os mesmos trechos.

Se o critério 1 falhar:

| O que falhou | O que fazer |
|---|---|
| Algum player falha com `frag_duration` e passa sem ele | Manter só `frag_keyframe` (duração 0) e documentar no código e no relatório qual player falhou e como. A perda em `kill -9` volta a ser de até um GOP. |
| Algum player falha mesmo sem `frag_duration` | Não aplicar E05. Ficar só com a opção A e registrar `kill -9` como limitação. |

### D2 — Política para pacote com carimbo inválido

**Confirmada pelo Davi: opção c (perder até um GOP; não fabricar carimbo).**

"Inválido" aqui é: `pts` ou `dts` ausente; `pts < dts`; ou `dts` menor ou igual ao do último pacote escrito no mesmo arquivo. São as três condições que o muxer recusa (`ff: libavformat/mux.c:567-583`).

Opções:

| Opção | Efeito | Problema |
|---|---|---|
| a. Descartar só o pacote | Arquivo continua | Quebra a cadeia de referência: os quadros seguintes do GOP dependem do descartado. |
| b. Corrigir o `dts` (último + 1) | Mantém todos os quadros | Fabrica carimbo. Não resolve carimbo ausente no primeiro pacote. Num recomeço da contagem, todos os pacotes seguintes recebem +1 e o trecho toca comprimido. |
| c. Fechar o arquivo antes do pacote e reabrir no próximo keyframe válido | Arquivos sempre válidos, nenhum carimbo inventado | Perde do pacote inválido até o próximo keyframe (no máximo um GOP). |
| d. Encerrar o processo | É o comportamento atual | Item 2.2. |

Evidência:

- [Certo] O único caso medido é o primeiro pacote da sessão RTSP. Ele quase sempre não é keyframe e é descartado pelo trim em `idle`. Vira falha quando a sessão começa exatamente num keyframe e um movimento é confirmado antes de esse GOP sair da fila [Provável].
- [Certo] A opção b tem precedente: o `ffmpeg` de linha de comando reescreve DTS não monotônico para o anterior + 1 com aviso (`ff: fftools/ffmpeg_mux.c:110-130`).
- [Palpite] `dts` repetido ou regressivo no meio de um evento com câmera real: não medido.

Onde validar: **na escrita**, dentro da máquina de estados, e não na entrada da fila. Dois motivos. A fila precisa receber todo pacote entregue ao NVDEC (I6), então não pode rejeitar. E "dts não crescente" depende do último pacote escrito no arquivo atual, estado que a fila não tem. A validação do muxer continua como última defesa.

Recomendação: **c**. É a única que nunca produz arquivo inválido nem carimbo inventado, reaproveita o estado `waiting_keyframe` que já existe e é simples de explicar. Regras em E06.

Critério que confirma: os testes E06 passam e, em operação normal com a fonte alvo, a linha `EVENT_INVALID_TIMESTAMP` não aparece no meio de eventos. Se aparecer de forma recorrente, reavaliar b para o caso específico de `dts` repetido, citando o precedente acima.

### D3 — Motivo do fim da leitura e códigos de saída

**Confirmada pelo Davi: códigos 3, 4, 5 e 6, com Ctrl+C em 0.** Conferido pelo Davi: `run_rtsp_recording.sh` só repassa o código do gravador (`exit "${recorder_exit_status}"`), e nenhum script testa valores específicos.

Restrição: `getNextPacket` devolve só `bool`, e o OpenCV trata `false` como fim de stream (`ocv: video_source.cpp:116-120`).

Opções para levar o motivo ao `main`:

| Opção | Avaliação |
|---|---|
| Estado atômico na fonte, consultado depois de `stopDecoding()` | Não toca a interface do OpenCV. `stopDecoding()` espera a thread terminar, então a leitura é segura. **Recomendada.** |
| Lançar exceção em `getNextPacket` | A exceção atravessaria a thread do OpenCV, que não a trata. Descartada. |
| Callback da fonte para o `main` | Executa na thread do OpenCV; cria um segundo canal de sincronização sem necessidade. |

Classificação recomendada, porque o código de retorno não separa os casos (medição em 2.3):

| Motivo final | Condição | Código de saída |
|---|---|---|
| `interrupted` | `::capture_active` falso | 0 |
| `file_end` | `AVERROR_EOF` e a fonte é um arquivo regular | 0 |
| `live_source_closed` | `AVERROR_EOF` e a fonte não é arquivo regular | 3 |
| `read_error` | outro retorno negativo de `av_read_frame` | 4 |
| `packet_error` | falha ao preparar o pacote (filtro, alocação) | 5 |
| `decoder_stalled` | teto da região à frente (E09) | 6 |

Códigos já existentes e mantidos: 1 para uso incorreto ou ausência de GPU (`src/main.cpp:87, 100`), 2 para exceção (`:365-371`).

Critério que confirma: os testes E07-T07 a T12 mostram o motivo e o código esperados em cada cenário, e `run_rtsp_recording.sh` continua funcionando (ele repassa o código do gravador).

### D4 — O que conta como ponto de corte

**Revista na versão 2 e decidida pelo Davi: manter o corte pela flag `KEY` e acrescentar observabilidade. A regra rígida (IDR real) fica como pendência condicionada à medição numa câmera real.**

Fato novo: não há câmera IP. As fontes disponíveis são o MediaMTX publicando arquivo com `-c copy` e a webcam codificada pelo FFmpeg (`run_rtsp_recording.sh --webcam`: libx264, `ultrafast`, `zerolatency`, `-g 60`, sem quadros B).

Opções:

| Opção | Critério | Consequência |
|---|---|---|
| a. Flag `KEY` (atual) | O que o parser do FFmpeg marcar | Aceita quadro não-IDR; em intra-refresh o início do MP4 não decodifica (item 2.4). |
| b. IDR real | Pacote com flag `KEY` que contém NAL tipo 5 | Todo MP4 começa em ponto independente. Em câmera sem IDR periódico nenhum evento seria gravado. |
| c. IDR ou recovery point com contagem zero | Exige interpretar a mensagem SEI | Mais código de análise de bitstream. |

Por que não impor b agora: sem a câmera alvo não dá para saber se ela tem IDR periódico. Impor IDR sem essa medição troca um defeito visível (início do arquivo com quadros perdidos) por um pior (nenhuma gravação). Nas duas fontes disponíveis o problema não deve aparecer: medido com codificação equivalente à da webcam, 5 de 5 keyframes continham IDR; e arquivos gravados com IDR periódico se comportam da mesma forma [Provável; E10 mede o vídeo do Davi].

Decisão:

- **E10** mede as duas fontes disponíveis e deixa a ferramenta pronta para uma câmera futura.
- **E11** mantém a opção a e registra, uma vez por arquivo, quando um MP4 começa em keyframe sem NAL 5, com um contador em `CAPTURE_COMPLETE`. O defeito do item 2.4 continua possível, mas deixa de ser silencioso.
- **Pendência:** adotar b (ou c) depois de rodar `h264_cut_point_survey` numa câmera real.

Critério para reabrir a decisão:

| Resultado numa câmera real | Ação |
|---|---|
| `key_without_idr = 0` | Nada a fazer; a opção a é suficiente para essa câmera. |
| `key_without_idr > 0` e há IDR com intervalo abaixo do teto de idle | Planejar a opção b. |
| `key_without_idr > 0` e não há IDR periódico | Planejar a opção c. |
| Em operação, aparece `CUT_POINT_WITHOUT_IDR` | Mesmo gatilho: medir a fonte e reabrir. |

### D5 — Extrair a máquina de estados antes de P1

**Confirmada pelo Davi: extrair primeiro (E01 a E03 antes de P1).**

| Ordem | Vantagem | Risco |
|---|---|---|
| Extrair primeiro (E01 a E03), depois P1 | P1, 2.2 e 2.6 nascem com teste sem GPU. A correção de 2.1 fica num destrutor de uma classe pequena. Cada refatoração é validada por log idêntico e pelos trechos do baseline. | O defeito mais grave fica três commits mais longe. Uma refatoração pode introduzir regressão. |
| P1 primeiro, dentro de `main.cpp` | Corrige 2.1 já. | Os testes de P1 só existem com GPU e RTSP. O mesmo código é movido de novo em P6, e os testes são reescritos. |

Evidência: a simulação citada em 2.10 mostra que a lógica do `main` é reproduzível sem GPU com resultado idêntico ao baseline. Isso reduz o risco da extração, porque há um oráculo.

Recomendação: **extrair primeiro**. Se o Davi preferir P1 primeiro, as etapas E04 a E06 precisam ser reescritas tendo `src/main.cpp` como escopo; o conteúdo técnico (o que finalizar, quais flags, qual política) não muda.

Critério que confirma: ao fim de E03, R1 a R6 verdes e o log normalizado idêntico ao da base.

## 4. Etapas

Ordem: E01 → E14. Cada etapa é um commit. Uma etapa só começa com a anterior aprovada e com as decisões listadas em "Dependências" confirmadas.

| Etapa | Título | Itens | Tipo |
|---|---|---|---|
| E01 | Extrair `MotionHysteresis` | 2.10 | Refatoração |
| E02 | Extrair `Mp4EventPacketSink` | 2.10 | Refatoração |
| E03 | Extrair `EventRecorder` | 2.10 | Refatoração |
| E04 | Finalizar o MP4 quando há exceção | 2.1 | Comportamento |
| E05 | MP4 fragmentado com descarga por pacote | 2.1 | Comportamento |
| E06 | Política para carimbo inválido | 2.2 | Comportamento |
| E07 | Motivo do fim da leitura e códigos de saída | 2.3 | Comportamento |
| E08 | SPS/PPS para o NVDEC quando não vêm no fluxo | 2.5 | Comportamento |
| E09 | Teto para a região à frente da fronteira | 2.5 | Comportamento |
| E10 | Ferramenta de medição de pontos de corte | 2.4 | Diagnóstico |
| E11 | Registrar quando um MP4 começa em keyframe sem IDR | 2.4 | Observabilidade |
| E12 | Log quando o teto de idle descarta | 2.6 | Comportamento |
| E13 | Rejeitar limiar não finito | 2.8 | Comportamento |
| E14 | Remover API legada e código morto, alinhar documentos | 2.11 | Limpeza |

### 4.0 Preparos comuns

Atalho: source motion_mog2/tools/tcc_checks.sh

Tudo abaixo roda a partir da raiz do repositório, na máquina do Davi.

**Variáveis**

```bash
export OPENCV_DIR=/home/davi/Downloads/tcc/opencv/install-cuda125/lib/cmake/opencv4
export VIDEO="/home/davi/Vídeos/tcc/video_h264_fhd_60fps.mp4"
export BUILD=motion_mog2/build
export BIN=$BUILD/motion_mog2
export MEDIAMTX_BIN=/home/davi/Downloads/tcc/mediamtx
export RTSP_URL=rtsp://127.0.0.1:8554/video
```

**BUILD** (o mesmo em todas as etapas)

```bash
cmake -S motion_mog2 -B motion_mog2/build -DOpenCV_DIR="$OPENCV_DIR" -DBUILD_TESTING=ON && cmake --build motion_mog2/build
```

**Funções de verificação** (validadas nesta análise com FFmpeg 6.1.1; colar no shell antes dos checks)

```bash
tcc_md5() { ffmpeg -v error -i "$1" -map 0:v:0 -c copy -f framemd5 - | grep -v '^#' | awk -F', *' '{print $NF}'; }

# uso: tcc_ranges <video_fonte> <diretorio_dos_mp4>   -> uma linha RANGE por MP4
tcc_ranges() {
  local source_video="$1" output_directory="$2" recording first_hash packet_count first_index last_index
  tcc_md5 "$source_video" > /tmp/tcc_source.md5
  for recording in "$output_directory"/*.mp4; do
    tcc_md5 "$recording" > /tmp/tcc_recording.md5
    packet_count=$(wc -l < /tmp/tcc_recording.md5)
    first_hash=$(head -n 1 /tmp/tcc_recording.md5)
    first_index=$(( $(grep -n -m 1 "$first_hash" /tmp/tcc_source.md5 | cut -d: -f1) - 1 ))
    last_index=$(( first_index + packet_count - 1 ))
    if [ "$(sed -n "$((first_index + 1)),$((last_index + 1))p" /tmp/tcc_source.md5 | md5sum)" = "$(md5sum < /tmp/tcc_recording.md5)" ]; then
      echo "RANGE $(basename "$recording") [$first_index,$last_index] packets=$packet_count contiguous=yes"
    else
      echo "RANGE $(basename "$recording") first=$first_index packets=$packet_count contiguous=NO"
    fi
  done
}

# uso: tcc_check_mp4 <arquivo.mp4>   -> uma linha MP4CHECK
tcc_check_mp4() {
  local first_flags decode_error_lines counts
  first_flags=$(ffprobe -v error -select_streams v:0 -show_entries packet=flags -of csv=p=0 -read_intervals '%+#1' "$1" | head -n 1)
  decode_error_lines=$(ffmpeg -v error -i "$1" -f null - 2>&1 | wc -l)
  counts=$(ffprobe -v error -select_streams v:0 -count_packets -count_frames -show_entries stream=nb_read_packets,nb_read_frames -of default=nw=1 "$1" | tr '\n' ' ')
  echo "MP4CHECK $(basename "$1") first_packet_flags=$first_flags decode_error_lines=$decode_error_lines $counts"
}

# uso: tcc_normalize_log <arquivo.log>   -> log sem os caminhos dos MP4 (o nome do arquivo contém a hora)
tcc_normalize_log() { sed -E 's/path="[^"]*"/path=X/g' "$1"; }
```

Um MP4 está **são** quando a linha `MP4CHECK` mostra `first_packet_flags=K__`, `decode_error_lines=0` e `nb_read_frames` igual a `nb_read_packets`. A terceira condição é necessária: um arquivo iniciado em keyframe não-IDR decodifica sem mensagem de erro e ainda assim perde quadros (medido: 107 de 125).

**Vídeos de teste**

| ID | O que é | Comando | Medido nesta análise |
|---|---|---|---|
| G1 | Vídeo do baseline | `$VIDEO` | Não tenho o arquivo. |
| G2 | H.264 sintético, 1199 pacotes, keyframe a cada 60, ~59,94 fps, sem quadros B | `ffmpeg -v error -y -f lavfi -i testsrc=d=20:r=60000/1001:s=640x360 -c:v libx264 -g 60 -keyint_min 60 -sc_threshold 0 -bf 0 -pix_fmt yuv420p /tmp/tcc_g2.mp4` | 1199 pacotes, 20 com flag `KEY`. |
| G3 | G1 com o `dts` e o `pts` do pacote 200 iguais aos do 199 | `ffmpeg -v error -y -i "$VIDEO" -map 0:v:0 -c copy -bsf:v "setts=dts='if(eq(N,200),PREV_OUTDTS,DTS)':pts='if(eq(N,200),PREV_OUTPTS,PTS)'" /tmp/tcc_g3_dup_dts.mkv` | Com G2 como entrada, o leitor vê exatamente 1 `dts` não crescente. Em MPEG-TS o desmultiplexador recalcula e o defeito some; por isso MKV. |
| G4 | G1 como H.264 cru, todos os pacotes sem carimbo | `ffmpeg -v error -y -i "$VIDEO" -map 0:v:0 -c copy -bsf:v h264_mp4toannexb -f h264 /tmp/tcc_g4_sem_ts.h264` | Com G2 como entrada, 1199 de 1199 pacotes sem `pts` e sem `dts`. |
| G5 | Intra-refresh sintético | `ffmpeg -v error -y -f lavfi -i testsrc=d=6:r=25:s=320x240 -c:v libx264 -bf 0 -pix_fmt yuv420p -x264-params intra-refresh=1:keyint=25 -f mpegts /tmp/tcc_g5_intra_refresh.ts` | 150 quadros, 6 pacotes `KEY`, 1 NAL IDR. |
| G6 | G1 recodificado em intra-refresh (só para teste; I5 permite) | `ffmpeg -v error -y -i "$VIDEO" -an -c:v libx264 -preset veryfast -bf 0 -x264-params intra-refresh=1:keyint=60 -f mpegts /tmp/tcc_g6_intra_refresh.ts` | Não medido. Os quadros de movimento podem mudar em relação ao G1. |
| G7 | H.264 sintético com GOP de 600 pacotes (10 s), 1798 pacotes, ~59,94 fps | `ffmpeg -v error -y -f lavfi -i testsrc=d=30:r=60000/1001:s=640x360 -c:v libx264 -g 600 -keyint_min 600 -sc_threshold 0 -bf 0 -pix_fmt yuv420p /tmp/tcc_g7_gop_longo.mp4` | 1798 pacotes, 3 com flag `KEY`. Usado na medição de `frag_duration` de D1. Não sei se o padrão de teste gera movimento para o MOG2. |

**Arranjo RTSP manual (RIG)**, para cenários que `run_rtsp_recording.sh` não cobre. Três terminais.

```bash
# Terminal 1: servidor. O YAML é o mesmo que run_rtsp_recording.sh gera.
printf '%s\n' 'logLevel: info' 'logDestinations: [stdout]' 'api: false' 'metrics: false' 'pprof: false' 'playback: false' \
  'rtsp: true' 'rtspAddress: 127.0.0.1:8554' 'rtspTransports: [udp, tcp]' 'rtpAddress: 127.0.0.1:8000' 'rtcpAddress: 127.0.0.1:8001' \
  'rtmp: false' 'hls: false' 'webrtc: false' 'srt: false' 'moq: false' 'paths:' '  video:' '    source: publisher' '    overridePublisher: false' \
  > /tmp/tcc_mediamtx.yml
"$MEDIAMTX_BIN" /tmp/tcc_mediamtx.yml

# Terminal 2: publicador. Escolher uma variante.
# PUB-COPY: como o baseline
ffmpeg -hide_banner -loglevel warning -nostdin -re -stream_loop -1 -i "$VIDEO" -map 0:v:0 -an -c:v copy -f rtsp -rtsp_transport tcp "$RTSP_URL"
# PUB-SPS: garante SPS/PPS dentro do fluxo, antes de cada IDR
ffmpeg -hide_banner -loglevel warning -nostdin -re -stream_loop -1 -i "$VIDEO" -map 0:v:0 -an -c:v copy -bsf:v h264_mp4toannexb -f rtsp -rtsp_transport tcp "$RTSP_URL"
# PUB-NOSPS: remove SPS/PPS do fluxo; ficam só no SDP
ffmpeg -hide_banner -loglevel warning -nostdin -re -stream_loop -1 -i "$VIDEO" -map 0:v:0 -an -c:v copy -bsf:v "h264_mp4toannexb,filter_units=remove_types=7|8" -f rtsp -rtsp_transport tcp "$RTSP_URL"
# PUB-WEBCAM-EQ: mesma codificação do modo --webcam do run_rtsp_recording.sh, sem precisar da webcam
ffmpeg -hide_banner -loglevel warning -nostdin -re -f lavfi -i testsrc=r=30:s=640x480 -map 0:v:0 -an -c:v libx264 -preset ultrafast -tune zerolatency -pix_fmt yuv420p -g 60 -keyint_min 60 -sc_threshold 0 -bf 0 -f rtsp -rtsp_transport tcp "$RTSP_URL"

# Terminal 3: gravador
"$BIN" "$RTSP_URL" /tmp/tcc_saida 5 1.0 2 3 5 tcp
```

Nesta análise usei MediaMTX 1.9.3, em que a chave equivalente a `rtspTransports` se chama `protocols` e `moq` não existe; a versão do Davi é mais nova. O comportamento medido pode diferir entre versões, e por isso existe a medição abaixo.

**MED-1 — o fluxo recebido tem SPS dentro dele?**

```bash
timeout 30 ffmpeg -v verbose -nostdin -rtsp_transport tcp -i "$RTSP_URL" -t 4 -map 0:v:0 -c copy -bsf:v trace_headers -f null - 2>&1 | grep -c "Sequence Parameter Set"
```

Resultado 1: só a extradata; não há SPS no fluxo. Resultado maior que 1: há SPS no fluxo. Medido: PUB-COPY com G2 dá 1; PUB-SPS dá 5; PUB-NOSPS dá 1; PUB-WEBCAM-EQ dá 1 (contagem de pacotes com SPS feita por um leitor próprio: 0, 5, 0 e 0).

**Regressão do baseline** (IDs usados em todas as etapas)

| ID | Check | Comando | Esperado |
|---|---|---|---|
| R1 | Build | BUILD | Termina sem erro. |
| R2 | Testes unitários | `ctest --test-dir $BUILD --output-on-failure` e, à parte, `$BUILD/encoded_packet_queue_tests` e `$BUILD/event_recording_config_tests` | Todos passam. `encoded_packet_queue_tests` com pelo menos 12 linhas PASS, `event_recording_config_tests` com pelo menos 10. |
| R3 | Arquivo, pós-evento 5 | `rm -rf /tmp/tcc_r3; "$BIN" "$VIDEO" /tmp/tcc_r3 5 1.0 2 3 5 > /tmp/tcc_r3.log 2>&1; echo exit=$?; tcc_ranges "$VIDEO" /tmp/tcc_r3` | `exit=0`; duas linhas RANGE: `[0,779]` e `[780,1180]`, ambas `contiguous=yes`. |
| R4 | Arquivo, pós-evento 0 | Igual a R3 com `/tmp/tcc_r4` e `5 1.0 2 3 0` | `exit=0`; três linhas RANGE: `[0,239]`, `[240,479]`, `[540,1139]`. |
| R5 | Sanidade dos MP4 | `for f in /tmp/tcc_r3/*.mp4 /tmp/tcc_r4/*.mp4; do tcc_check_mp4 "$f"; done` | Todos sãos. |
| R6 | Movimento dentro dos arquivos | `grep -E '^MOTION_(STARTED|ENDED)' /tmp/tcc_r3.log` | Mesmas linhas da base. Cada intervalo STARTED..ENDED contido em algum RANGE de R3. O último evento vai até o pacote 1180 (fim de arquivo durante o evento). |
| R7 | Ctrl+C em RTSP TCP | `RTSP_TRANSPORT=tcp SHOW_PREVIEW=0 bash run_rtsp_recording.sh "$VIDEO" /tmp/tcc_r7 5`; depois de aparecer `EVENT_STARTED`, Ctrl+C | O gravador encerra em até 5 s; `recording.log` contém `SHUTDOWN_FLUSHED` e `MP4_SAVED`; MP4s sãos. |
| R8 | Ctrl+C com publicador suspenso | Como R7; antes do Ctrl+C, `kill -STOP <pid do ffmpeg publicador>` | Encerra em até 5 s. |

R7 e R8 são manuais. Um agente sem terminal interativo pode usar `timeout -s INT 25 env RTSP_TRANSPORT=tcp SHOW_PREVIEW=0 bash run_rtsp_recording.sh "$VIDEO" /tmp/tcc_r7 5`; não validei essa forma, então, se ela não funcionar, o check vai para UNV com a razão.

Para as refatorações (E01 a E03) há um check adicional, **R9 — log idêntico**: `diff <(tcc_normalize_log /tmp/tcc_pre_r3.log) <(tcc_normalize_log /tmp/tcc_r3.log)` sem saída, onde `/tmp/tcc_pre_r3.log` é o log de R3 capturado no PRE, com o binário da base.

---

### E01 — Extrair `MotionHysteresis`

- **Itens atendidos:** 2.10 (parte da histerese). **Dependências:** D5.

**Objetivo e por que esta solução.** A regra que transforma "este quadro tem movimento" em "o movimento começou / terminou" (aquecimento, N quadros seguidos para iniciar, M para terminar) está no mesmo método que chama o MOG2 em CUDA (`src/detectors/Mog2MotionDetector.cpp:89-130`). Separá-la numa classe sem OpenCV permite testá-la sem GPU. A alternativa de testar o detector inteiro com um vídeo exige GPU e não isola a regra.

**Escopo de arquivos**

- Criar: `motion_mog2/include/detectors/MotionHysteresis.hpp`, `motion_mog2/src/detectors/MotionHysteresis.cpp`, `motion_mog2/tests/motion_hysteresis_tests.cpp`.
- Alterar: `motion_mog2/include/detectors/Mog2MotionDetector.hpp`, `motion_mog2/src/detectors/Mog2MotionDetector.cpp`, `motion_mog2/CMakeLists.txt`.

**Mudança**

`struct MotionState` sai de `Mog2MotionDetector.hpp:9-15` e vai, sem alteração de campos, para `MotionHysteresis.hpp`; `Mog2MotionDetector.hpp` passa a incluir esse cabeçalho.

```cpp
class MotionHysteresis {
public:
    MotionHysteresis(double motion_threshold_ratio, int motion_start_frame_count,
                     int motion_end_frame_count, std::uint64_t warmup_frame_count);
    MotionState update(double foreground_ratio, std::uint64_t decoded_frame_index);
    bool checkIfMotionIsActive() const;
    std::uint64_t getWarmupFrameCount() const;
};
```

`update` reproduz exatamente as linhas 89-130 atuais: durante o aquecimento (`decoded_frame_index < warmup_frame_count`) zera os dois contadores e devolve um estado com `detected`, `started`, `ended` falsos, `foreground_ratio` 0 e `active` com o valor corrente; fora dele, calcula `detected = foreground_ratio >= motion_threshold_ratio` e aplica os contadores. `Mog2MotionDetector` mantém MOG2, limiar, morfologia e `countNonZero` (linhas 62-87), calcula a razão e delega. Os membros `motion_active_`, `consecutive_motion_frame_count_`, `consecutive_idle_frame_count_`, `motion_threshold_ratio_`, `motion_start_frame_count_`, `motion_end_frame_count_` e `warmup_frame_count_` passam para a nova classe. A validação do limiar no construtor do detector (linhas 30-35) fica onde está. Nenhum log novo.

No CMake: acrescentar `src/detectors/MotionHysteresis.cpp` ao executável e criar o alvo `motion_hysteresis_tests` (só esse `.cpp` e o teste, sem OpenCV), com `add_test`.

**Invariantes e riscos.** I7. Risco: mudar a ordem de zerar contadores ou o tratamento do aquecimento altera os quadros de início e fim; R6 e R9 detectam.

**Casos de teste**

| ID | Tipo | Objetivo | Preparo | Passos | Resultado esperado | Critério |
|---|---|---|---|---|---|---|
| E01-T01 | Unitário sem GPU | Aquecimento não inicia movimento | Histerese (0,01; 2; 3; aquecimento 60) | `update(1.0, i)` para i de 0 a 59 | Nunca `started`; `detected` falso; razão devolvida 0 | Asserções passam |
| E01-T02 | Unitário sem GPU | Início exige N seguidos | Idem | Quadros 60 e 61 com razão 0,5 | `started` só no 61; `active` verdadeiro | Idem |
| E01-T03 | Unitário sem GPU | Sequência interrompida zera a contagem | Idem | 60 com movimento, 61 sem, 62 com | Sem `started` até 63 com movimento | Idem |
| E01-T04 | Unitário sem GPU | Fim exige M seguidos | Movimento ativo | 3 quadros sem movimento | `ended` só no terceiro | Idem |
| E01-T05 | Unitário sem GPU | Quadro com movimento no meio zera a contagem de fim | Movimento ativo | sem, sem, com, sem, sem, sem | `ended` só no último | Idem |
| E01-T06 | Unitário sem GPU | Limiar é inclusivo | Limiar 0,01 | Razão exatamente 0,01 | `detected` verdadeiro | Idem |
| E01-T07 | Unitário sem GPU | `started` e `ended` nunca juntos | Sequência pseudoaleatória fixa de 10 000 quadros | Percorrer | Em nenhum quadro os dois são verdadeiros | Idem |
| E01-T08 | Unitário sem GPU | N = 1 e M = 1 | Histerese (0,01; 1; 1; 0) | com, sem | `started` no primeiro, `ended` no segundo | Idem |
| E01-T09 | Integração com GPU | Detector inalterado | G1 | R3 | Linhas `MOTION_*` iguais às da base | R6 e R9 |

**Regressão obrigatória:** R1 a R6, R9.

**Critério de pronto:** todos os checks PASS; `Mog2MotionDetector.cpp` sem lógica de contadores. **Reverter:** `git revert <commit>`.

```text
TASK
ID: E01
BASE: feat/fila-unica@bc806b8
SPEC: motion_mog2/specs/plano_mudancas_v2.md, secao 4, etapa E01
SCOPE: motion_mog2/include/detectors/MotionHysteresis.hpp (criar); motion_mog2/src/detectors/MotionHysteresis.cpp (criar); motion_mog2/tests/motion_hysteresis_tests.cpp (criar); motion_mog2/include/detectors/Mog2MotionDetector.hpp; motion_mog2/src/detectors/Mog2MotionDetector.cpp; motion_mog2/CMakeLists.txt
BUILD: cmake -S motion_mog2 -B motion_mog2/build -DOpenCV_DIR=/home/davi/Downloads/tcc/opencv/install-cuda125/lib/cmake/opencv4 -DBUILD_TESTING=ON && cmake --build motion_mog2/build
PRE: git status limpo; compilar a base; rodar R3 com o binario da base e salvar o log em /tmp/tcc_pre_r3.log
CHECK C1: BUILD termina sem erro
CHECK C2: motion_mog2/build/motion_hysteresis_tests termina com codigo 0 e imprime PASS para E01-T01 a E01-T08
CHECK C3: R2 (ctest verde; encoded_packet_queue_tests >= 12 PASS; event_recording_config_tests >= 10 PASS)
CHECK C4: R3 -> exit=0 e RANGE [0,779], [780,1180] contiguous=yes
CHECK C5: R4 -> exit=0 e RANGE [0,239], [240,479], [540,1139]
CHECK C6: R5 -> todos os MP4 saos
CHECK C7: R9 -> diff do log normalizado contra /tmp/tcc_pre_r3.log vazio
OUT O1: saida completa de motion_hysteresis_tests
OUT O2: linhas RANGE de C4 e C5
OUT O3: linhas MOTION_STARTED e MOTION_ENDED de /tmp/tcc_r3.log
COMMIT: refactor: extrai histerese de movimento para MotionHysteresis
PUSH: origin feat/fila-unica
```

---

### E02 — Extrair `Mp4EventPacketSink`

- **Itens atendidos:** 2.10 (parte da escrita). **Dependências:** E01, D5.

**Objetivo e por que esta solução.** Abrir, escrever e finalizar o MP4 hoje são três lambdas dentro de `main` que capturam tudo por referência (`src/main.cpp:187-219`). A máquina de estados precisa de um destino de pacotes que possa ser trocado por um falso nos testes. Uma interface pequena com uma implementação MP4 resolve isso e dá à correção de 2.1 (E04) um destrutor onde morar. A alternativa de passar três `std::function` mantém o estado do arquivo espalhado no `main`.

**Escopo de arquivos**

- Criar: `motion_mog2/include/recording/EventPacketSink.hpp`, `motion_mog2/include/recording/Mp4EventPacketSink.hpp`, `motion_mog2/src/recording/Mp4EventPacketSink.cpp`, `motion_mog2/tests/mp4_event_packet_sink_tests.cpp`.
- Alterar: `motion_mog2/src/main.cpp`, `motion_mog2/CMakeLists.txt`.

**Mudança**

```cpp
class EventPacketSink {
public:
    virtual ~EventPacketSink() = default;
    virtual void openRecording() = 0;
    virtual void writePackets(std::vector<AvPacketPtr>&& encoded_packets) = 0;
    virtual void finalizeRecording() = 0;
    virtual bool checkIfRecordingIsOpen() const = 0;
    virtual std::size_t getWrittenPacketCount() const = 0;
    virtual std::string getRecordingDescription() const = 0;   // caminho, para log
};

class Mp4EventPacketSink final : public EventPacketSink {
public:
    Mp4EventPacketSink(std::filesystem::path output_directory,
                       std::shared_ptr<const VideoStreamParameters> video_stream_parameters);
    // sobrescreve os seis metodos; copia e movimento apagados
};
```

- `openRecording()` contém o corpo de `recording_opener` (linhas 188-195) e a função `buildRecordingFilePath` (linhas 39-66), que sai do `main`. Lança `std::logic_error` se já houver gravação aberta.
- `writePackets()` contém o corpo de `encoded_packet_writer` (linhas 199-209), inclusive a verificação de que o primeiro lote começa em keyframe. Lote vazio continua sendo aceito sem efeito. Lança `std::logic_error` se não houver gravação aberta.
- `finalizeRecording()` contém `writeMp4Trailer` e a liberação dos dois contextos (linhas 212-214). Lança `std::logic_error` se não houver gravação aberta.
- Membros privados, **nesta ordem**: `output_io_context_` e depois `mp4_output_context_`, para manter a ordem de destruição de hoje (`src/main.cpp:178-179`).
- O destrutor é o padrão. Destruir com gravação aberta continua deixando o arquivo sem trailer; isso muda em E04, não aqui.
- No `main`, as três lambdas passam a chamar o sink e continuam imprimindo `MP4_STARTED` e `MP4_SAVED` com o mesmo texto. `recording_read_count`, `recording_encoded_packet_count` e `completed_recording_count` continuam no `main` nesta etapa; `recording_encoded_packet_count` passa a vir de `getWrittenPacketCount()`.

No CMake: acrescentar o `.cpp` ao executável; criar `mp4_event_packet_sink_tests` com as fontes de `ffmpeg/` necessárias, sem OpenCV, com `add_test`.

**Invariantes e riscos.** I4, I5, I7. Riscos: trocar a ordem dos membros; mudar o texto ou a ordem dos logs; perder a checagem de keyframe do primeiro lote. O log atual imprime um `std::filesystem::path`, que sai entre aspas (`path="..."`); esse formato deve ser mantido, e R9 depende dele.

**Casos de teste** (pacotes sintéticos, como em `tests/av_output_format_context_tests.cpp`)

| ID | Tipo | Objetivo | Preparo | Passos | Resultado esperado | Critério |
|---|---|---|---|---|---|---|
| E02-T01 | Unitário sem GPU | Ciclo completo | Diretório temporário | abrir, escrever 4 pacotes (o primeiro keyframe), finalizar | Arquivo reabre com `avformat_open_input`; 4 pacotes, bytes e ordem iguais | Asserções passam |
| E02-T02 | Unitário sem GPU | Primeiro lote sem keyframe | Idem | abrir, escrever lote iniciado por não-keyframe | `std::logic_error` | Idem |
| E02-T03 | Unitário sem GPU | Escrever sem abrir | Idem | `writePackets` direto | `std::logic_error`; nenhum arquivo criado | Idem |
| E02-T04 | Unitário sem GPU | Finalizar sem abrir | Idem | `finalizeRecording` direto | `std::logic_error` | Idem |
| E02-T05 | Unitário sem GPU | Abrir duas vezes | Idem | abrir, abrir | `std::logic_error` na segunda | Idem |
| E02-T06 | Unitário sem GPU | Duas gravações no mesmo segundo | Idem | ciclo completo duas vezes seguidas | Dois arquivos distintos; o segundo com sufixo `_001` se o nome colidir | Idem |
| E02-T07 | Unitário sem GPU | Lote vazio | Gravação aberta | `writePackets({})` | Sem efeito; contagem 0 | Idem |
| E02-T08 | Unitário sem GPU | Contagem | Gravação aberta | lotes de 2 e 3 | `getWrittenPacketCount() == 5` | Idem |
| E02-T09 | Integração com GPU | Gravação inalterada | G1 | R3, R4 | Trechos e log iguais | R3, R4, R9 |

**Regressão obrigatória:** R1 a R6, R9.

**Critério de pronto:** checks PASS; `main.cpp` sem `buildRecordingFilePath` e sem chamadas diretas a `allocateMp4OutputContext`, `openOutputFile`, `writeMp4*`. **Reverter:** `git revert <commit>`.

```text
TASK
ID: E02
BASE: feat/fila-unica@<commit da etapa anterior>
SPEC: motion_mog2/specs/plano_mudancas_v2.md, secao 4, etapa E02
SCOPE: motion_mog2/include/recording/EventPacketSink.hpp (criar); motion_mog2/include/recording/Mp4EventPacketSink.hpp (criar); motion_mog2/src/recording/Mp4EventPacketSink.cpp (criar); motion_mog2/tests/mp4_event_packet_sink_tests.cpp (criar); motion_mog2/src/main.cpp; motion_mog2/CMakeLists.txt
BUILD: cmake -S motion_mog2 -B motion_mog2/build -DOpenCV_DIR=/home/davi/Downloads/tcc/opencv/install-cuda125/lib/cmake/opencv4 -DBUILD_TESTING=ON && cmake --build motion_mog2/build
PRE: git status limpo; rodar R3 com o binario da base desta etapa e salvar o log em /tmp/tcc_pre_r3.log
CHECK C1: BUILD termina sem erro
CHECK C2: motion_mog2/build/mp4_event_packet_sink_tests termina com codigo 0 e imprime PASS para E02-T01 a E02-T08
CHECK C3: R2
CHECK C4: R3 -> exit=0 e RANGE [0,779], [780,1180] contiguous=yes
CHECK C5: R4 -> exit=0 e RANGE [0,239], [240,479], [540,1139]
CHECK C6: R5 -> todos os MP4 saos
CHECK C7: R9 -> diff do log normalizado vazio
OUT O1: saida completa de mp4_event_packet_sink_tests
OUT O2: linhas RANGE de C4 e C5
COMMIT: refactor: extrai abertura, escrita e finalizacao do MP4 para Mp4EventPacketSink
PUSH: origin feat/fila-unica
```

---

### E03 — Extrair `EventRecorder`

- **Itens atendidos:** 2.10 (máquina de estados). **Dependências:** E02, D5.

**Objetivo e por que esta solução.** As transições de `RecordingState` e o encerramento estão em `main` (`src/main.cpp:240-325` e `:332-362`). Movê-las para uma classe que recebe a fila e um `EventPacketSink` permite exercitar cada transição com a fila real e um sink falso. A fila continua sendo a única (I1): a classe guarda apenas uma referência.

**Escopo de arquivos**

- Criar: `motion_mog2/include/recording/EventRecorder.hpp`, `motion_mog2/src/recording/EventRecorder.cpp`, `motion_mog2/tests/event_recorder_tests.cpp`.
- Alterar: `motion_mog2/src/main.cpp`, `motion_mog2/CMakeLists.txt`.

**Mudança**

```cpp
enum class RecordingState { idle, waiting_keyframe, recording, post_event, closing_at_keyframe };

struct EventRecorderConfig {
    std::size_t lookback_packet_count = 0;
    std::size_t maximum_idle_packet_count = 0;
    std::size_t post_event_packet_count = 0;
};

class EventRecorder {
public:
    EventRecorder(EncodedPacketQueue& encoded_packet_queue, EventPacketSink& event_packet_sink,
                  EventRecorderConfig recorder_config, std::ostream& event_log);
    void processDecodedFrame(const MotionState& motion_state, std::uint64_t decoded_frame_index);
    void finishCapture(std::vector<AvPacketPtr> remaining_encoded_packets);
    RecordingState getRecordingState() const;
    std::size_t getCompletedRecordingCount() const;
};
```

- O construtor lança `std::invalid_argument` se `maximum_idle_packet_count` for 0 ou menor que `lookback_packet_count` (mesma regra de `trimCountedPackets`).
- `processDecodedFrame` contém, na mesma ordem, o incremento de `recording_read_count` quando há gravação aberta (linhas 240-242), o `switch` (246-316) e a transição de fim de evento (319-325).
- `finishCapture` contém o `switch` de encerramento (332-362). O `main` continua chamando `video_reader.stopDecoding()` e `takeAllPackets()` (328-329) e passa o resultado.
- Os logs `EVENT_STARTED`, `EVENT_WAITING_KEYFRAME`, `PRE_EVENT_WRITTEN`, `EVENT_DISCARDED_NO_KEYFRAME`, `EVENT_RESUMED`, `POST_EVENT_EXPIRED`, `EVENT_ENDED`, `MP4_STARTED`, `MP4_SAVED`, `SHUTDOWN_FLUSHED`, `SHUTDOWN_DISCARDED_IDLE_PACKETS` passam a ser escritos em `event_log`, com o mesmo texto, a mesma ordem e `std::endl` onde hoje há `std::endl`. O `main` passa `std::cout`.
- Ficam no `main`: leitura, detecção, logs `MOTION_STARTED` e `MOTION_ENDED` (233-238), cálculo das contagens (117-157) e `CAPTURE_COMPLETE` (363-364, usando `getCompletedRecordingCount()`).
- A escrita continua fora do lock: os métodos `take*` devolvem o lote por valor antes de o sink ser chamado.

No CMake: acrescentar o `.cpp` ao executável; criar `event_recorder_tests` com `EventRecorder.cpp`, `EncodedPacketQueue.cpp`, `AvPacket.cpp`, sem OpenCV, com `add_test`.

**Invariantes e riscos.** I1, I3, I4, I7. Riscos: ordem dos logs; esquecer a transição das linhas 319-325 quando o arquivo é aberto e o movimento termina na mesma leitura; alterar a contagem de `decoded_reads`.

**Casos de teste.** O sink falso guarda, por gravação, a lista dos identificadores dos pacotes (o `pts` sintético igual ao índice). O cenário "baseline sintético" é: 1181 pacotes, keyframe a cada 60, um pacote inserido e contabilizado por leitura, `started` nos quadros 118, 404, 875, 929, 1014 e `ended` nos quadros 205, 464, 921, 972, 1100, `lookback` 333, teto 1532. Os trechos esperados abaixo foram obtidos nesta análise com esse mesmo método sobre a fila real.

| ID | Tipo | Objetivo | Preparo | Passos | Resultado esperado | Critério |
|---|---|---|---|---|---|---|
| E03-T01 | Unitário sem GPU | Baseline, pós-evento 5 s | Baseline sintético, pós-evento 300 pacotes | Processar os 1181 quadros e `finishCapture` | 2 gravações: [0,779] e [780,1180] | Listas de identificadores exatas |
| E03-T02 | Unitário sem GPU | Baseline, pós-evento 0 | Idem, pós-evento 0 | Idem | 3 gravações: [0,239], [240,479], [540,1139] | Idem |
| E03-T03 | Unitário sem GPU | Início sem keyframe na fila | Fila só com não-keyframes; `started` | Processar; depois inserir keyframe | `EVENT_WAITING_KEYFRAME`; gravação abre no keyframe | Estado e primeiro identificador |
| E03-T04 | Unitário sem GPU | Movimento termina antes do keyframe | Como T03, `ended` antes do keyframe | Processar | `EVENT_DISCARDED_NO_KEYFRAME`; nenhuma gravação; estado `idle` | Idem |
| E03-T05 | Unitário sem GPU | Retomada em `post_event` | Evento encerrado, pós-evento em curso | `started` | `EVENT_RESUMED`; mesma gravação continua | Uma gravação só |
| E03-T06 | Unitário sem GPU | Retomada em `closing_at_keyframe` | Pós-evento expirado, sem keyframe ainda | `started` | Idem | Idem |
| E03-T07 | Unitário sem GPU | Fechamento deixa o keyframe na fila | `closing_at_keyframe`; chega keyframe | Processar | Gravação termina no pacote anterior; keyframe na frente da fila | Último identificador e conteúdo da fila |
| E03-T08 | Unitário sem GPU | `finishCapture` em cada estado | Cinco cenários, com pacotes à frente da fronteira | Chamar `finishCapture` | `recording`, `post_event`, `closing_at_keyframe`: restantes escritos e gravação finalizada; `waiting_keyframe` com keyframe nos restantes: grava a partir dele; sem keyframe: descarta; `idle`: descarta | Por cenário |
| E03-T09 | Unitário sem GPU | Avanço irregular da fronteira | Baseline sintético com contabilização em rajadas (4, 0, 0, 3, ...) e fronteira até 32 pacotes à frente | Processar | Toda gravação começa em keyframe; gravações contíguas e sem sobreposição; todo pacote de índice entre um `started` e o `ended` correspondente está em alguma gravação | Invariantes; **não** comparar com os trechos de T01 e T02 |
| E03-T10 | Unitário sem GPU | Texto e ordem dos logs | Cenário curto: um evento com pré e pós | Capturar `event_log` | Sequência exata de linhas esperada | Comparação de texto |
| E03-T11 | Unitário sem GPU | Configuração inválida | Teto 0; teto menor que lookback | Construir | `std::invalid_argument` | Asserções |
| E03-T12 | Integração com GPU | Aplicação inalterada | G1 | R3, R4 | Trechos e log iguais | R3, R4, R9 |

Sobre T09: medi que, com a fronteira adiantada, os limites dos arquivos mudam (com 8 pacotes de avanço e pós-evento 0 a simulação gera 4 arquivos em vez de 3), porque o fechamento acontece assim que um keyframe entra na região contabilizada. Os trechos do baseline valem para o avanço real do NVDEC naquele vídeo; por isso T09 verifica invariantes.

**Regressão obrigatória:** R1 a R6, R9; R7 e R8 (o caminho de encerramento foi movido).

**Critério de pronto:** checks PASS; `main.cpp` sem `switch` sobre `RecordingState`. **Reverter:** `git revert <commit>`.

```text
TASK
ID: E03
BASE: feat/fila-unica@<commit da etapa anterior>
SPEC: motion_mog2/specs/plano_mudancas_v2.md, secao 4, etapa E03
SCOPE: motion_mog2/include/recording/EventRecorder.hpp (criar); motion_mog2/src/recording/EventRecorder.cpp (criar); motion_mog2/tests/event_recorder_tests.cpp (criar); motion_mog2/src/main.cpp; motion_mog2/CMakeLists.txt
BUILD: cmake -S motion_mog2 -B motion_mog2/build -DOpenCV_DIR=/home/davi/Downloads/tcc/opencv/install-cuda125/lib/cmake/opencv4 -DBUILD_TESTING=ON && cmake --build motion_mog2/build
PRE: git status limpo; rodar R3 e R4 com o binario da base desta etapa e salvar os logs em /tmp/tcc_pre_r3.log e /tmp/tcc_pre_r4.log
CHECK C1: BUILD termina sem erro
CHECK C2: motion_mog2/build/event_recorder_tests termina com codigo 0 e imprime PASS para E03-T01 a E03-T11
CHECK C3: R2
CHECK C4: R3 -> exit=0 e RANGE [0,779], [780,1180] contiguous=yes
CHECK C5: R4 -> exit=0 e RANGE [0,239], [240,479], [540,1139]
CHECK C6: R5 -> todos os MP4 saos
CHECK C7: R9 para R3 e para R4 -> diffs vazios
CHECK C8: R7 -> encerra em ate 5 s com SHUTDOWN_FLUSHED
CHECK C9: R8 -> encerra em ate 5 s
OUT O1: saida completa de event_recorder_tests
OUT O2: linhas RANGE de C4 e C5
OUT O3: ultimas 10 linhas do recording.log de C8
COMMIT: refactor: extrai maquina de estados de gravacao para EventRecorder
PUSH: origin feat/fila-unica
```

---

### E04 — Finalizar o MP4 quando há exceção

- **Itens atendidos:** 2.1. **Dependências:** E03. Não depende de D1 (é a parte A).

**Objetivo e por que esta solução.** Hoje qualquer exceção durante um evento destrói os contextos sem escrever o trailer (`src/ffmpeg/AvMp4OutputContext.cpp:16-22`, `src/main.cpp:365-371`), e o arquivo não abre. A correção é o destrutor de `Mp4EventPacketSink` finalizar a gravação aberta. Um `catch` no `main` chamando um método de aborto faria o mesmo, mas dependeria de cada caminho de saída lembrar de chamá-lo; o destrutor cobre todos, e o `AGENTS.md` pede RAII. Colocar o trailer no deleter de `AvMp4OutputContextPtr` foi descartado: o deleter é usado também por contextos que nunca escreveram cabeçalho.

**Escopo de arquivos**

- Alterar: `motion_mog2/include/recording/Mp4EventPacketSink.hpp`, `motion_mog2/src/recording/Mp4EventPacketSink.cpp`, `motion_mog2/tests/mp4_event_packet_sink_tests.cpp`.

**Mudança**

`~Mp4EventPacketSink() noexcept` passa a ser declarado. Se houver gravação aberta com cabeçalho já escrito:

1. Com pelo menos um pacote escrito: chama `writeMp4Trailer` dentro de `try`. Em caso de sucesso imprime em `std::cout` a linha `MP4_SAVED_AFTER_ERROR path=<caminho> encoded_packets=<n>`. Em caso de exceção imprime em `std::cerr` a linha `[erro] MP4_FINALIZE_FAILED path=<caminho> reason=<what()>`. Nenhuma exceção sai do destrutor.
2. Com zero pacotes escritos: libera os contextos, remove o arquivo (foi criado por este objeto e não tem conteúdo útil) e imprime `MP4_DISCARDED_EMPTY path=<caminho>`.

O sink precisa de um indicador `header_written_` para distinguir "arquivo criado" de "cabeçalho escrito". `finalizeRecording()` não muda, e depois dela o destrutor não faz nada. O código de saída continua 2 e a mensagem `[fatal]` continua sendo impressa pelo `main`.

Limites: se a exceção veio de falha de I/O, `writeMp4Trailer` recusa o contexto (`src/ffmpeg/AvMp4OutputContext.cpp:259-262`) e o resultado é `MP4_FINALIZE_FAILED`. `kill -9` e falha de segmentação não passam por destrutor; isso é E05.

**Invariantes e riscos.** I4, I5. Riscos: escrever trailer duas vezes; exceção escapando de destrutor durante o desenrolar da pilha, o que chamaria `std::terminate`.

**Casos de teste**

| ID | Tipo | Objetivo | Preparo | Passos | Resultado esperado | Critério |
|---|---|---|---|---|---|---|
| E04-T01 | Unitário sem GPU | Destruição com gravação aberta | Sink num bloco; 30 pacotes | Sair do bloco sem finalizar | Arquivo reabre; 30 pacotes; saída contém `MP4_SAVED_AFTER_ERROR` | Asserções |
| E04-T02 | Unitário sem GPU | Sem trailer duplicado | Sink; ciclo completo | Destruir | Arquivo igual ao de antes da destruição; nenhuma linha `MP4_SAVED_AFTER_ERROR` | Tamanho e conteúdo |
| E04-T03 | Unitário sem GPU | Exceção no segundo lote | 30 pacotes válidos; segundo lote com um `dts` ausente | `writePackets` lança; sair do bloco | Exceção propagada; arquivo reabre com 30 pacotes | Asserções |
| E04-T04 | Unitário sem GPU | Gravação aberta sem pacotes | Sink; só `openRecording` | Destruir | Arquivo removido; `MP4_DISCARDED_EMPTY` | Arquivo não existe |
| E04-T05 | Unitário sem GPU | Destrutor não lança | Gravação aberta; diretório de saída removido antes da destruição | Destruir | Processo continua; no máximo `MP4_FINALIZE_FAILED` | Teste termina normalmente |
| E04-T06 | Integração com GPU | Reproduzir o defeito e a correção | G3 | `"$BIN" /tmp/tcc_g3_dup_dts.mkv /tmp/tcc_e04 5 1.0 2 3 5; echo exit=$?`; `tcc_check_mp4` no arquivo gerado | No PRE (base): `exit=2`, mensagem `erro -22`, arquivo não abre. Depois: `exit=2`, `MP4_SAVED_AFTER_ERROR`, arquivo são com pacotes [0,199] | `MP4CHECK` são; `nb_read_packets=200` |

Sobre T06: o valor 200 supõe que o evento do baseline (quadros 118 a 205) está em gravação quando o pacote 200 chega e que G3 produz os mesmos quadros de movimento que G1 [Provável]. Se o PRE não reproduzir o erro, a etapa para com BLOCKED.

**Regressão obrigatória:** R1 a R8.

**Critério de pronto:** checks PASS. **Reverter:** `git revert <commit>`.

```text
TASK
ID: E04
BASE: feat/fila-unica@<commit da etapa anterior>
SPEC: motion_mog2/specs/plano_mudancas_v2.md, secao 4, etapa E04
SCOPE: motion_mog2/include/recording/Mp4EventPacketSink.hpp; motion_mog2/src/recording/Mp4EventPacketSink.cpp; motion_mog2/tests/mp4_event_packet_sink_tests.cpp
BUILD: cmake -S motion_mog2 -B motion_mog2/build -DOpenCV_DIR=/home/davi/Downloads/tcc/opencv/install-cuda125/lib/cmake/opencv4 -DBUILD_TESTING=ON && cmake --build motion_mog2/build
PRE: git status limpo; gerar G3; com o binario da base, rodar E04-T06 e confirmar exit=2, "erro -22" e MP4 que nao abre (moov atom not found). Se nao reproduzir => BLOCKED
CHECK C1: BUILD termina sem erro
CHECK C2: mp4_event_packet_sink_tests termina com codigo 0 e imprime PASS para E02-T01 a E02-T08 e E04-T01 a E04-T05
CHECK C3: E04-T06 -> exit=2, log com MP4_SAVED_AFTER_ERROR, MP4CHECK sao com nb_read_packets=200
CHECK C4: R2
CHECK C5: R3 e R4 com os trechos do baseline
CHECK C6: R5
CHECK C7: R7 e R8
OUT O1: saida completa de mp4_event_packet_sink_tests
OUT O2: saida do PRE (log do gravador e erro do ffprobe)
OUT O3: log do gravador e linha MP4CHECK de C3
COMMIT: fix: finaliza o MP4 aberto quando a gravacao termina por excecao
PUSH: origin feat/fila-unica
```

---

### E05 — MP4 fragmentado com descarga por pacote

- **Itens atendidos:** 2.1 (morte abrupta do processo). **Dependências:** E04; D1 (confirmada; o critério de players é verificado nesta etapa, antes do commit).

**Objetivo e por que esta solução.** Depois de E04, um `kill -9` ainda deixa o arquivo ilegível. O MP4 fragmentado grava os metadados junto de cada trecho, e a descarga por pacote garante que cada fragmento completo chegue ao sistema operacional. As duas partes são necessárias; a evidência está em D1.

`frag_duration` de 1 s entra junto de `frag_keyframe`. Só com `frag_keyframe`, o fragmento em curso fica na memória do muxer até o próximo keyframe: com GOP de 10 s medi 5 s perdidos num `kill -9` no meio do GOP, e **nenhum quadro** recuperado quando o processo morre dentro do primeiro GOP. Com `frag_duration` de 1 s a perda ficou em 1 s nos dois casos. O custo medido foi +0,84 % no tamanho, contra +0,08 %. A duração é um parâmetro da função de configuração (0 desliga), para que o teste compare as duas formas e para que a retirada, se algum player exigir, seja a troca de uma constante.

**Escopo de arquivos**

- Alterar: `motion_mog2/include/ffmpeg/AvMp4OutputContext.hpp`, `motion_mog2/src/ffmpeg/AvMp4OutputContext.cpp`, `motion_mog2/src/recording/Mp4EventPacketSink.cpp`, `motion_mog2/tests/mp4_event_packet_sink_tests.cpp`, `motion_mog2/tests/av_output_format_context_tests.cpp`.

**Mudança**

Nova função na camada FFmpeg:

```cpp
void configureMp4ForInterruptedWriting(AVFormatContext& mp4_output_context,
                                       std::int64_t fragment_duration_microseconds);
```

Ela valida que o contexto é uma saída `mp4` ainda sem cabeçalho (`pb` nulo) e que a duração não é negativa; define a opção privada `movflags` com `frag_keyframe+empty_moov+default_base_moof` (por `av_opt_set` em `priv_data`; exige `libavutil/opt.h`); se a duração for maior que zero, define a opção privada `frag_duration` com esse valor (`av_opt_set_int`); e define `flush_packets = 1`. Lança `std::invalid_argument` para contexto ou duração inválidos e `std::runtime_error` se uma opção for recusada.

`Mp4EventPacketSink::openRecording()` chama essa função logo depois de definir `avoid_negative_ts`, passando a constante nomeada `mp4_fragment_duration_microseconds = 1'000'000`. Comentário no código junto da constante: o fragmento fecha quando chega um pacote a pelo menos 1 s do primeiro do fragmento ou quando chega um keyframe (`ff: libavformat/movenc.c:6492-6500`); o primeiro fragmento começa no primeiro pacote do arquivo, que é keyframe; os demais podem começar em quadro comum; o motivo é limitar a 1 s a perda em interrupção, qualquer que seja o GOP. Citar também `ff: doc/muxers.texi` (seção mov, mp4, ismv) e `ff: libavformat/file.c:310-313` como motivo da descarga.

`writeMp4Trailer` continua sendo chamado: ele fecha o último fragmento. O destrutor de E04 não muda. Nenhum log novo.

**Invariantes e riscos.** I5 (continua remux; `framemd5` idêntico, medido). Riscos: player que não abre MP4 fragmentado ou que estranha fragmento iniciado fora de keyframe; ferramentas que leem `nb_frames` do cabeçalho passam a ver N/A; uma chamada de escrita por pacote em vez de por bloco de 256 KiB (impacto não medido com vídeo FHD).

**Casos de teste**

| ID | Tipo | Objetivo | Preparo | Passos | Resultado esperado | Critério |
|---|---|---|---|---|---|---|
| E05-T01 | Unitário sem GPU | Opções aplicadas | Contexto novo | Chamar com 1 000 000; ler `movflags` e `frag_duration` com `av_opt_get` | As três flags presentes; `frag_duration` 1000000; `flush_packets == 1`. Com duração 0, `frag_duration` fica 0 | Asserções |
| E05-T02 | Unitário sem GPU | Entrada inválida | Contexto com cabeçalho escrito; duração negativa | Chamar | `std::invalid_argument` nos dois | Asserção |
| E05-T03 | Unitário sem GPU | Arquivo completo | Sink; 120 pacotes sintéticos, keyframe a cada 60 | Ciclo completo | Reabre; 120 pacotes; bytes iguais; primeiro é keyframe | Asserções |
| E05-T04 | Unitário sem GPU | **Medição comparativa da perda** | Pacotes sintéticos a 60000/1001 fps com **keyframe a cada 600**. Processo filho (`fork`) grava N pacotes e chama `raise(SIGKILL)`. Quatro execuções: N = 900 e N = 300, cada uma com duração 0 e com 1 000 000 | Pai espera e reabre cada arquivo; imprime `KILL_RECOVERY packets_written=<N> fragment_duration_us=<d> packets_recovered=<r>` | Com 1 s: 840 e 240. Com 0: 600 e 0 (arquivo sem quadros). Primeiro pacote keyframe sempre que houver pacotes | Com 1 s: `r >= N - 61`. Com 0: `r` igual ao último múltiplo de 600 abaixo de N. Valores medidos nesta análise com vídeo real de GOP 600 |
| E05-T05 | Manual com RTSP | `kill -9` durante evento, de ponta a ponta | RIG com PUB-COPY e G1 | Iniciar o gravador; após `MP4_STARTED`, esperar 3 s; `kill -9 <pid do gravador>`; `tcc_check_mp4` no arquivo | Arquivo são; duração de pelo menos 1 s. Guardar este arquivo como `/tmp/tcc_e05_kill.mp4` para T10, T12, T14 | `MP4CHECK` são |
| E05-T06 | Manual com RTSP (opcional) | O mesmo com GOP longo | RIG com PUB-COPY e G7 | Idem, esperando 5 s | Arquivo são com duração de pelo menos 3 s | `MP4CHECK` são. Se não houver `MOTION_STARTED` em 20 s (o padrão de teste pode não gerar movimento), SKIP |
| E05-T07 | Integração com GPU | Trechos inalterados e início em keyframe | G1 | R3, R4, R5 | Mesmos trechos; todos os MP4 sãos | R3, R4, R5 |
| E05-T08 | Integração com GPU | Duração reportada | Saída de R3 | `ffprobe -v error -show_entries format=duration -of csv=p=0` em cada MP4 | 780 pacotes ≈ 13,0 s; 401 ≈ 6,7 s | Diferença menor que 0,1 s |
| E05-T09 | Manual, player | `ffplay`, arquivo do baseline | Primeiro MP4 de R3 | `ffplay -v error -autoexit <arquivo>` | Toca do início ao fim; nenhuma linha de erro; setas de busca funcionam | Código 0, saída de erro vazia, confirmação visual |
| E05-T10 | Manual, player | `ffplay`, arquivo de `kill -9` | `/tmp/tcc_e05_kill.mp4` | Idem | Idem | Idem |
| E05-T11 | Manual, player | GNOME Vídeos, arquivo do baseline | Primeiro MP4 de R3 | `totem <arquivo>` | Toca do início ao fim; duração exibida correta; arrastar a barra funciona | Confirmação visual do Davi |
| E05-T12 | Manual, player | GNOME Vídeos, arquivo de `kill -9` | `/tmp/tcc_e05_kill.mp4` | Idem | Idem | Idem |
| E05-T13 | Manual, player | Chrome ou Edge, arquivo do baseline | Primeiro MP4 de R3 | Abrir `file:///...` no navegador | Toca do início ao fim; barra de busca funciona | Confirmação visual do Davi; anotar navegador e versão |
| E05-T14 | Manual, player | Chrome ou Edge, arquivo de `kill -9` | `/tmp/tcc_e05_kill.mp4` | Idem | Idem | Idem |
| E05-T15 | Integração sem GPU | GStreamer sem tela, como apoio a T11 e T12 | Os dois arquivos; `gst-launch-1.0` instalado | `gst-launch-1.0 playbin uri=file://<arquivo> video-sink="fakesink sync=false" audio-sink=fakesink 2>&1 \| grep -ciE '^ERROR\|WARNING'` | 0 | Valor 0. Sem `gst-launch-1.0`, SKIP. Não substitui T11 e T12 |

Se algum de T09 a T14 falhar, **antes do commit**: trocar a constante `mp4_fragment_duration_microseconds` para 0, recompilar, regerar os dois arquivos e repetir T09 a T14. Se passarem, commitar com a constante em 0 e registrar em DEV qual player falhou, com que arquivo e de que forma; o comentário da constante passa a explicar a retirada. Se falharem também com 0, não commitar: BLOCKED, e o projeto fica só com E04.

**Regressão obrigatória:** R1 a R8. R9 não se aplica (o log não muda, mas o formato sim).

**Critério de pronto:** checks PASS, incluindo T11 a T14 confirmados pelo Davi. **Reverter:** `git revert <commit>`; o projeto volta ao MP4 comum com a proteção de E04.

```text
TASK
ID: E05
BASE: feat/fila-unica@<commit da etapa anterior>
SPEC: motion_mog2/specs/plano_mudancas_v2.md (versao 2), secao 4, etapa E05. D1 confirmada: opcao C com frag_keyframe+empty_moov+default_base_moof, frag_duration de 1 s e flush_packets=1
SCOPE: motion_mog2/include/ffmpeg/AvMp4OutputContext.hpp; motion_mog2/src/ffmpeg/AvMp4OutputContext.cpp; motion_mog2/src/recording/Mp4EventPacketSink.cpp; motion_mog2/tests/mp4_event_packet_sink_tests.cpp; motion_mog2/tests/av_output_format_context_tests.cpp
BUILD: cmake -S motion_mog2 -B motion_mog2/build -DOpenCV_DIR=/home/davi/Downloads/tcc/opencv/install-cuda125/lib/cmake/opencv4 -DBUILD_TESTING=ON && cmake --build motion_mog2/build
PRE: git status limpo; com o binario da base, executar E05-T05 e confirmar que o MP4 nao abre apos kill -9. Se abrir => BLOCKED
CHECK C1: BUILD termina sem erro
CHECK C2: av_output_format_context_tests e mp4_event_packet_sink_tests terminam com codigo 0, com PASS para E05-T01 a E05-T04
CHECK C3: E05-T04 -> quatro linhas KILL_RECOVERY; com 1 s, packets_recovered >= packets_written - 61; com 0, 600 (N=900) e 0 (N=300)
CHECK C4: E05-T05 -> MP4CHECK sao; arquivo copiado para /tmp/tcc_e05_kill.mp4
CHECK C5: R2
CHECK C6: R3 e R4 com os trechos do baseline; R5 com todos os MP4 saos
CHECK C7: E05-T08 -> duracoes dentro da tolerancia
CHECK C8: R7 e R8
CHECK C9: E05-T09 e E05-T10 (ffplay) -> codigo 0 e saida de erro vazia
CHECK C10: E05-T15 (GStreamer sem tela) -> 0 nos dois arquivos, ou SKIP se nao houver gst-launch-1.0
CHECK C11: E05-T11 a E05-T14 (GNOME Videos e Chrome/Edge) -> confirmacao visual do Davi. NAO commitar antes dessa confirmacao; reportar como UNV e aguardar
OUT O1: saida dos dois executaveis de teste, incluindo as quatro linhas KILL_RECOVERY
OUT O2: linhas MP4CHECK de C4 e C6
OUT O3: linhas RANGE de C6 e duracoes de C7
OUT O4: resultado de cada player (T09 a T15), com nome e versao do player
OUT O5: se a constante de duracao foi para 0, qual player falhou e como
COMMIT: feat: grava MP4 fragmentado com descarga por pacote para sobreviver a interrupcao
PUSH: origin feat/fila-unica
```

---

### E06 — Política para carimbo inválido

- **Itens atendidos:** 2.2. **Dependências:** E04; **D2 confirmada**. Se D2 não for a opção c, esta etapa precisa ser reescrita.

**Objetivo e por que esta solução.** Um pacote sem `pts` ou `dts`, com `pts < dts` ou com `dts` não crescente faz `writeMp4Packets` lançar (`src/ffmpeg/AvMp4OutputContext.cpp:215-222`, `:162-169`), e a exceção encerra o processo. A política c fecha o arquivo antes do pacote inválido e retoma no próximo keyframe válido. As alternativas e o motivo de validar na escrita estão em D2.

**Escopo de arquivos**

- Criar: `motion_mog2/include/recording/PacketTimestampCheck.hpp`, `motion_mog2/src/recording/PacketTimestampCheck.cpp`, `motion_mog2/tests/packet_timestamp_check_tests.cpp`.
- Alterar: `motion_mog2/include/recording/EventRecorder.hpp`, `motion_mog2/src/recording/EventRecorder.cpp`, `motion_mog2/tests/event_recorder_tests.cpp`, `motion_mog2/CMakeLists.txt`.

**Mudança**

Função pura:

```cpp
enum class PacketTimestampIssue { none, missing_timestamp, pts_before_dts, non_increasing_dts };

PacketTimestampIssue checkPacketTimestampsForMuxing(
    const AVPacket& encoded_packet, std::optional<std::int64_t> previous_written_dts);
```

Ordem das verificações: carimbo ausente; `pts < dts`; `dts <= previous_written_dts` quando houver anterior. As três correspondem a `ff: libavformat/mux.c:567-583`.

Em `EventRecorder`, todo envio de pacotes ao sink passa por um método privado único, que substitui as chamadas diretas a `writePackets`. A abertura da gravação deixa de acontecer antes da escrita e passa a acontecer **quando há o primeiro pacote válido para escrever**. O recorder guarda `last_written_dts_` (um `std::optional<std::int64_t>`), zerado a cada abertura.

```text
escrever_lote(lote, movimento_ativo):
  para cada pacote, em ordem:
    problema = checkPacketTimestampsForMuxing(pacote, last_written_dts_)
    se problema == none:
        se não há gravação aberta: abrir (o pacote é keyframe por construção)
        acumular no trecho válido; atualizar last_written_dts_
    senão:
        escrever o trecho válido acumulado
        se há gravação aberta: finalizar (log MP4_SAVED normal)
        descartar este pacote e os seguintes até achar, no mesmo lote,
          um keyframe com problema == none avaliado sem anterior
        se achou e movimento_ativo: retomar a partir dele (abre nova gravação)
        senão: descartar o resto do lote
        registrar EVENT_INVALID_TIMESTAMP
  escrever o trecho válido acumulado
```

`movimento_ativo` é verdadeiro quando a chamada vem do início de evento, de `waiting_keyframe` ou de `recording`, e falso em `post_event`, `closing_at_keyframe`. Em `finishCapture` vale o estado em que a captura terminou.

Estado ao final do lote, quando houve pacote inválido e nenhuma gravação ficou aberta: se `movimento_ativo`, o estado passa a `waiting_keyframe` (as leituras seguintes procuram o próximo keyframe, como já acontece hoje); senão, passa a `idle`. Quando uma gravação ficou aberta, o estado segue o fluxo normal.

Log novo, uma linha por lote afetado:

`EVENT_INVALID_TIMESTAMP reason=<missing_timestamp|pts_before_dts|non_increasing_dts> frame=<indice do quadro> dropped_packets=<n> resumed=<yes|no>`

`dropped_packets` conta só os pacotes descartados dentro do lote. A validação de `writeMp4Packets` não muda; se algo passar pela política e falhar no muxer, vale E04.

**Invariantes e riscos.** I1 (os pacotes descartados já tinham saído da fila; nada volta para ela), I3, I4. Riscos: mudar o momento da abertura altera a ordem `MP4_STARTED` / `PRE_EVENT_WRITTEN`; a ordem atual (abrir, escrever, registrar) deve ser mantida para fluxo sem pacote inválido, e E03-T10 verifica. Com fonte em que todos os pacotes são inválidos, sai uma linha de log a cada keyframe tentado.

**Casos de teste**

| ID | Tipo | Objetivo | Preparo | Passos | Resultado esperado | Critério |
|---|---|---|---|---|---|---|
| E06-T01 | Unitário sem GPU | Função pura | Pacotes sintéticos | Tabela: válido; sem `pts`; sem `dts`; `pts < dts`; `dts` igual ao anterior; `dts` menor; sem anterior | `none`, `missing_timestamp` (2x), `pts_before_dts`, `non_increasing_dts` (2x), `none` | Asserções |
| E06-T02 | Unitário sem GPU | Primeiro keyframe sem carimbo no início do evento | Fila: K0 sem carimbo, 59 válidos, K60 válido...; `started` com tudo contabilizado | Processar | Nenhuma gravação começa em K0; gravação começa em K60; log com `reason=missing_timestamp resumed=yes dropped_packets=60` | Identificadores e log |
| E06-T03 | Unitário sem GPU | `dts` repetido no meio, com keyframe no mesmo lote | Gravando; lote de 120 com `dts` repetido no índice 10 e keyframe no 60 | Processar | Primeira gravação termina no índice 9; segunda começa no 60; `dropped_packets=50 resumed=yes` | Idem |
| E06-T04 | Unitário sem GPU | `dts` repetido no meio, sem keyframe no lote | Gravando; lote de 5 com defeito no índice 2 | Processar; depois alimentar até o próximo keyframe | Gravação fecha com 2 pacotes do lote; estado `waiting_keyframe`; nova gravação no keyframe | Estado e identificadores |
| E06-T05 | Unitário sem GPU | Inválido em `post_event` | Pós-evento em curso; lote com defeito | Processar | Gravação finalizada; estado `idle`; `resumed=no` | Idem |
| E06-T06 | Unitário sem GPU | Inválido em `closing_at_keyframe` | Idem | Idem | Idem | Idem |
| E06-T07 | Unitário sem GPU | Todos os pacotes sem carimbo | Baseline sintético com todos os carimbos ausentes | Processar tudo e `finishCapture` | Nenhuma gravação; nenhuma exceção | Zero gravações |
| E06-T08 | Unitário sem GPU | `pts < dts` | Um pacote com `pts = dts - 1` no meio | Processar | Mesmo tratamento de T04, `reason=pts_before_dts` | Log |
| E06-T09 | Unitário sem GPU | `finishCapture` com inválido nos restantes | Gravando; restantes com defeito no meio e keyframe depois | `finishCapture` | Gravação fecha antes do defeito; nova gravação a partir do keyframe, finalizada | Identificadores |
| E06-T10 | Unitário sem GPU | Fluxo válido inalterado | Baseline sintético | E03-T01, T02, T10 | Mesmos resultados de E03 | Idem |
| E06-T11 | Integração com GPU | `dts` repetido em arquivo real | G3 | `"$BIN" /tmp/tcc_g3_dup_dts.mkv /tmp/tcc_e06a 5 1.0 2 3 5; echo exit=$?; tcc_ranges "$VIDEO" /tmp/tcc_e06a` | `exit=0`; nenhum `[fatal]`; log com `EVENT_INVALID_TIMESTAMP reason=non_increasing_dts`; nenhum MP4 contém o pacote 200; todos sãos. Trechos esperados [Provável]: [0,199], [240,779], [780,1180] | Critério firme: os quatro primeiros pontos. Os trechos exatos são derivados por raciocínio; se diferirem, relatar em DEV sem corrigir |
| E06-T12 | Integração com GPU | Fonte sem nenhum carimbo | G4 | `"$BIN" /tmp/tcc_g4_sem_ts.h264 /tmp/tcc_e06b 5 1.0 2 3 5; echo exit=$?` | `exit=0`; nenhum `[fatal]`; nenhum MP4; pelo menos uma linha `EVENT_INVALID_TIMESTAMP reason=missing_timestamp` | Idem. Se a fonte não abrir o `.h264` cru, SKIP com a mensagem |
| E06-T13 | Manual com RTSP | Sessão RTSP normal | RIG com PUB-COPY | Gravar 60 s; Ctrl+C | Eventos gravados; MP4s sãos; se a linha `EVENT_INVALID_TIMESTAMP` aparecer, só no primeiro evento | Log |

No trecho de T11: o evento 1 do baseline vai dos quadros 118 a 205. O pacote 200 fecha o arquivo em 199; o movimento termina em 205, antes do keyframe 240, então a continuação é descartada (`EVENT_DISCARDED_NO_KEYFRAME`). Seis quadros de movimento (200 a 205) ficam fora de arquivo. É o custo da opção c, e por isso R6 não se aplica a G3.

**Regressão obrigatória:** R1 a R8.

**Critério de pronto:** checks PASS. **Reverter:** `git revert <commit>`.

```text
TASK
ID: E06
BASE: feat/fila-unica@<commit da etapa anterior>
SPEC: motion_mog2/specs/plano_mudancas_v2.md, secao 4, etapa E06. Pre-requisito: D2 confirmada pelo Davi com a opcao c
SCOPE: motion_mog2/include/recording/PacketTimestampCheck.hpp (criar); motion_mog2/src/recording/PacketTimestampCheck.cpp (criar); motion_mog2/tests/packet_timestamp_check_tests.cpp (criar); motion_mog2/include/recording/EventRecorder.hpp; motion_mog2/src/recording/EventRecorder.cpp; motion_mog2/tests/event_recorder_tests.cpp; motion_mog2/CMakeLists.txt
BUILD: cmake -S motion_mog2 -B motion_mog2/build -DOpenCV_DIR=/home/davi/Downloads/tcc/opencv/install-cuda125/lib/cmake/opencv4 -DBUILD_TESTING=ON && cmake --build motion_mog2/build
PRE: git status limpo; gerar G3 e G4; com o binario da base, rodar E06-T11 e confirmar exit=2 (comportamento de E04). Se exit=0 => BLOCKED
CHECK C1: BUILD termina sem erro
CHECK C2: packet_timestamp_check_tests e event_recorder_tests terminam com codigo 0, com PASS para E06-T01 a E06-T10
CHECK C3: E06-T11 -> exit=0, sem [fatal], EVENT_INVALID_TIMESTAMP presente, MP4 saos, nenhum RANGE contendo o indice 200
CHECK C4: E06-T12 -> exit=0, sem [fatal], nenhum MP4 (ou SKIP justificado)
CHECK C5: R2
CHECK C6: R3 e R4 com os trechos do baseline
CHECK C7: R5
CHECK C8: R7 e R8
OUT O1: saida dos dois executaveis de teste
OUT O2: log completo do gravador e linhas RANGE e MP4CHECK de C3
OUT O3: log completo do gravador de C4
COMMIT: fix: fecha e retoma a gravacao em pacote com timestamp invalido em vez de encerrar
PUSH: origin feat/fila-unica
```

---

### E07 — Motivo do fim da leitura e códigos de saída

- **Itens atendidos:** 2.3. **Dependências:** E03; **D3 confirmada**.

**Objetivo e por que esta solução.** `getNextPacket` devolve `false` para qualquer retorno negativo de `av_read_frame`, sem guardar o código (`src/input/FfmpegEncodedVideoSource.cpp:269-271`), e o programa termina com 0. A fonte passa a guardar o motivo em estado atômico, o `main` o lê depois de `stopDecoding()` e escolhe o código de saída. Alternativas em D3.

**Escopo de arquivos**

- Criar: `motion_mog2/include/input/PacketReadEnd.hpp`, `motion_mog2/include/app/CaptureEnd.hpp`, `motion_mog2/src/app/CaptureEnd.cpp`, `motion_mog2/tests/capture_end_tests.cpp`.
- Alterar: `motion_mog2/include/input/FfmpegEncodedVideoSource.hpp`, `motion_mog2/src/input/FfmpegEncodedVideoSource.cpp`, `motion_mog2/include/input/VideoStreamReader.hpp`, `motion_mog2/src/input/VideoStreamReader.cpp`, `motion_mog2/src/main.cpp`, `motion_mog2/tests/ffmpeg_encoded_video_packets_tests.cpp`, `motion_mog2/CMakeLists.txt`.

**Mudança**

```cpp
// PacketReadEnd.hpp
enum class PacketReadEndReason { not_ended, end_of_stream, interrupted, read_error, packet_preparation_error };
struct PacketReadEnd { PacketReadEndReason reason = PacketReadEndReason::not_ended; int ffmpeg_error_code = 0; };

// FfmpegEncodedVideoSource e VideoStreamReader
PacketReadEnd getPacketReadEnd() const;

// CaptureEnd.hpp (sem OpenCV)
enum class CaptureEndReason { file_end, interrupted, live_source_closed, read_error, packet_error };
CaptureEndReason classifyCaptureEnd(PacketReadEnd packet_read_end, bool source_is_live, bool interrupted_by_signal);
int getExitCodeForCaptureEnd(CaptureEndReason capture_end_reason);
const char* getCaptureEndReasonName(CaptureEndReason capture_end_reason);
```

- Na fonte, dois membros atômicos (motivo e código). `getNextPacket` grava o código e depois o motivo imediatamente antes de cada `return false`: `AVERROR_EOF` → `end_of_stream`; `AVERROR_EXIT` → `interrupted`; outro negativo → `read_error`; as saídas das linhas 287-292 e 301-304 → `packet_preparation_error`. A primeira causa registrada prevalece.
- `VideoStreamReader::getPacketReadEnd()` repassa. Comentário: só é confiável depois de `stopDecoding()`, que espera a thread de leitura (`ocv: video_reader.cpp:202-206`, `video_source.cpp:90-95`).
- `classifyCaptureEnd`: `interrupted_by_signal` vence; depois `end_of_stream` vira `file_end` ou `live_source_closed` conforme `source_is_live`; `read_error` e `packet_preparation_error` mapeiam direto; `not_ended` sem sinal vira `read_error` com código 0.
- No `main`, depois de `video_reader.stopDecoding()` e antes do `finishCapture`: `interrupted_by_signal = !::capture_active`; `source_is_live` é verdadeiro quando `input_source` **não** é um arquivo regular (`std::filesystem::is_regular_file` na forma que não lança). Imprimir:

  `CAPTURE_END reason=<file_end|interrupted|live_source_closed|read_error|packet_error> av_error=<codigo> detail="<texto de av_strerror ou vazio>"`

  O encerramento do evento em curso não muda. O `return 0` final (`src/main.cpp:373`) passa a devolver `getExitCodeForCaptureEnd`. Os retornos 1 e 2 existentes não mudam.

Códigos: `file_end` 0; `interrupted` 0; `live_source_closed` 3; `read_error` 4; `packet_error` 5.

**Invariantes e riscos.** I6 (nada muda na entrega de pacotes). Riscos: ler o motivo antes de a thread terminar; scripts que tratem qualquer código diferente de 0 como falha de build. `run_rtsp_recording.sh` repassa o código do gravador, o que é o efeito desejado.

**Casos de teste**

| ID | Tipo | Objetivo | Preparo | Passos | Resultado esperado | Critério |
|---|---|---|---|---|---|---|
| E07-T01 | Unitário sem GPU | Sinal vence | — | `classifyCaptureEnd({end_of_stream, EOF}, true, true)` | `interrupted`; código 0 | Asserções |
| E07-T02 | Unitário sem GPU | Fim de arquivo | — | `({end_of_stream, EOF}, false, false)` | `file_end`; 0 | Idem |
| E07-T03 | Unitário sem GPU | Fonte ao vivo encerrou | — | `({end_of_stream, EOF}, true, false)` | `live_source_closed`; 3 | Idem |
| E07-T04 | Unitário sem GPU | Erro de leitura | — | `({read_error, -5}, true, false)` | `read_error`; 4 | Idem |
| E07-T05 | Unitário sem GPU | Erro de preparo | — | `({packet_preparation_error, 0}, false, false)` | `packet_error`; 5 | Idem |
| E07-T06 | Unitário sem GPU | Fim sem motivo | — | `({not_ended, 0}, false, false)` | `read_error`; 4 | Idem |
| E07-T07 | Integração sem GPU (exige cabeçalhos do OpenCV) | Fonte registra fim de arquivo | G2 | Em `ffmpeg_encoded_video_packets_tests`, ler até `getNextPacket` devolver falso | `reason == end_of_stream`; código `AVERROR_EOF` | `$BUILD/ffmpeg_encoded_video_packets_tests /tmp/tcc_g2.mp4` termina com 0 |
| E07-T08 | Integração sem GPU | Fonte registra interrupção | G2; `keep_running` falso antes da leitura | `getNextPacket` | Falso; `reason == interrupted` | Idem. Se `av_read_frame` em arquivo local não consultar o callback, registrar em UNV e cobrir por T11 |
| E07-T09 | Manual com RTSP | Publicador cai em `idle` | RIG com PUB-COPY; gravador em execução | `kill -9 <pid do ffmpeg publicador>`; `echo $?` do gravador | `CAPTURE_END reason=live_source_closed`; código 3 | Log e código. Medido nesta análise: `av_read_frame` devolve `AVERROR_EOF` nesse cenário |
| E07-T10 | Manual com RTSP | Publicador cai durante evento | Idem; esperar `EVENT_STARTED` | Idem | Código 3; `SHUTDOWN_FLUSHED`; MP4 são | Log, código, `MP4CHECK` |
| E07-T11 | Manual com RTSP | Ctrl+C | Idem | Ctrl+C no gravador | `CAPTURE_END reason=interrupted`; código 0 | Log e código |
| E07-T12 | Integração com GPU | Fim de arquivo | G1 | R3 | `CAPTURE_END reason=file_end`; `exit=0` | Log e código |
| E07-T13 | Manual com RTSP | Servidor cai | RIG | `kill -9 <pid do MediaMTX>` | Código 3 | Idem T09 |
| E07-T14 | Manual | Fonte inexistente | Sem servidor | `"$BIN" "$RTSP_URL" /tmp/x; echo $?` | `[fatal]` com erro de conexão; código 2 (inalterado) | Código |

**Regressão obrigatória:** R1 a R8. Em R9 a única diferença aceita é a linha `CAPTURE_END`.

**Critério de pronto:** checks PASS. **Reverter:** `git revert <commit>`.

```text
TASK
ID: E07
BASE: feat/fila-unica@<commit da etapa anterior>
SPEC: motion_mog2/specs/plano_mudancas_v2.md, secao 4, etapa E07. Pre-requisito: D3 confirmada pelo Davi
SCOPE: motion_mog2/include/input/PacketReadEnd.hpp (criar); motion_mog2/include/app/CaptureEnd.hpp (criar); motion_mog2/src/app/CaptureEnd.cpp (criar); motion_mog2/tests/capture_end_tests.cpp (criar); motion_mog2/include/input/FfmpegEncodedVideoSource.hpp; motion_mog2/src/input/FfmpegEncodedVideoSource.cpp; motion_mog2/include/input/VideoStreamReader.hpp; motion_mog2/src/input/VideoStreamReader.cpp; motion_mog2/src/main.cpp; motion_mog2/tests/ffmpeg_encoded_video_packets_tests.cpp; motion_mog2/CMakeLists.txt
BUILD: cmake -S motion_mog2 -B motion_mog2/build -DOpenCV_DIR=/home/davi/Downloads/tcc/opencv/install-cuda125/lib/cmake/opencv4 -DBUILD_TESTING=ON && cmake --build motion_mog2/build
PRE: git status limpo; gerar G2; com o binario da base, executar E07-T09 e confirmar codigo 0 sem linha de motivo. Rodar motion_mog2/build/ffmpeg_encoded_video_packets_tests /tmp/tcc_g2.mp4 na base e anotar o resultado
CHECK C1: BUILD termina sem erro
CHECK C2: capture_end_tests termina com codigo 0, com PASS para E07-T01 a E07-T06
CHECK C3: ffmpeg_encoded_video_packets_tests /tmp/tcc_g2.mp4 termina com codigo 0 (E07-T07, E07-T08)
CHECK C4: E07-T09 -> codigo 3 e CAPTURE_END reason=live_source_closed
CHECK C5: E07-T10 -> codigo 3, SHUTDOWN_FLUSHED, MP4 sao
CHECK C6: E07-T11 -> codigo 0 e reason=interrupted
CHECK C7: R3 -> exit=0, reason=file_end, trechos do baseline; R4 idem
CHECK C8: R2, R5, R7, R8
OUT O1: saida de capture_end_tests
OUT O2: ultimas 8 linhas do log do gravador em C4, C5, C6 e C7
COMMIT: feat: registra o motivo do fim da leitura e devolve codigo de saida correspondente
PUSH: origin feat/fila-unica
```

---

### E08 — SPS/PPS para o NVDEC quando não vêm no fluxo

- **Itens atendidos:** 2.5 (primeira metade). **Dependências:** E07 (para o log de motivo nos testes), nenhuma decisão.

**Objetivo e por que esta solução.** Em RTSP os pacotes já chegam em Annex B e o filtro `h264_mp4toannexb` os repassa sem inserir nada (`ff: libavcodec/h264_mp4toannexb_bsf.c:146-150, 185-188`). Se a câmera manda SPS/PPS só no SDP, eles ficam na extradata e nunca chegam ao NVDEC, porque o OpenCV apenas guarda a extradata (`ocv: video_source.cpp:125-131`). A fonte embutida do OpenCV resolve isso prefixando a extradata ao **primeiro** pacote (`ocv: ffmpeg_video_source.cpp:197-208`).

A solução aqui é prefixar a extradata a **cada keyframe que não contenha SPS**, somente na cópia entregue ao NVDEC. Motivo de não copiar a abordagem do primeiro pacote: numa sessão RTSP o primeiro pacote quase nunca é keyframe; dar SPS/PPS ao decoder nesse ponto o faria iniciar no meio de um GOP. Prefixar nos keyframes reproduz o que um fluxo com parâmetros embutidos entrega (medido: com `h264_mp4toannexb` no publicador, cada pacote IDR traz SPS), que é o caso em que o baseline já funciona.

O pacote clonado para a fila não é alterado. O MP4 recebe SPS/PPS pela extradata do contexto de saída, como hoje.

**Escopo de arquivos**

- Criar: `motion_mog2/include/ffmpeg/H264NalUnits.hpp`, `motion_mog2/src/ffmpeg/H264NalUnits.cpp`, `motion_mog2/tests/h264_nal_units_tests.cpp`, `motion_mog2/tests/ffmpeg_source_parameter_sets_tests.cpp`.
- Alterar: `motion_mog2/include/input/FfmpegEncodedVideoSource.hpp`, `motion_mog2/src/input/FfmpegEncodedVideoSource.cpp`, `motion_mog2/CMakeLists.txt`.

**Mudança**

Funções puras (sem FFmpeg além dos tipos básicos):

```cpp
bool checkIfBufferStartsWithAnnexBStartCode(const unsigned char* data, std::size_t size);
bool checkIfAnnexBContainsNalUnitType(const unsigned char* data, std::size_t size, int nal_unit_type);
```

A segunda percorre os códigos de início de 3 e de 4 bytes e compara `byte_apos_o_codigo & 0x1F` com o tipo pedido. Dados nulos ou tamanho menor que 4 devolvem falso.

Na fonte:

- Novo membro `parameter_sets_prefix_enabled_`, definido no construtor: verdadeiro quando a extradata começa com código de início Annex B **e** contém NAL 7. Para entrada em AVCC (arquivos MP4 e MKV) fica falso; nesse caso o filtro já insere SPS/PPS.
- Novo membro `encoded_packet_with_parameter_sets_for_nvdec_` (`std::vector<unsigned char>`), que vive até a próxima chamada de `getNextPacket`. O OpenCV copia os bytes de forma síncrona (`ocv: video_parser.cpp:78-79`), então esse tempo de vida basta.
- Em `getNextPacket`, depois de obter o pacote Annex B e de inserir o clone na fila: se o prefixo está habilitado, o pacote original tem `AV_PKT_FLAG_KEY` e o pacote Annex B não contém NAL 7, o vetor recebe extradata seguida do pacote, e os ponteiros de saída apontam para o vetor. Caso contrário, nada muda.
- Log, uma única vez por execução, com `CV_LOG_WARNING`: `SPS/PPS ausentes no fluxo; a extradata sera prefixada aos keyframes entregues ao NVDEC.`

No CMake: `H264NalUnits.cpp` entra no executável e nos alvos que compilam a fonte; `h264_nal_units_tests` sem OpenCV, com `add_test`; `ffmpeg_source_parameter_sets_tests` com OpenCV, sem `add_test` (recebe a URL como argumento).

**Invariantes e riscos.** I5, I6 (um pacote ao NVDEC por pacote na fila; só o conteúdo da cópia muda), I1. Riscos: prefixar em fluxo que já tem SPS (a verificação de NAL 7 evita); alterar por engano o clone da fila (T08 verifica); custo de uma cópia por keyframe.

**Casos de teste**

| ID | Tipo | Objetivo | Preparo | Passos | Resultado esperado | Critério |
|---|---|---|---|---|---|---|
| E08-T01 | Unitário sem GPU | Código de início de 4 bytes | `00 00 00 01 67 ...` | Procurar tipo 7 | Verdadeiro | Asserções |
| E08-T02 | Unitário sem GPU | Código de início de 3 bytes | `00 00 01 65 ...` | Procurar tipo 5 | Verdadeiro | Idem |
| E08-T03 | Unitário sem GPU | Vários NAL | AUD, SEI, IDR em sequência | Procurar 5, 7 | Verdadeiro, falso | Idem |
| E08-T04 | Unitário sem GPU | Tipo usa só 5 bits | Cabeçalho `0x67` e `0x27` | Procurar 7 | Verdadeiro nos dois | Idem |
| E08-T05 | Unitário sem GPU | Bordas | Nulo; tamanho 0; 3 bytes; código de início no fim sem cabeçalho | Procurar | Falso; sem leitura fora do buffer | Idem |
| E08-T06 | Unitário sem GPU | Detecção de Annex B | `00 00 01`, `00 00 00 01`, `01 64 00 1f` | `checkIfBufferStartsWithAnnexBStartCode` | Verdadeiro, verdadeiro, falso | Idem |
| E08-T07 | Medição | O arranjo não tem SPS no fluxo | RIG com PUB-NOSPS | MED-1 | 1 | Valor 1 |
| E08-T08 | Integração sem GPU (exige OpenCV e RIG) | Prefixo só na cópia do NVDEC | RIG com PUB-NOSPS | `$BUILD/ffmpeg_source_parameter_sets_tests "$RTSP_URL" tcp`: chamar `getNextPacket` até o primeiro keyframe | Bytes do NVDEC começam com a extradata e contêm NAL 7; tamanho = extradata + clone; os bytes finais são idênticos ao clone; o clone não contém NAL 7; pacotes não-keyframe sem prefixo | Código 0 |
| E08-T09 | Integração sem GPU (exige OpenCV e RIG) | Sem prefixo quando o fluxo já traz SPS | RIG com PUB-SPS | Mesmo teste com a opção de esperar ausência de prefixo | Bytes do NVDEC idênticos ao clone em todos os pacotes | Código 0 |
| E08-T10 | Manual com RTSP e GPU | Reproduzir a falha e a correção | RIG com PUB-NOSPS | `timeout 30 "$BIN" "$RTSP_URL" /tmp/tcc_e08 5 1.0 2 3 5 tcp; echo exit=$?` | No PRE (base) [Provável, nunca executado]: sem linha `STREAM_FPS`; `exit=124`. Depois: `STREAM_FPS` aparece, eventos são gravados, MP4s sãos | Depois: pelo menos um `MP4_SAVED` em 30 s e `MP4CHECK` são |
| E08-T11 | Manual com RTSP e GPU | Controle positivo | RIG com PUB-SPS | Idem | Funciona antes e depois; sem a mensagem de prefixo | Log |
| E08-T12 | Integração com GPU | Arquivos inalterados | G1 | R3, R4 | Trechos do baseline; sem a mensagem de prefixo | R3, R4 |
| E08-T13 | Medição | Como é o fluxo do baseline do Davi | RIG com PUB-COPY e G1 | MED-1 | Informativo: maior que 1 indica SPS no fluxo | Colar o valor |
| E08-T14 | Medição | Como é o fluxo do modo webcam | RIG com PUB-WEBCAM-EQ, ou `run_rtsp_recording.sh --webcam` em outro terminal | MED-1 | Informativo. Medido nesta análise com PUB-WEBCAM-EQ: 1 (sem SPS no fluxo) | Colar o valor |

Se o PRE de T10 não reproduzir a falha (a base imprime `STREAM_FPS` com PUB-NOSPS), o item 2.5 não se confirma nesta máquina: a etapa para com BLOCKED e o resultado volta para o Davi.

Nota da versão 2: medi que a codificação do modo `--webcam` chega sem SPS no fluxo (PUB-WEBCAM-EQ, MED-1 = 1). Então `run_rtsp_recording.sh --webcam` na base é um segundo jeito de reproduzir a falha, e a memória do Davi sobre esse modo já ter funcionado, ou não, antecipa o resultado do PRE. E08-T14 registra essa medição; ela é informativa e não muda os checks.

**Regressão obrigatória:** R1 a R8.

**Critério de pronto:** checks PASS. **Reverter:** `git revert <commit>`.

```text
TASK
ID: E08
BASE: feat/fila-unica@<commit da etapa anterior>
SPEC: motion_mog2/specs/plano_mudancas_v2.md, secao 4, etapa E08
SCOPE: motion_mog2/include/ffmpeg/H264NalUnits.hpp (criar); motion_mog2/src/ffmpeg/H264NalUnits.cpp (criar); motion_mog2/tests/h264_nal_units_tests.cpp (criar); motion_mog2/tests/ffmpeg_source_parameter_sets_tests.cpp (criar); motion_mog2/include/input/FfmpegEncodedVideoSource.hpp; motion_mog2/src/input/FfmpegEncodedVideoSource.cpp; motion_mog2/CMakeLists.txt
BUILD: cmake -S motion_mog2 -B motion_mog2/build -DOpenCV_DIR=/home/davi/Downloads/tcc/opencv/install-cuda125/lib/cmake/opencv4 -DBUILD_TESTING=ON && cmake --build motion_mog2/build
PRE: git status limpo; subir o RIG com PUB-NOSPS; MED-1 deve dar 1 (senao BLOCKED); com o binario da base, rodar E08-T10 e confirmar ausencia de STREAM_FPS e exit=124 (senao BLOCKED)
CHECK C1: BUILD termina sem erro
CHECK C2: h264_nal_units_tests termina com codigo 0, com PASS para E08-T01 a E08-T06
CHECK C3: E08-T08 -> codigo 0
CHECK C4: E08-T09 -> codigo 0
CHECK C5: E08-T10 -> STREAM_FPS presente, pelo menos um MP4_SAVED, MP4 saos
CHECK C6: E08-T11 -> funciona sem a mensagem de prefixo
CHECK C7: R2; R3 e R4 com os trechos do baseline; R5
CHECK C8: R7 e R8
OUT O1: saida de h264_nal_units_tests e de ffmpeg_source_parameter_sets_tests
OUT O2: valores de MED-1 para PUB-NOSPS, PUB-SPS, PUB-COPY e PUB-WEBCAM-EQ (ou webcam real)
OUT O3: log do gravador no PRE e em C5
COMMIT: fix: entrega SPS e PPS da extradata ao NVDEC quando o fluxo nao os contem
PUSH: origin feat/fila-unica
```

---

### E09 — Teto para a região à frente da fronteira

- **Itens atendidos:** 2.5 (segunda metade). **Dependências:** E07 (enum de motivo), E08. Valor de 40 s aprovado pelo Davi.

**Objetivo e por que esta solução.** Se o decoder para de entregar quadros, ninguém avança a fronteira e a fila cresce sem limite; `udpSource = true` desliga a proteção do OpenCV (`ocv: video_parser.cpp:51, 98-106`). E08 remove a causa conhecida; esta etapa limita o dano das desconhecidas.

Descartar pacotes da região à frente foi rejeitado: quebraria I3 e a correspondência com N (I6). A solução é **parar a leitura** com motivo explícito quando a região à frente passa de um limite. O limite não pode vir do `main`, porque o travamento pode ocorrer dentro do construtor do leitor; por isso a fonte o calcula a partir do FPS que ela mesma obtém.

**Escopo de arquivos**

- Alterar: `motion_mog2/include/buffers/EncodedPacketQueue.hpp`, `motion_mog2/src/buffers/EncodedPacketQueue.cpp`, `motion_mog2/tests/encoded_packet_queue_tests.cpp`, `motion_mog2/include/input/PacketReadEnd.hpp`, `motion_mog2/include/input/FfmpegEncodedVideoSource.hpp`, `motion_mog2/src/input/FfmpegEncodedVideoSource.cpp`, `motion_mog2/include/app/CaptureEnd.hpp`, `motion_mog2/src/app/CaptureEnd.cpp`, `motion_mog2/tests/capture_end_tests.cpp`, `motion_mog2/tests/ffmpeg_encoded_video_packets_tests.cpp`.

**Mudança**

- Fila: `std::size_t getUncountedPacketCount() const;` devolve tamanho menos fronteira, sob o mutex.
- Fonte: parâmetro novo no construtor, `std::size_t maximum_uncounted_packet_count = 0`. Zero significa derivar: `ceil(fps * 40) + 32`. Os 40 s são o dobro da suposição de GOP de 20 s do `main` (`src/main.cpp:140`); os 32 são a margem de superfícies. Comentário obrigatório no código, junto da constante, com este conteúdo: os 40 s são duas vezes o GOP suposto; na partida de uma sessão a região à frente cresce de forma legítima até o primeiro keyframe, porque os pacotes anteriores a ele não geram quadro e por isso não são contabilizados; o teto precisa ficar acima de um GOP inteiro para não disparar nesse período. Novo método `std::size_t getMaximumUncountedPacketCount() const`.
- Em `getNextPacket`, antes de `pushPacket`: se `getUncountedPacketCount() >= limite`, registrar com `CV_LOG_ERROR` a linha `Regiao a frente da fronteira atingiu <n> pacotes sem contabilizacao pelo decoder; leitura encerrada.`, gravar o motivo `decoder_stalled` e devolver falso.
- `PacketReadEndReason` ganha `decoder_stalled`; `CaptureEndReason` ganha `decoder_stalled`, com nome `decoder_stalled` e código de saída 6.

Efeito: durante o `read()`, o `grab` devolve falso e o `main` encerra com `CAPTURE_END reason=decoder_stalled`, código 6, depois de finalizar o evento em curso. Durante o construtor, o OpenCV lança `cv::Exception` (`ocv: video_reader.cpp:170-177`) e o `main` sai com 2, mas a linha de erro acima já explica a causa.

Por que não há falso positivo em operação normal [Provável]: com `allowFrameDrop = false`, a thread de leitura bloqueia quando a fila de quadros do OpenCV enche (`ocv: frame_queue.cpp:113-136`), então a região à frente fica limitada ao que o decoder tem em trânsito. No início de uma sessão RTSP os pacotes anteriores ao primeiro IDR não geram quadro, o que dá no máximo um GOP.

**Invariantes e riscos.** I1, I3 (nada é retirado), I6. Riscos: câmera com GOP acima de 40 s dispararia o teto na partida.

**Casos de teste**

| ID | Tipo | Objetivo | Preparo | Passos | Resultado esperado | Critério |
|---|---|---|---|---|---|---|
| E09-T01 | Unitário sem GPU | Contagem da região à frente | Fila vazia | inserir 5; contabilizar 2; retirar contabilizados; inserir 1 | 5, 3, 3, 4 | Asserções |
| E09-T02 | Unitário sem GPU | `takeAllPackets` zera | Fila com 4 à frente | `takeAllPackets` | 0 | Idem |
| E09-T03 | Unitário sem GPU | Classificação | — | `classifyCaptureEnd({decoder_stalled, 0}, true, false)` | `decoder_stalled`; código 6 | Idem |
| E09-T04 | Integração sem GPU (exige OpenCV) | Limite injetado | G2; fonte com limite 10 | 11 chamadas de `getNextPacket` sem contabilizar | 10 verdadeiras, a 11ª falsa; motivo `decoder_stalled`; fila com 10 pacotes | Código 0 do teste |
| E09-T05 | Integração sem GPU (exige OpenCV) | Sem disparo quando a fronteira avança | G2; limite 10 | 100 chamadas, contabilizando 1 a cada chamada | Todas verdadeiras | Idem |
| E09-T06 | Integração sem GPU (exige OpenCV) | Limite derivado | G2 | `getMaximumUncountedPacketCount()` | `ceil(fps * 40) + 32`; para 60000/1001 fps, 2430 | Idem |
| E09-T07 | Integração com GPU | Sem falso positivo em arquivo | G1 | R3, R4 | `reason=file_end`; trechos do baseline | R3, R4 |
| E09-T08 | Manual com RTSP e GPU | Sem falso positivo em RTSP | — | R7 por 60 s antes do Ctrl+C | Sem `decoder_stalled` | Log |

O travamento real do decoder não tem teste com GPU: depois de E08 não conheço entrada que o provoque de forma controlada. Vai para UNV.

**Regressão obrigatória:** R1 a R8.

**Critério de pronto:** checks PASS. **Reverter:** `git revert <commit>`.

```text
TASK
ID: E09
BASE: feat/fila-unica@<commit da etapa anterior>
SPEC: motion_mog2/specs/plano_mudancas_v2.md, secao 4, etapa E09
SCOPE: motion_mog2/include/buffers/EncodedPacketQueue.hpp; motion_mog2/src/buffers/EncodedPacketQueue.cpp; motion_mog2/tests/encoded_packet_queue_tests.cpp; motion_mog2/include/input/PacketReadEnd.hpp; motion_mog2/include/input/FfmpegEncodedVideoSource.hpp; motion_mog2/src/input/FfmpegEncodedVideoSource.cpp; motion_mog2/include/app/CaptureEnd.hpp; motion_mog2/src/app/CaptureEnd.cpp; motion_mog2/tests/capture_end_tests.cpp; motion_mog2/tests/ffmpeg_encoded_video_packets_tests.cpp
BUILD: cmake -S motion_mog2 -B motion_mog2/build -DOpenCV_DIR=/home/davi/Downloads/tcc/opencv/install-cuda125/lib/cmake/opencv4 -DBUILD_TESTING=ON && cmake --build motion_mog2/build
PRE: git status limpo; gerar G2
CHECK C1: BUILD termina sem erro
CHECK C2: encoded_packet_queue_tests e capture_end_tests terminam com codigo 0, com PASS para E09-T01 a E09-T03
CHECK C3: ffmpeg_encoded_video_packets_tests /tmp/tcc_g2.mp4 termina com codigo 0 (E09-T04 a E09-T06)
CHECK C4: R3 e R4 com os trechos do baseline e reason=file_end
CHECK C5: R2, R5
CHECK C6: R7 por 60 s sem decoder_stalled; R8
OUT O1: saida dos tres executaveis de teste
OUT O2: linha CAPTURE_END de C4 e de C6
COMMIT: feat: encerra a leitura quando a regiao a frente da fronteira passa do teto
PUSH: origin feat/fila-unica
```

---

### E10 — Ferramenta de medição de pontos de corte

- **Itens atendidos:** 2.4 (medir). **Dependências:** E08 (reaproveita `H264NalUnits`).

**Objetivo e por que esta solução.** Não há câmera IP. A ferramenta serve a dois fins: medir agora as duas fontes disponíveis (arquivo publicado pelo MediaMTX com `-c copy` e webcam codificada pelo `run_rtsp_recording.sh --webcam`) e ficar pronta para ser apontada a uma câmera real quando houver, que é a condição para reabrir D4. O `trace_headers` do FFmpeg mostra os NAL, mas não consegui associar de forma confiável cada bloco de NAL à linha do pacote correspondente na saída dele; uma ferramenta pequena que lê com as mesmas opções do projeto e conta por pacote é mais direta. Ela não entra no executável do gravador.

**Escopo de arquivos**

- Criar: `motion_mog2/tests/h264_cut_point_survey.cpp`.
- Alterar: `motion_mog2/include/ffmpeg/H264NalUnits.hpp`, `motion_mog2/src/ffmpeg/H264NalUnits.cpp`, `motion_mog2/tests/h264_nal_units_tests.cpp`, `motion_mog2/CMakeLists.txt`.

**Mudança**

```cpp
bool checkIfLengthPrefixedContainsNalUnitType(const unsigned char* data, std::size_t size,
                                             int nal_length_size, int nal_unit_type);
bool checkIfH264PacketContainsNalUnitType(const AVPacket& encoded_packet,
                                          const AVCodecParameters& codec_parameters, int nal_unit_type);
```

A segunda decide o formato pela extradata: se começa com código de início Annex B (ou está vazia), usa a busca Annex B; se o primeiro byte é 1 e há pelo menos 7 bytes, é AVCC e o tamanho do prefixo é `(extradata[4] & 0x03) + 1` (mesma leitura de `ff: libavcodec/h264_mp4toannexb_bsf.c:80`). A primeira percorre os NAL pelo prefixo de tamanho e para, devolvendo falso, se um tamanho passar do fim do buffer.

Ferramenta: `h264_cut_point_survey <fonte> [rtsp_transporte=tcp] [segundos=60]`. Aceita arquivo local ou qualquer URL `rtsp://`, inclusive com usuário e senha na URL, que é como uma câmera real será informada. Abre a entrada como `FfmpegEncodedVideoSource` (opção `rtsp_transport` só para `rtsp://`; callback de interrupção com prazo, para não ficar presa em fonte parada). Lê até o prazo ou o fim. Com `--help` ou sem argumentos, imprime o uso e o significado de cada campo. Saída:

```text
CUT_POINT_SURVEY source=<fonte> seconds=<t> packets=<n> key_packets=<k> key_with_idr=<a> key_without_idr=<b> idr_without_key=<c> packets_with_sps=<d> max_key_interval_packets=<m> max_idr_interval_packets=<m2>
KEY_WITHOUT_IDR index=<i> size=<bytes>
```

A segunda linha se repete para os primeiros 20 casos. Se a URL tiver credenciais, `source=` imprime a URL sem a parte de usuário e senha. Código de saída 0 se leu ao menos um pacote; 1 em erro de abertura ou de uso.

No CMake: alvo `h264_cut_point_survey` dentro de `BUILD_TESTING`, sem OpenCV, sem `add_test`.

**Invariantes e riscos.** Nenhum código do gravador muda. Risco: erro de análise em AVCC com prefixo de 1, 2 ou 3 bytes; T01 cobre.

**Casos de teste**

| ID | Tipo | Objetivo | Preparo | Passos | Resultado esperado | Critério |
|---|---|---|---|---|---|---|
| E10-T01 | Unitário sem GPU | AVCC com prefixos de 1, 2, 3 e 4 bytes | Buffers sintéticos | Procurar tipos 5 e 7 | Correto em todos | Asserções |
| E10-T02 | Unitário sem GPU | Tamanho além do buffer | Prefixo declarando mais bytes do que há | Procurar | Falso; sem leitura fora do buffer | Idem |
| E10-T03 | Unitário sem GPU | Escolha do formato | Extradata Annex B; extradata avcC; extradata vazia | `checkIfH264PacketContainsNalUnitType` | Usa o formato certo em cada caso | Idem |
| E10-T04 | Integração sem GPU | Fluxo comum | G2 | `$BUILD/h264_cut_point_survey /tmp/tcc_g2.mp4` | `packets=1199 key_packets=20 key_without_idr=0` | Linha de saída. `key_packets=20` foi medido; `key_without_idr=0` é [Provável] |
| E10-T05 | Integração sem GPU | Intra-refresh | G5 | `$BUILD/h264_cut_point_survey /tmp/tcc_g5_intra_refresh.ts` | `packets=150 key_packets=6 key_with_idr=1 key_without_idr=5` | Linha de saída. Valores medidos nesta análise por outro método |
| E10-T06 | Integração sem GPU | Vídeo do baseline, lido como arquivo | G1 | `$BUILD/h264_cut_point_survey "$VIDEO"` | `packets=1181`; `max_key_interval_packets=60` | Linha de saída |
| E10-T07 | Manual com RTSP | **Fonte disponível 1:** arquivo via MediaMTX | RIG com PUB-COPY e G1 | `$BUILD/h264_cut_point_survey "$RTSP_URL" tcp 30` | Esperado [Provável]: `key_without_idr=0`; `max_key_interval_packets=60` | Colar a saída. `key_without_idr > 0` não reprova a etapa; vai para o Davi |
| E10-T08 | Manual com RTSP | **Fonte disponível 2:** webcam | Em outro terminal, `RTSP_TRANSPORT=tcp SHOW_PREVIEW=0 bash run_rtsp_recording.sh --webcam /tmp/tcc_e10_webcam 5`; a ferramenta entra como segundo leitor. Sem webcam, usar o RIG com PUB-WEBCAM-EQ | `$BUILD/h264_cut_point_survey "$RTSP_URL" tcp 30` | Esperado: `key_without_idr=0`, `packets_with_sps=0`, `max_key_interval_packets=60`. Medido nesta análise com PUB-WEBCAM-EQ por outro método: 269 pacotes, 5 keyframes, 5 com IDR, 0 com SPS | Colar a saída e informar qual das duas formas foi usada |
| E10-T09 | Integração sem GPU | Uso e credenciais | — | Rodar sem argumentos; rodar com `rtsp://usuario:senha@127.0.0.1:1/x` | Sem argumentos: texto de uso, código 1. Com credenciais: erro de abertura, código 1, e a mensagem de erro da própria ferramenta mostra a URL sem usuário e senha | Saída e código. Se o FFmpeg imprimir a URL completa no log dele, registrar em DEV |

Câmera futura: o comando é o de T07 com a URL da câmera e 120 s. O resultado entra na tabela "Critério para reabrir a decisão" de D4. Não faz parte dos checks desta etapa.

**Regressão obrigatória:** R1, R2.

**Critério de pronto:** checks PASS e saídas de T06 a T08 entregues ao Davi. **Reverter:** `git revert <commit>`.

```text
TASK
ID: E10
BASE: feat/fila-unica@<commit da etapa anterior>
SPEC: motion_mog2/specs/plano_mudancas_v2.md (versao 2), secao 4, etapa E10
SCOPE: motion_mog2/tests/h264_cut_point_survey.cpp (criar); motion_mog2/include/ffmpeg/H264NalUnits.hpp; motion_mog2/src/ffmpeg/H264NalUnits.cpp; motion_mog2/tests/h264_nal_units_tests.cpp; motion_mog2/CMakeLists.txt
BUILD: cmake -S motion_mog2 -B motion_mog2/build -DOpenCV_DIR=/home/davi/Downloads/tcc/opencv/install-cuda125/lib/cmake/opencv4 -DBUILD_TESTING=ON && cmake --build motion_mog2/build
PRE: git status limpo; gerar G2 e G5
CHECK C1: BUILD termina sem erro
CHECK C2: h264_nal_units_tests termina com codigo 0, com PASS para E10-T01 a E10-T03 e para os casos de E08
CHECK C3: E10-T04 -> packets=1199 key_packets=20 key_without_idr=0
CHECK C4: E10-T05 -> packets=150 key_packets=6 key_with_idr=1 key_without_idr=5
CHECK C5: E10-T06 -> packets=1181 e max_key_interval_packets=60
CHECK C6: E10-T07 -> a ferramenta termina com codigo 0 e imprime a linha CUT_POINT_SURVEY (o valor de key_without_idr e informativo)
CHECK C7: E10-T08 -> idem, com a webcam ou com PUB-WEBCAM-EQ
CHECK C8: E10-T09 -> uso com codigo 1; a mensagem de erro da ferramenta nao contem a senha
CHECK C9: R2
OUT O1: saida de h264_nal_units_tests
OUT O2: linhas CUT_POINT_SURVEY e KEY_WITHOUT_IDR de C3 a C7, indicando em C7 se foi webcam real ou PUB-WEBCAM-EQ
OUT O3: saida de C8
COMMIT: test: adiciona ferramenta de medicao de keyframes sem IDR
PUSH: origin feat/fila-unica
```

---

### E11 — Registrar quando um MP4 começa em keyframe sem IDR

- **Itens atendidos:** 2.4 (observabilidade; o corte não muda). **Dependências:** E10 (função `checkIfH264PacketContainsNalUnitType`), E04 (sink). D4 revista e decidida.

**Objetivo e por que esta solução.** Na versão 1 esta etapa impunha IDR como ponto de corte. Sem câmera alvo, essa regra poderia impedir qualquer gravação numa câmera sem IDR periódico, e isso só seria descoberto em campo. A decisão (D4) é manter o corte pela flag `KEY` e tornar o problema visível: uma linha de log por arquivo que começa em keyframe sem NAL 5 e um contador no fim da execução. Quem vir essa linha sabe que o início daquele arquivo pode ter quadros perdidos e que é hora de medir a fonte com a ferramenta de E10.

A verificação fica no `Mp4EventPacketSink`, e não na fonte nem na fila: é ele que sabe qual é o primeiro pacote de cada arquivo, tem o caminho do arquivo e tem os parâmetros do codec necessários para escolher entre Annex B e AVCC. A fila, a fonte e a máquina de estados não mudam.

**Escopo de arquivos**

- Alterar: `motion_mog2/include/recording/Mp4EventPacketSink.hpp`, `motion_mog2/src/recording/Mp4EventPacketSink.cpp`, `motion_mog2/tests/mp4_event_packet_sink_tests.cpp`, `motion_mog2/src/main.cpp`, `motion_mog2/CMakeLists.txt` (o alvo de teste do sink passa a compilar `H264NalUnits.cpp`).

**Mudança**

- Em `Mp4EventPacketSink::writePackets`, no primeiro lote não vazio de cada gravação e antes de escrever: se `checkIfH264PacketContainsNalUnitType(primeiro_pacote, *video_stream_parameters_->codec_parameters, 5)` for falso, incrementar `recordings_started_without_idr_count_` e imprimir em `std::cout`:

  `CUT_POINT_WITHOUT_IDR path=<caminho> first_packet_pts=<pts> first_packet_dts=<dts>`

  A linha sai uma vez por arquivo, por construção. A gravação segue normalmente.
- Novo método na classe concreta: `std::size_t getRecordingsStartedWithoutIdrCount() const`. A interface `EventPacketSink` não muda.
- No `main`, a linha final passa a ser `CAPTURE_COMPLETE decoded_reads=<n> saved_files=<m> recordings_without_idr=<k>`. O campo novo vai no fim e aparece sempre, inclusive com valor 0.

Dois desvios do exemplo dado pelo Davi (`CUT_POINT_WITHOUT_IDR file=... packet_index=...`), com o motivo:

- `path=` no lugar de `file=`: é o campo que `MP4_STARTED` e `MP4_SAVED` já usam, e a normalização de log de R9 depende desse nome.
- `first_packet_pts` e `first_packet_dts` no lugar de `packet_index`: o projeto não numera pacotes, e criar esse contador exigiria estado novo na fila ou na fonte só para um log. O `dts` identifica o pacote na fonte (`ffprobe -show_packets`).

Se o Davi preferir os nomes do exemplo, a troca é só de texto; o índice de pacote exigiria uma etapa própria.

**Invariantes e riscos.** I1 a I6 sem alteração: nenhum pacote, flag ou decisão de corte muda. Riscos: o custo de percorrer um pacote por arquivo é desprezível; a linha `CAPTURE_COMPLETE` muda de formato, o que afeta quem a interpretar por posição.

**Casos de teste**

| ID | Tipo | Objetivo | Preparo | Passos | Resultado esperado | Critério |
|---|---|---|---|---|---|---|
| E11-T01 | Unitário sem GPU | Início em IDR não gera log | Sink; primeiro pacote com NAL 5, no formato indicado pela extradata do teste | Ciclo completo | Nenhuma linha `CUT_POINT_WITHOUT_IDR`; contador 0 | Asserções |
| E11-T02 | Unitário sem GPU | Início em keyframe sem IDR | Primeiro pacote com flag `KEY` e só NAL 1 | Ciclo completo | Uma linha, com o caminho e os carimbos do primeiro pacote; contador 1; arquivo gravado normalmente | Idem |
| E11-T03 | Unitário sem GPU | Uma linha por arquivo | Como T02, com três lotes | Escrever os três | Uma linha só | Idem |
| E11-T04 | Unitário sem GPU | Contagem por execução | Duas gravações sem IDR e uma com | Três ciclos | Duas linhas; contador 2 | Idem |
| E11-T05 | Unitário sem GPU | Os dois formatos | T01 e T02 com extradata Annex B e com extradata avcC | Repetir | Mesmos resultados nos dois formatos | Idem |
| E11-T06 | Unitário sem GPU | Lote inicial vazio não conta | Gravação aberta; `writePackets({})` e depois um lote sem IDR | Escrever | A verificação ocorre no primeiro lote **não vazio**; uma linha | Idem |
| E11-T07 | Integração com GPU | Baseline sem ocorrência | G1 | R3, R4 | Nenhuma linha `CUT_POINT_WITHOUT_IDR`; `CAPTURE_COMPLETE ... recordings_without_idr=0`; trechos do baseline | R3, R4; em R9 a única diferença aceita é o fim da linha `CAPTURE_COMPLETE` |
| E11-T08 | Integração com GPU | Ocorrência visível | G6 | `"$BIN" /tmp/tcc_g6_intra_refresh.ts /tmp/tcc_e11 5 1.0 2 3 0; echo exit=$?`; `tcc_check_mp4` em cada MP4 | Esperado [Provável]: pelo menos uma linha `CUT_POINT_WITHOUT_IDR`; `recordings_without_idr` igual ao número dessas linhas; cada arquivo citado mostra `nb_read_frames` menor que `nb_read_packets`; os não citados estão sãos | Critério firme: o contador é igual ao número de linhas, e todo MP4 com `nb_read_frames` menor que `nb_read_packets` foi citado. Se não houver nenhuma ocorrência, UNV |
| E11-T09 | Manual com RTSP e GPU | RTSP sem ocorrência | — | R7 | Nenhuma linha; contador 0 | Log |

Sobre T08: G6 é G1 recodificado em intra-refresh; só o primeiro quadro é IDR, e os quadros de movimento podem diferir dos de G1. O teste não exige um número de ocorrências; exige que o log e a perda de quadros coincidam arquivo a arquivo.

**Regressão obrigatória:** R1 a R8.

**Critério de pronto:** checks PASS. **Reverter:** `git revert <commit>`.

```text
TASK
ID: E11
BASE: feat/fila-unica@<commit da etapa anterior>
SPEC: motion_mog2/specs/plano_mudancas_v2.md (versao 2), secao 4, etapa E11. O corte pela flag KEY NAO muda; esta etapa so acrescenta log e contador
SCOPE: motion_mog2/include/recording/Mp4EventPacketSink.hpp; motion_mog2/src/recording/Mp4EventPacketSink.cpp; motion_mog2/tests/mp4_event_packet_sink_tests.cpp; motion_mog2/src/main.cpp; motion_mog2/CMakeLists.txt
BUILD: cmake -S motion_mog2 -B motion_mog2/build -DOpenCV_DIR=/home/davi/Downloads/tcc/opencv/install-cuda125/lib/cmake/opencv4 -DBUILD_TESTING=ON && cmake --build motion_mog2/build
PRE: git status limpo; gerar G6; rodar R3 com o binario da base desta etapa e salvar o log em /tmp/tcc_pre_r3.log; com o binario da base, rodar o comando de E11-T08 e colar as linhas MP4CHECK
CHECK C1: BUILD termina sem erro
CHECK C2: mp4_event_packet_sink_tests termina com codigo 0, com PASS para E11-T01 a E11-T06
CHECK C3: R3 e R4 com os trechos do baseline, sem CUT_POINT_WITHOUT_IDR e com recordings_without_idr=0
CHECK C4: R9 -> a unica diferenca e o campo recordings_without_idr na linha CAPTURE_COMPLETE
CHECK C5: E11-T08 -> contador igual ao numero de linhas CUT_POINT_WITHOUT_IDR; todo MP4 com nb_read_frames < nb_read_packets foi citado (UNV se nao houver ocorrencia)
CHECK C6: R2, R5
CHECK C7: R7 sem CUT_POINT_WITHOUT_IDR; R8
OUT O1: saida de mp4_event_packet_sink_tests
OUT O2: linhas CUT_POINT_WITHOUT_IDR, CAPTURE_COMPLETE e MP4CHECK de C5, e as MP4CHECK do PRE
OUT O3: diff de C4
COMMIT: feat: registra quando um MP4 comeca em keyframe sem IDR
PUSH: origin feat/fila-unica
```

---

### E12 — Log quando o teto de idle descarta

- **Itens atendidos:** 2.6. **Dependências:** E03.

**Objetivo e por que esta solução.** Quando a região contabilizada passa do teto sem um segundo keyframe, `trimCountedPackets` descarta o GOP incompleto (`src/buffers/EncodedPacketQueue.cpp:81-87`) e o chamador ignora o retorno. O descarte pelo teto precisa aparecer no log. Comparar o tamanho antes e depois no chamador não distingue o corte normal por GOP do corte pelo teto; por isso o retorno passa a separar os dois.

**Escopo de arquivos**

- Alterar: `motion_mog2/include/buffers/EncodedPacketQueue.hpp`, `motion_mog2/src/buffers/EncodedPacketQueue.cpp`, `motion_mog2/tests/encoded_packet_queue_tests.cpp`, `motion_mog2/src/recording/EventRecorder.cpp`, `motion_mog2/tests/event_recorder_tests.cpp`.

**Mudança**

```cpp
struct TrimResult {
    std::size_t discarded_packet_count = 0;           // total
    std::size_t ceiling_discarded_packet_count = 0;   // parte descartada pelo teto
};
TrimResult trimCountedPackets(std::size_t minimum_kept_packet_count,
                              std::size_t maximum_counted_packet_count);
```

O comportamento do trim não muda; só a contagem do segundo laço é separada. Em `EventRecorder`, no ramo `idle` sem início de movimento, quando `ceiling_discarded_packet_count > 0`:

`IDLE_CEILING_DISCARDED packets=<n> ceiling_packets=<teto>`

Os sete pontos de `tests/encoded_packet_queue_tests.cpp` que usam o retorno (linhas 141, 146, 157, 169, 179, 285, 327) passam a ler `discarded_packet_count`.

**Invariantes e riscos.** I3. Risco: mudar por engano a condição de algum dos dois laços.

**Casos de teste**

| ID | Tipo | Objetivo | Preparo | Passos | Resultado esperado | Critério |
|---|---|---|---|---|---|---|
| E12-T01 | Unitário sem GPU | Corte normal não conta como teto | Cenário de "trim corta GOP inteiro" já existente | `trimCountedPackets(6, 100)` | `discarded_packet_count == 4`; teto 0 | Asserções |
| E12-T02 | Unitário sem GPU | Teto com keyframe | Cenário "teto descarta GOP mesmo abaixo do mínimo" | `trimCountedPackets(12, 15)` | Total 10; teto 10 | Idem |
| E12-T03 | Unitário sem GPU | Teto sem keyframe | Cenário "teto sem keyframe esvazia" | `trimCountedPackets(5, 10)` | Total 21; teto 21 | Idem |
| E12-T04 | Unitário sem GPU | Recorder registra o descarte | GOP de 750 pacotes, lookback 158, teto 658, 1450 pacotes, `started` no último | Processar | Exatamente 2 linhas `IDLE_CEILING_DISCARDED packets=659 ceiling_packets=658`; depois `EVENT_WAITING_KEYFRAME` | Log. Medido nesta análise com a fila real: 1449 descartes em idle e pré-evento vazio; a divisão em 2 x 659 é derivada da conta |
| E12-T05 | Unitário sem GPU | Sem log em operação normal | Baseline sintético | E03-T01 | Nenhuma linha `IDLE_CEILING_DISCARDED` | Log |
| E12-T06 | Integração com GPU | Baseline inalterado | G1 | R3, R4 | Trechos do baseline; nenhuma linha nova | R3, R4, R9 |

**Regressão obrigatória:** R1 a R6, R9.

**Critério de pronto:** checks PASS. **Reverter:** `git revert <commit>`.

```text
TASK
ID: E12
BASE: feat/fila-unica@<commit da etapa anterior>
SPEC: motion_mog2/specs/plano_mudancas_v2.md, secao 4, etapa E12
SCOPE: motion_mog2/include/buffers/EncodedPacketQueue.hpp; motion_mog2/src/buffers/EncodedPacketQueue.cpp; motion_mog2/tests/encoded_packet_queue_tests.cpp; motion_mog2/src/recording/EventRecorder.cpp; motion_mog2/tests/event_recorder_tests.cpp
BUILD: cmake -S motion_mog2 -B motion_mog2/build -DOpenCV_DIR=/home/davi/Downloads/tcc/opencv/install-cuda125/lib/cmake/opencv4 -DBUILD_TESTING=ON && cmake --build motion_mog2/build
PRE: git status limpo; rodar R3 com o binario da base desta etapa e salvar o log em /tmp/tcc_pre_r3.log
CHECK C1: BUILD termina sem erro
CHECK C2: encoded_packet_queue_tests e event_recorder_tests terminam com codigo 0, com PASS para E12-T01 a E12-T05
CHECK C3: R2
CHECK C4: R3 e R4 com os trechos do baseline
CHECK C5: R5
CHECK C6: R9 -> diff vazio
OUT O1: saida dos dois executaveis de teste
OUT O2: linhas IDLE_CEILING_DISCARDED capturadas em E12-T04
COMMIT: feat: registra quando o teto de idle descarta um GOP incompleto
PUSH: origin feat/fila-unica
```

---

### E13 — Rejeitar limiar não finito

- **Itens atendidos:** 2.8. **Dependências:** nenhuma técnica; fica aqui pela prioridade.

**Objetivo e por que esta solução.** `parseMotionThreshold` aceita `nan` porque as duas comparações de intervalo são falsas para NaN (`src/config/EventRecordingConfig.cpp:41`), e o construtor do detector repete a mesma comparação (`src/detectors/Mog2MotionDetector.cpp:30`). A correção é a mesma checagem de finitude já usada para os tempos (`src/config/EventRecordingConfig.cpp:58-59`), nos dois lugares.

**Escopo de arquivos**

- Alterar: `motion_mog2/src/config/EventRecordingConfig.cpp`, `motion_mog2/src/detectors/Mog2MotionDetector.cpp`, `motion_mog2/tests/event_recording_config_tests.cpp`.

**Mudança.** Em `parseMotionThreshold`, rejeitar com `std::invalid_argument` quando `!std::isfinite(valor)`, antes da checagem de intervalo; a mensagem é a de intervalo já existente. No construtor do detector, a mesma condição com `std::runtime_error` (incluir `<cmath>`). Nenhum log novo.

**Invariantes e riscos.** Nenhum risco para valores finitos.

**Casos de teste**

| ID | Tipo | Objetivo | Preparo | Passos | Resultado esperado | Critério |
|---|---|---|---|---|---|---|
| E13-T01 | Unitário sem GPU | `nan` | Argumentos com limiar `nan` | `parseEventRecordingArguments` | `std::invalid_argument` | Asserção. Na base este caso é aceito (reproduzido nesta análise) |
| E13-T02 | Unitário sem GPU | Variações | `NaN`, `inf`, `-inf`, `1e999` | Idem | `std::invalid_argument` em todos | Idem |
| E13-T03 | Unitário sem GPU | Limites válidos | `100`, `0.0001` | Idem | Aceitos | Idem |
| E13-T04 | Unitário sem GPU | Limites inválidos já cobertos | `0`, `100.0001`, `-1` | Idem | Rejeitados | Idem |
| E13-T05 | Manual | Linha de comando | — | `"$BIN" "$VIDEO" /tmp/x 5 nan; echo $?` | `[fatal]` com a mensagem de intervalo; código 2 | Código e mensagem |

**Regressão obrigatória:** R1 a R4.

**Critério de pronto:** checks PASS; `event_recording_config_tests` com pelo menos 11 linhas PASS. **Reverter:** `git revert <commit>`.

```text
TASK
ID: E13
BASE: feat/fila-unica@<commit da etapa anterior>
SPEC: motion_mog2/specs/plano_mudancas_v2.md, secao 4, etapa E13
SCOPE: motion_mog2/src/config/EventRecordingConfig.cpp; motion_mog2/src/detectors/Mog2MotionDetector.cpp; motion_mog2/tests/event_recording_config_tests.cpp
BUILD: cmake -S motion_mog2 -B motion_mog2/build -DOpenCV_DIR=/home/davi/Downloads/tcc/opencv/install-cuda125/lib/cmake/opencv4 -DBUILD_TESTING=ON && cmake --build motion_mog2/build
PRE: git status limpo; com o binario da base, rodar E13-T05 e confirmar que o programa NAO rejeita o limiar (processa o video, termina com codigo 0 e nao grava MP4)
CHECK C1: BUILD termina sem erro
CHECK C2: event_recording_config_tests termina com codigo 0, com PASS para E13-T01 a E13-T04 e pelo menos 11 linhas PASS
CHECK C3: E13-T05 -> codigo 2 e mensagem de intervalo
CHECK C4: R2
CHECK C5: R3 e R4 com os trechos do baseline
OUT O1: saida de event_recording_config_tests
OUT O2: saida de C3
COMMIT: fix: rejeita limiar de movimento nao finito
PUSH: origin feat/fila-unica
```

---

### E14 — Remover API legada e código morto, alinhar documentos

- **Itens atendidos:** 2.11 e código morto. **Dependências:** E07 e E09 (as duas editam `tests/ffmpeg_encoded_video_packets_tests.cpp`; esta etapa vem depois para não conflitar).

**Objetivo e por que esta solução.** `dequeueEncodedPackets` e `dequeueEncodedPacketsWithStreamParameters` são um segundo ponto que avança a fronteira, usado só por teste (`src/input/FfmpegEncodedVideoSource.cpp:308-324`). Dois arquivos não entram em nenhum alvo, um cabeçalho não é incluído por ninguém, um teste útil está sem alvo, e dois documentos descrevem código que não existe mais.

**Escopo de arquivos**

- Alterar: `motion_mog2/include/input/FfmpegEncodedVideoSource.hpp`, `motion_mog2/src/input/FfmpegEncodedVideoSource.cpp`, `motion_mog2/tests/ffmpeg_encoded_video_packets_tests.cpp`, `motion_mog2/CMakeLists.txt`, `motion_mog2/PRE_EVENT.md`, `motion_mog2/NOTES.md`.
- Remover: `motion_mog2/tests/ffmpeg_raw_video_source_tests.cpp`, `motion_mog2/include/ffmpeg/EncodedPacket.hpp`.

**Mudança**

- Remover as duas funções da fonte (`.hpp:40-44`, `.cpp:308-324`) e o `#include "ffmpeg/EncodedVideoPackets.hpp"` do cabeçalho da fonte, se nada mais nele usar o tipo. `EncodedVideoPackets` continua existindo: `AvMp4OutputContext` o usa.
- Em `tests/ffmpeg_encoded_video_packets_tests.cpp`, trocar cada uso das funções removidas (linhas 213-230 e 337 na base original) por `getEncodedPacketQueue().advanceCountedPackets(n)` seguido de `takeCountedPackets()`, e obter os parâmetros por `getVideoStreamParameters()`. As asserções continuam as mesmas: retirar zero devolve vazio; pedir mais do que há lança `std::runtime_error`; a ordem e os bytes são preservados.
- No CMake, criar o alvo `pre_event_tests` (`tests/pre_event_tests.cpp` e `src/config/PreEventConfig.cpp`) com `add_test`. Compilei e executei esse teste isoladamente nesta análise: passa.
- `PRE_EVENT.md`: substituir as referências a `EncodedVideoBuffer` pela descrição atual (o `main` converte segundos em pacotes; a fila única guarda o histórico; `lookback` = pré-evento + margem). Conteúdo a escrever a partir da seção 1 deste plano e do código; não inventar comportamento.
- `NOTES.md`: a nota diz `allowFrameDrop = true` (linhas 8-12) e cita `EncodedVideoBuffer`; o código usa `false` (`src/input/VideoStreamReader.cpp:17`). Atualizar o trecho para o estado atual, mantendo a discussão em aberto como histórico.

Fora do escopo desta etapa, mas a avisar no relatório: `AGENTS.md:224`, na raiz, diz que `pre_event_tests.cpp` não tem alvo; passa a estar desatualizado.

**Invariantes e riscos.** I1 a I3 (a remoção reduz os pontos que avançam a fronteira). Risco: algum alvo ainda referenciar os arquivos removidos.

**Casos de teste**

| ID | Tipo | Objetivo | Preparo | Passos | Resultado esperado | Critério |
|---|---|---|---|---|---|---|
| E14-T01 | Estático | Nenhuma referência restante | — | `grep -rn "dequeueEncodedPackets\|EncodedPacket.hpp\|ffmpeg_raw_video_source_tests\|EncodedVideoBuffer" motion_mog2 --include='*.cpp' --include='*.hpp' --include='*.txt' --include='*.md' \| grep -v '/specs/'` | Sem saída | Saída vazia |
| E14-T02 | Unitário sem GPU | `pre_event_tests` no CTest | — | `ctest --test-dir $BUILD -R pre_event_tests --output-on-failure` | 1 teste, passa | Saída do ctest |
| E14-T03 | Integração sem GPU (exige OpenCV) | Teste da fonte adaptado | G2 | `$BUILD/ffmpeg_encoded_video_packets_tests /tmp/tcc_g2.mp4` | Código 0 | Código |
| E14-T04 | Integração com GPU | Aplicação inalterada | G1 | R3, R4 | Trechos e log iguais | R3, R4, R9 |
| E14-T05 | Revisão | Documentos coerentes com o código | — | Davi lê `PRE_EVENT.md` e `NOTES.md` | Nenhuma afirmação contradiz o código | Confirmação do Davi |

**Regressão obrigatória:** R1 a R6, R9.

**Critério de pronto:** checks PASS. **Reverter:** `git revert <commit>`.

```text
TASK
ID: E14
BASE: feat/fila-unica@<commit da etapa anterior>
SPEC: motion_mog2/specs/plano_mudancas_v2.md, secao 4, etapa E14
SCOPE: motion_mog2/include/input/FfmpegEncodedVideoSource.hpp; motion_mog2/src/input/FfmpegEncodedVideoSource.cpp; motion_mog2/tests/ffmpeg_encoded_video_packets_tests.cpp; motion_mog2/CMakeLists.txt; motion_mog2/PRE_EVENT.md; motion_mog2/NOTES.md; motion_mog2/tests/ffmpeg_raw_video_source_tests.cpp (remover); motion_mog2/include/ffmpeg/EncodedPacket.hpp (remover)
BUILD: cmake -S motion_mog2 -B motion_mog2/build -DOpenCV_DIR=/home/davi/Downloads/tcc/opencv/install-cuda125/lib/cmake/opencv4 -DBUILD_TESTING=ON && cmake --build motion_mog2/build
PRE: git status limpo; gerar G2; rodar ffmpeg_encoded_video_packets_tests /tmp/tcc_g2.mp4 com o binario da base e confirmar codigo 0 (senao BLOCKED); rodar R3 e salvar o log em /tmp/tcc_pre_r3.log
CHECK C1: BUILD termina sem erro
CHECK C2: E14-T01 -> saida vazia
CHECK C3: E14-T02 -> pre_event_tests passa no ctest
CHECK C4: E14-T03 -> codigo 0
CHECK C5: R2
CHECK C6: R3 e R4 com os trechos do baseline; R5
CHECK C7: R9 -> diff vazio
OUT O1: saida de C2, C3 e C4
OUT O2: git diff --stat
COMMIT: chore: remove API legada da fonte e codigo morto, religa pre_event_tests e alinha documentos
PUSH: origin feat/fila-unica
```

## 5. Matriz de rastreabilidade

| Item da avaliação | Decisão | Etapa | Casos de teste |
|---|---|---|---|
| 2.1 Exceção deixa o MP4 ilegível | D1 (parte A) | E04 | E04-T01 a T06 |
| 2.1 Morte abrupta do processo | D1 (parte B) | E05 | E05-T01 a T15 (players: T09 a T15) |
| 2.2 Carimbo inválido encerra o processo | D2 | E06 | E06-T01 a T13 |
| 2.3 Erro tratado como fim normal | D3 | E07 | E07-T01 a T14 |
| 2.4 `KEY` não garante IDR (medir as fontes disponíveis) | D4 | E10 | E10-T01 a T09 (fontes: T07 e T08) |
| 2.4 `KEY` não garante IDR (observabilidade) | D4 | E11 | E11-T01 a T09 |
| 2.4 `KEY` não garante IDR (regra rígida) | D4 | Fora do plano; pendência condicionada a câmera real | — |
| 2.5 SPS/PPS só no SDP | — | E08 | E08-T01 a T14 |
| 2.5 Região à frente sem teto | D3 (código 6) | E09 | E09-T01 a T08 |
| 2.6 Teto descarta sem aviso | — | E12 | E12-T01 a T06 |
| 2.8 Limiar `nan` | — | E13 | E13-T01 a T05 |
| 2.10 Histerese sem teste | D5 | E01 | E01-T01 a T09 |
| 2.10 Escrita do MP4 dentro do `main` | D5 | E02 | E02-T01 a T09 |
| 2.10 Máquina de estados sem teste | D5 | E03 | E03-T01 a T12 |
| 2.11 e código morto | — | E14 | E14-T01 a T05 |
| 2.7, 2.9 | — | Fora do plano | — |

## 6. Pendências fora do plano

| Pendência | Situação |
|---|---|
| 2.7 Timeout de I/O | Declarado fora de escopo em `specs/00-LEIA-ME.md:32`. Com MediaMTX o travamento vira fim de stream em cerca de 10 s (medido); com câmera real não há limite. Depois de E07, a expiração pode reutilizar o motivo `read_error`. |
| 2.9 TEARDOWN após Ctrl+C | Medido: em UDP a sessão fica no servidor até expirar; em TCP cai com a conexão. Sem efeito no padrão `tcp`. |
| Thread de escrita separada | Pendência conhecida. E05 aumenta o número de chamadas de escrita na thread do `read()`; se houver atraso medido, esta pendência sobe de prioridade. |
| `max_evento_s` | Pendência conhecida. |
| IDR real como ponto de corte (opção b de D4) | Condicionada à medição numa câmera real com `h264_cut_point_survey` (E10) ou ao aparecimento de `CUT_POINT_WITHOUT_IDR` em operação (E11). Até lá o item 2.4 continua possível: um MP4 pode começar em keyframe sem IDR e perder os primeiros quadros. Ao adotar a regra, lembrar que em fonte sem IDR periódico ela impede o fechamento e a abertura de arquivos, o que torna `max_evento_s` pré-requisito. |
| Vídeo entrelaçado | Pendência conhecida. Mecanismo provável descrito na avaliação (N repetido no segundo campo). |
| Teto de idle configurável | E12 só registra o descarte. Tornar os 20 s configuráveis ou derivados do GOP medido fica para depois de E10 mostrar o GOP real da fonte. |
| `AGENTS.md:224` | Fora de `motion_mog2/`. Fica desatualizado depois de E14; ajuste manual do Davi. |
| Reconexão automática | Não pedida. E07 dá ao supervisor do processo o código 3 para decidir reiniciar. |

## 7. Riscos do plano e o que não foi possível verificar

**Não verificado**

- Nada foi executado com GPU. Todo comportamento atribuído ao NVDEC ou ao `cudacodec` vem da leitura do código do OpenCV 4.10.0. Em especial, o travamento do item 2.5 nunca foi observado; o PRE de E08 existe para isso.
- Não tenho o vídeo do baseline. Os valores esperados em G3, G4 e G6 supõem que os quadros de movimento são os informados e que a recodificação não os desloca.
- O MediaMTX usado aqui (1.9.3) é mais antigo que o do Davi. As medições de SPS no fluxo, de `AVERROR_EOF` na queda da fonte e do `readTimeout` precisam ser repetidas (MED-1, E07-T09 a T13).
- FFmpeg 6.1.1 do Ubuntu, não a compilação do Davi.
- Compatibilidade de players. Medi sem tela: GStreamer 1.24.2 e `ffplay` 6.1.1 leem os arquivos fragmentados, com e sem `frag_duration`, completos e interrompidos. Não testei o GNOME Vídeos em si nem Chrome ou Edge (E05-T09 a T15).
- Abertura de `.h264` cru pelo gravador (E06-T12) e consulta do callback de interrupção em leitura de arquivo local (E07-T08).
- O texto da ISO/IEC 14496-15 sobre amostra de sincronismo, usado como apoio em D4.
- Com que frequência câmeras reais mandam SPS/PPS só no SDP, usam intra-refresh ou repetem `dts`.

**Riscos**

- **As três refatorações iniciais.** Movem o código mais delicado antes de corrigir o defeito mais grave. Mitigação: log idêntico (R9), trechos do baseline (R3, R4) e a simulação sem GPU que já reproduz o baseline.
- **Limites dos arquivos dependem do avanço do decoder.** Medido em simulação: com a fronteira 8 pacotes à frente e pós-evento 0, o mesmo cenário gera 4 arquivos em vez de 3, porque o fechamento ocorre quando o keyframe entra na região contabilizada, e não quando o quadro analisado chega nele. Os trechos do baseline valem para aquele vídeo naquela máquina. Não é defeito introduzido pelo plano, mas afeta como interpretar uma diferença em R3 ou R4 depois de trocar driver, GPU ou versão do OpenCV.
- **E05 muda o formato dos arquivos.** Se algum dos três players não ler o resultado, vale a regra de recuo de D1: primeiro sem `frag_duration`, depois sem fragmentação. Com `frag_duration`, fragmentos do meio do arquivo começam fora de keyframe; isso não afetou `ffmpeg`, `ffplay` nem GStreamer nas medições, mas é o ponto mais provável de estranhamento num navegador.
- **E06 perde até um GOP por pacote inválido**, inclusive quadros de movimento (exemplo em E06-T11). É a troca feita em D2.
- **E09 pode disparar na partida** com câmera de GOP acima de 40 s.
- **E11 não corrige o item 2.4.** Ela só o torna visível. Enquanto a regra rígida for pendência, um MP4 pode começar em keyframe sem IDR e perder os primeiros quadros; nas duas fontes disponíveis isso não deve acontecer [Provável], e E10 confirma.
- **E11 muda a linha `CAPTURE_COMPLETE`**, acrescentando um campo no fim.
- **Etapas que editam o mesmo teste.** E07, E09 e E14 alteram `tests/ffmpeg_encoded_video_packets_tests.cpp`; E02, E04, E05 e E11 alteram `tests/mp4_event_packet_sink_tests.cpp`. A ordem do plano evita conflito; mudar a ordem exige rever o SCOPE dessas etapas.
- **Números de linha.** Todas as referências são do commit `bc806b8`. Depois de E01 a E03 o conteúdo de `main.cpp` muda de lugar; as etapas seguintes descrevem o destino por nome de classe e método para não depender das linhas.
