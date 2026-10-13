# Relatório de dificuldades, soluções e pendências do TCC

- **Data de referência:** 12 de outubro de 2026
- **Projeto:** detecção de movimento em transmissões RTSP, com decodificação na GPU e preservação do vídeo codificado
- **Branch examinada:** `agent/tcc-stepwise-codex-test`
- **Commit-base atual:** `cfbca46` — `build: registra configuracao parcial de libavcodec`

## 1. Resumo executivo

O avanço comprovado até esta data é a aquisição de vídeo H.264 por arquivo local e por RTSP local, com decodificação de frames pelo OpenCV/NVDEC e recuperação separada dos pacotes codificados originais. Os testes também verificaram a manutenção dos pacotes e dos parâmetros da stream após destruir a fonte ou o leitor.

A principal falha de execução encontrada na integração foi a inicialização do NVDEC ao receber o bitstream do MP4 sem a adaptação necessária. O problema foi resolvido entregando ao decoder a saída do filtro `h264_mp4toannexb`, enquanto os pacotes originais permanecem separados para armazenamento futuro. Também foi corrigida a substituição de um FPS válido por `NaN` durante a atualização do formato.

O teste RTSP local passou com três frames decodificados, FPS informado de aproximadamente 59,94 e dez pacotes retornados. Isso não comprova ainda a aplicação completa de detecção e gravação: o buffer atual está incompleto, o `main` conserva chamadas de uma API antiga e não existe gravação MP4 por remux implementada no fluxo atual.

A estratégia continua sendo um **MVP incremental**: demonstrar avanços pequenos e verificáveis, implementar o armazenamento por lotes em etapas e complementar a robustez posteriormente. Isso não significa tratar funcionalidades planejadas como prontas.

## 2. Critério de evidência e arquitetura atual

Este relatório distingue:

- **Problema observado:** houve mensagem de erro, valor incorreto ou comportamento registrado em execução.
- **Risco identificado e mitigado:** o código foi estruturado para prevenir um problema; isso não significa que uma falha desse tipo chegou a ocorrer.
- **Implementado e testado em cenário limitado:** existe código e houve execução relacionada, mas não validação geral.
- **Histórico:** mudança comprovada pelo Git, que pode não representar a implementação atual.
- **Pendente ou sem confirmação:** implementação ausente, incompatibilidade identificada ou comportamento ainda não validado.

As referências de código usam caminhos relativos à raiz do repositório e números de linha do estado examinado em 12/10/2026. As linhas podem mudar em alterações futuras.

O fluxo implementado e exercitado nos testes é:

```text
Arquivo H.264 ou RTSP
    → FFmpeg abre a entrada e faz demux
    → AVPacket original
        → clone referenciado na fila → retirada por movimento + parâmetros da stream
        → filtro de bitstream → bytes para OpenCV/NVDEC → frame na GPU
    → VideoStreamReader retorna os dois resultados separadamente
    → integração com o buffer: ainda pendente
```

O detector MOG2 existe no código, mas não foi executado no teste RTSP descrito neste relatório. A presença de suas classes não comprova a integração completa com o novo caminho de pacotes.

**Referências:** [FfmpegRawVideoSource.cpp](motion_mog2/src/FfmpegRawVideoSource.cpp), linhas 32, 52, 236, 247 e 288; [VideoStreamReader.cpp](motion_mog2/src/VideoStreamReader.cpp), linha 52; [Mog2MotionDetector.cpp](motion_mog2/src/Mog2MotionDetector.cpp), linhas 39 e 56.

## 3. Dificuldades encontradas e tratamento adotado

### 3.1. Separar frame decodificado, pacote codificado e lote de uma leitura

**Dificuldade:** havia risco de confundir a imagem usada pela detecção com os bytes usados para preservar o vídeo. Também surgiu a interpretação de que a quantidade indicada por `PROP_NUMBER_OF_RAW_PACKAGES_SINCE_LAST_GRAB` identificaria exatamente os pacotes de um único frame.

**Tratamento:** o leitor retorna um `DecodedFrame`, com imagem na GPU e índice, e um `EncodedVideoPackets`, com pacotes e configuração da stream. O contador raw informa os pacotes consumidos entre aquisições; não estabelece uma correspondência exata pacote–frame.

**Evidência:** no arquivo local, três leituras retornaram cinco pacotes; no RTSP local, três leituras retornaram dez. Esses números pertencem às execuções observadas, não são proporções fixas.

**Situação:** a separação está implementada e testada. Agrupamento definitivo por frame codificado não foi implementado e não pode ser presumido apenas pelo lote retornado.

**Referências:** [VideoStreamReader.hpp](motion_mog2/include/VideoStreamReader.hpp), linhas 14 e 23; [EncodedVideoPackets.hpp](motion_mog2/include/EncodedVideoPackets.hpp), linha 9; [VideoStreamReader.cpp](motion_mog2/src/VideoStreamReader.cpp), linhas 68–82. O commit histórico `38bd243` registra a separação anterior entre `DecodedFrame` e `EncodedPacketBatch`.

### 3.2. Administrar a posse dos AVPacket sem liberar memória duas vezes

**Risco:** reutilizar o pacote de leitura poderia invalidar uma referência guardada no buffer. Tratar o mesmo ponteiro bruto como propriedade de dois objetos independentes poderia provocar double free ou acesso após liberação.

**Tratamento:** foi criado `AvPacketPtr`, um `unique_ptr` com deleter que chama `av_packet_free()`. A alocação e o clone verificam falha e usam RAII para liberar recursos mesmo em saídas por exceção.

É importante distinguir as operações: `av_packet_clone()` cria outro objeto `AVPacket`, mas, para payload referenciado, mantém uma referência aos mesmos bytes. Não é uma promessa de cópia profunda do vídeo. A movimentação do vetor transfere a posse dos objetos sem copiar seus payloads.

**Validação:** o teste de pacote destrói o objeto original e confere que o clone continua válido. Há resultado `Test Passed` no log CTest consultado, em um build configurado com AddressSanitizer.

**Situação:** risco mitigado e comportamento testado. Não foi identificado um double free efetivamente ocorrido nesta sessão.

**Referências:** [AvPacket.hpp](motion_mog2/include/AvPacket.hpp), linha 13; [AvPacket.cpp](motion_mog2/src/AvPacket.cpp), linhas 5, 9 e 19; [av_packet_tests.cpp](motion_mog2/tests/av_packet_tests.cpp), linhas 19–30; commits `dcd3ed4` e `bdd50be`.

### 3.3. Manter os parâmetros da stream vivos após fechar a entrada

**Risco:** `AVStream::codecpar` pertence ao contexto de entrada do FFmpeg. Guardar esse ponteiro como se fosse uma alocação de propriedade independente não resolveria o tempo de vida dos dados e poderia gerar referências inválidas após fechar a fonte.

Além disso, um `AVPacket` sozinho não substitui todos os parâmetros necessários para criar futuramente uma stream de saída.

**Tratamento:** `AvCodecParameters` aloca um destino próprio e usa `avcodec_parameters_copy()`. `VideoStreamParameters` conserva essa cópia e a base de tempo dos pacotes. As coleções compartilham uma configuração imutável por `shared_ptr`, em vez de copiar os parâmetros a cada retirada.

Diferentemente do clone de `AVPacket`, os testes de parâmetros verificam uma cópia independente, inclusive da extradata.

**Validação:** testes verificaram que destruir os parâmetros de origem ou a fonte não invalida a configuração retida; a última referência libera os recursos. Os resultados aparecem no log CTest existente e nas execuções CPU da integração.

**Situação:** implementado e testado. Ter os parâmetros disponíveis facilita o remux futuro, mas não significa que o remux já exista.

**Referências:** [AvCodecParameters.cpp](motion_mog2/src/AvCodecParameters.cpp), linhas 7 e 23; [VideoStreamParameters.cpp](motion_mog2/src/VideoStreamParameters.cpp), linha 5; [VideoStreamParameters.hpp](motion_mog2/include/VideoStreamParameters.hpp), linhas 17–18; [encoded_video_packets_tests.cpp](motion_mog2/tests/encoded_video_packets_tests.cpp), linhas 68–104.

### 3.4. Preservar a extradata e retirar pacotes de uma fila compartilhada

**Dificuldades:** fornecer ao OpenCV os dados de configuração sem expor memória de vida curta; preservar pacotes enquanto o decoder consome a entrada; retirar a quantidade anunciada sem alterar a ordem ou concorrer com a inserção.

**Tratamento:** a extradata é copiada para um `cv::Mat` próprio, e `getExtraData()` entrega outra cópia. A fila de pacotes é protegida por mutex. `dequeueEncodedPackets()` verifica a quantidade disponível e move os elementos da frente da deque para o vetor retornado. `takePacketsWithStreamParameters()` acrescenta a configuração compartilhada.

Os nomes distinguem a retirada física da fila da montagem do resultado com parâmetros. Pedir mais pacotes do que existem gera erro explícito; pedir zero retorna uma coleção vazia com configuração.

**Situação:** implementado e coberto por testes de quantidade, ordem e tempo de vida. Isso não é ainda um buffer por lotes de 1000 frames nem comprova resistência a execução prolongada.

**Referências:** [FfmpegRawVideoSource.cpp](motion_mog2/src/FfmpegRawVideoSource.cpp), símbolos `copyCodecExtraDataFromStream()`, `getExtraData()`, `dequeueEncodedPackets()` e `takePacketsWithStreamParameters()`; [ffmpeg_encoded_video_packets_tests.cpp](motion_mog2/tests/ffmpeg_encoded_video_packets_tests.cpp), linhas 202, 212 e 220. Commits históricos: `33e1934`, `ff2853d`, `c27445d` e `f211cf7`.

### 3.5. Falha genérica na inicialização do NVDEC

**Problema observado:** o teste com arquivo local falhava durante a construção do leitor, em `waitForDecoderInit`, antes de uma leitura bem-sucedida:

```text
Parsing/Decoding video source failed, check GPU memory is available
and GPU supports requested functionality.
```

Essa mensagem genérica não demonstrava, por si só, falta de memória ou incompatibilidade da GPU. Também não era necessário iniciar MediaMTX ou um publicador para abrir diretamente o MP4 local.

**Diagnóstico:** o leitor padrão do OpenCV conseguiu decodificar o mesmo arquivo. A inspeção dos bytes da fonte customizada mostrou o primeiro pacote original do MP4 com prefixos de tamanho, enquanto o decoder precisava da representação adequada do bitstream. O filtro preparou a saída em Annex B.

Exemplo observado no teste CPU:

```text
reference_original_packet: bytes=69161 prefix=00 01 0e 25 65 b8 42 ff
custom_decoder_packet:     bytes=69192 prefix=00 00 00 01 67 64 00 2a
```

**Solução:** criar um filtro persistente `h264_mp4toannexb` e entregar sua saída em `getNextPacket()`. A fila continua recebendo um clone do pacote original, não a saída filtrada. `current_decoder_packet` conserva a memória entregue ao decoder até a próxima chamada de aquisição.

A adaptação reorganiza a representação do bitstream; não decodifica e reencoda o vídeo. Não foi implementado um decoder de vídeo em CPU como substituto do NVDEC.

**Validação:** o teste customizado passou a produzir três frames via NVDEC. O teste CPU comparou a saída filtrada e os originais com uma entrada FFmpeg independente, verificando bytes e metadados.

**Situação:** falha resolvida para o arquivo H.264 testado, com confirmação posterior no cenário RTSP local. Não há garantia para todos os codecs, perfis ou fontes.

**Referências:** [AvBitstreamFilter.cpp](motion_mog2/src/AvBitstreamFilter.cpp), linhas 16 e 68; [FfmpegRawVideoSource.cpp](motion_mog2/src/FfmpegRawVideoSource.cpp), linhas 236, 247 e 253; [ffmpeg_encoded_video_packets_tests.cpp](motion_mog2/tests/ffmpeg_encoded_video_packets_tests.cpp), linhas 130–209. Log original consultado: `/home/davi/.cache/JetBrains/CLion2026.2/tmp/ij_run__video_stream_reader_tests_17659934803999023364.log`.

### 3.6. Usar o filtro sem consumir o original ou dessincronizar a contagem

**Riscos:** `av_bsf_send_packet()` consome a referência do pacote enviado. Enviar diretamente o objeto destinado ao armazenamento seria inadequado. Também seria incorreto presumir que todo filtro FFmpeg genérico produz exatamente uma saída por entrada.

**Tratamento:** o helper clona a referência de entrada, envia esse objeto ao filtro e drena as saídas até `EAGAIN` ou EOF. Verifica entradas vazias e erros do filtro. O contexto persiste entre chamadas.

Na integração atual, específica de H.264 com esse filtro, `getNextPacket()` exige uma única saída válida antes de enfileirar o original. Essa restrição mantém compatível a contagem raw observada pelo OpenCV com a fila de originais. O helper retorna um vetor, mas a fonte não implementa suporte genérico a filtros que produzam zero ou várias saídas por entrada.

Exceções padrão durante a preparação são capturadas no callback, registradas e convertidas em retorno `false`. Isso evita deixar essas exceções escaparem para o trabalhador do OpenCV nesse caminho.

**Validação:** os testes conferem originais intactos, metadados preservados, saída em Annex B, vida útil da saída e rejeição de filtro não drenado. A mensagem `Invalid extradata size: 6` nos testes CPU vem de um caso negativo intencional; esses testes terminaram com código zero.

**Situação:** implementado e testado no escopo atual. Flush genérico de filtros, distinção pública detalhada entre erro e fim de entrada e outros codecs permanecem fora da implementação validada.

**Referências:** [AvBitstreamFilter.cpp](motion_mog2/src/AvBitstreamFilter.cpp), linhas 76–109; [FfmpegRawVideoSource.cpp](motion_mog2/src/FfmpegRawVideoSource.cpp), linhas 240–259; [ffmpeg_encoded_video_packets_tests.cpp](motion_mog2/tests/ffmpeg_encoded_video_packets_tests.cpp).

### 3.7. FPS válido substituído por NaN

**Problema observado no debugger:** a fonte tinha FPS `59.940059940059939`; o formato recebido do decoder continha `NaN`. `updateFormat()` substituía o formato inteiro, perdendo o valor válido. No construtor do leitor, `NaN <= 0.0` resultava em falso, portanto a verificação existente não o rejeitava.

**Solução:** guardar o FPS anterior, aplicar o formato recebido e restaurar o valor anterior quando o novo FPS não é finito ou é menor ou igual a zero. Um FPS novo válido continua sendo aceito; os outros campos do formato continuam sendo atualizados.

**Validação:** o teste CPU cobre `NaN`, infinitos positivo e negativo, zero e valor negativo; também aceita 30 FPS e preserva esse último valor após uma nova atualização inválida. No debugger, a saída da correção manteve `59.940059940059939`. O teste GPU passou com `reader FPS=59.9401`.

**Situação:** problema corrigido na atualização do formato. A proteção adicional com `std::isfinite()` no construtor de `VideoStreamReader` não foi implementada nesta etapa. O FPS informado é metadado, não uma medição de desempenho ou taxa real de processamento.

**Referências:** [FfmpegRawVideoSource.cpp](motion_mog2/src/FfmpegRawVideoSource.cpp), linha 145; [VideoStreamReader.cpp](motion_mog2/src/VideoStreamReader.cpp), linhas 20–28; [ffmpeg_video_stream_parameters_tests.cpp](motion_mog2/tests/ffmpeg_video_stream_parameters_tests.cpp), linhas 65–97; [video_stream_reader_tests.cpp](motion_mog2/tests/video_stream_reader_tests.cpp), linhas 35 e 49.

### 3.8. Testes e build precisaram acompanhar a nova arquitetura

**Dificuldades:** a configuração commitada tinha integração parcial com libavcodec, enquanto a fonte direta passou a depender também de libavformat e libavutil. Alguns testes antigos comparavam o pacote original com os bytes destinados ao decoder, comparação que deixou de ser correta após a filtragem.

**Tratamento:** o CMake local inclui as bibliotecas FFmpeg necessárias e alvos específicos para os componentes. Os testes passaram a distinguir os dois caminhos: demux independente como referência dos originais, e referência filtrada para a saída do decoder.

O teste legado `ffmpeg_raw_video_source_tests.cpp` foi adaptado e compilado manualmente nos diretórios de build, pois não possui alvo próprio no CMake atual. Um caminho relativo incorreto nessa compilação manual foi corrigido de `../../tests` para `../tests`; tratou-se de um ajuste operacional, não de um defeito do leitor.

Também houve `ninja: command not found` ao invocar diretamente o comando pelo PATH. O build do CLion aponta para `/opt/clion/bin/ninja/linux/x64/ninja`; a compilação pela configuração CMake existente funcionou sem instalar outra dependência.

**Validação:** os alvos de integração consultados compilaram e os testes CPU passaram no build normal e com ASan. Os testes GPU foram executados no build normal, sem ASan.

**Situação:** compilação comprovada para os alvos específicos. Não se deve extrapolar isso para o executável completo `motion_mog2`, que tem incompatibilidades de integração descritas adiante.

**Referências:** commit-base `cfbca46`; [CMakeLists.txt](motion_mog2/CMakeLists.txt), linhas 10–12 e alvos de testes; [ffmpeg_encoded_video_packets_tests.cpp](motion_mog2/tests/ffmpeg_encoded_video_packets_tests.cpp), linha 130; [ffmpeg_raw_video_source_tests.cpp](motion_mog2/tests/ffmpeg_raw_video_source_tests.cpp), linhas 81 e 106. Os caches de build confirmam ASan no diretório `build-codec-parameters-raii` e flags sem ASan no `cmake-build-debug`.

### 3.9. Evidências de depuração nem sempre ficaram em logs comuns

**Dificuldade operacional:** os eventos de logpoint não estavam disponíveis para leitura no backend nativo utilizado. Ausência de eventos não poderia ser interpretada como prova de que uma linha não foi executada.

**Tratamento:** foram usadas pausas curtas em breakpoints, inspeção de pilha e leitura de valores para verificar prefixos, tamanhos, FPS e estado dos frames na GPU. Os breakpoints criados para essas verificações foram removidos, e os breakpoints do usuário foram preservados.

**Limite:** valores lidos no debugger são evidência de pontos específicos da execução; os testes de linha de comando, com saída e código de retorno, fornecem a confirmação de conclusão dos cenários. Parte dos registros está apenas na conversa e em arquivos temporários do IDE.

**Situação:** diagnóstico realizado, sem transformar a observação pontual em garantia de robustez geral.

### 3.10. Preparar um RTSP local sem envolver a aplicação completa

**Dificuldade:** os scripts existentes iniciam também transcoding com `h264_nvenc`, FFplay e MOG2. Usá-los integralmente misturaria comportamentos e não isolaria a integração do novo leitor.

**Tratamento:** foi montado um teste temporário com MediaMTX, publicação do MP4 por `-c:v copy`, consulta por ffprobe, controle OpenCV e teste customizado, nessa ordem. Não foram executados MOG2, gravação ou o executável principal.

O vídeo usado foi `/home/davi/Vídeos/tcc/video_h264_fhd_60fps.mp4`. A URL foi `rtsp://127.0.0.1:18554/nvdec_smoke`.

**Problema observado na primeira preparação:** o MediaMTX v1.19.2 usa a mensagem `started with listeners on`, enquanto a verificação esperava `listener opened on`. Além disso, a configuração incompleta deixou MoQ habilitado por padrão. A tentativa foi encerrada antes de publicar o vídeo ou executar os leitores.

**Solução:** corrigir a verificação de inicialização e incluir `moq: false`, mantendo apenas RTSP na interface de loopback. A repetição usou outro diretório temporário para preservar os logs da primeira tentativa.

**Validação:** ffprobe confirmou H.264 1920×1080; o controle OpenCV e o leitor customizado passaram. Os processos iniciados para o teste foram encerrados e a porta foi liberada. A comparação do diff Git e dos hashes dos arquivos examinados confirmou que o teste não alterou o código do projeto.

**Situação:** preparação corrigida e integração validada nesse cenário local.

**Referências:** [run_mog2_test.sh](run_mog2_test.sh), linhas 163–210; logs em `/tmp/tcc-rtsp-nvdec.hSRzaH` e `/tmp/tcc-rtsp-nvdec.PCWc7x`; [configuração oficial do MediaMTX v1.19.2](https://raw.githubusercontent.com/bluenviron/mediamtx/v1.19.2/mediamtx.yml), chave `moq`.

### 3.11. Aviso 461 Unsupported Transport

**Problema observado:**

```text
method SETUP failed: 461 Unsupported Transport
```

No RTSP, esse código indica que a proposta de transporte enviada no pedido não foi aceita pelo servidor. Não indica, por si só, problema do codec H.264 ou do NVDEC.

**Constatação:** o servidor do teste aceitava apenas TCP. Houve uma tentativa inicial rejeitada e depois uma conexão de leitura por TCP, registrada pelo MediaMTX. Não foi capturado o cabeçalho Transport da tentativa recusada; atribuí-la especificamente a UDP seria uma inferência, não uma leitura direta desse pedido.

**Resultado:** o leitor completou o teste e terminou com código zero. Isso representa negociação posterior bem-sucedida, não eliminação do aviso.

**Situação:** pendência de robustez. Não foi alterada a política de transporte do código. A discussão sobre UDP/TCP e seus custos foi explicitamente adiada; este teste sobre TCP não fixa uma escolha definitiva para o projeto.

**Referências:** `/tmp/tcc-rtsp-nvdec.PCWc7x/video_stream_reader.log:1`; `mediamtx.yml:9` e `mediamtx.log:21` no mesmo diretório; [FfmpegRawVideoSource.cpp](motion_mog2/src/FfmpegRawVideoSource.cpp), linhas 54–58, sem opções explícitas de transporte; [RFC 2326, seção 11.3.12](https://www.rfc-editor.org/rfc/rfc2326.html#section-11.3.12).

### 3.12. Mensagens de transporte e encerramento não equivalem a transcoding

O log do MediaMTX registra ajuste de pacotes RTP grandes, de 1460 para o limite de 1440 bytes, usando a expressão `remuxing them into smaller ones`. Esse registro é sobre o empacotamento RTP no servidor de teste, não prova de que exista no projeto um remuxer para gravar MP4.

A publicação FFmpeg registrou `Stream #0:0 -> #0:0 (copy)`, evidenciando que não houve reencodificação pelo publicador. Não se deve confundir preservar o vídeo codificado com manter byte a byte os envelopes de transporte ou do contêiner MP4 original.

No encerramento automático, o publicador registrou `Broken pipe` e `received signal 15`. Os testes já tinham retornado sucesso antes dessa limpeza; a mensagem deve ser lida no contexto de interrupção do publicador e fechamento do servidor, não apresentada como falha do teste de leitura.

**Situação:** observações documentadas. Gravação MP4 continua não implementada.

**Referências:** `/tmp/tcc-rtsp-nvdec.PCWc7x/mediamtx.log:8` e `ffmpeg.log`, trecho de mapeamento e linhas finais.

### 3.13. Decisões históricas de MOG2, descarte, GOP e pré-evento

O Git registra avanços anteriores de separação de responsabilidades e de preparação de trechos por GOP e pré-evento. O commit `31e3baa` alterou `allowFrameDrop` de verdadeiro para falso; esse valor permanece falso no leitor atual.

O código do MOG2 contém aquecimento, limiar de foreground, abertura morfológica e contadores para confirmar início e fim de movimento. São tratamentos implementados para a decisão de movimento; não há nesta validação recente evidência de precisão, recall, ausência de falsos positivos ou desempenho prolongado.

O commit `79a3998` registra pré-evento configurável. A documentação histórica descreve alinhamento ao keyframe, margem anterior ao evento, prevenção de duplicação dos pacotes do ciclo de início e crescimento de memória dependente do bitrate e dos GOPs. A conversão de segundos para frames, com validação e arredondamento, ainda existe em `PreEventConfig.cpp`.

**Situação:** o histórico está comprovado, mas o buffer atual foi reduzido e não oferece aquela API completa. Não se pode apresentar GOP, pré-evento e eventos de movimento como plenamente integrados ao novo buffer de pacotes. O trade-off entre atraso e descarte de frames também não foi validado em execução prolongada.

**Referências:** commits `31e3baa`, `38bd243` e `79a3998`; [Mog2MotionDetector.cpp](motion_mog2/src/Mog2MotionDetector.cpp), linhas 39, 56, 90 e 104; [PreEventConfig.cpp](motion_mog2/src/PreEventConfig.cpp), linhas 7 e 27; [PRE_EVENT.md](motion_mog2/PRE_EVENT.md).

### 3.14. Compatibilidade com FFmpeg 6 em outra branch

**Registro histórico:** o commit `5e63053`, presente em `agent/tcc-stepwise-clean`, substitui a referência à flag inexistente `AV_PIX_FMT_FLAG_GRAY` pela verificação `descriptor->nb_components == 1` para identificar formato monocromático.

**Situação:** a correção está documentada no diff desse commit, mas pertence a outra branch e não deve ser confundida com a solução atual da falha NVDEC. Não foi localizada nesta elaboração uma evidência de execução que permita afirmar a validação completa daquela implementação histórica.

### 3.15. Buffer, main e documentação ficaram em estados diferentes

**Pendência de implementação:** `EncodedVideoBuffer` possui um construtor que rejeita capacidade zero e faz reserve. `appendPacket()` está apenas declarado. A capacidade se chama `batch_packet_capacity` e representa pacotes, não frames decodificados.

A estrutura atual do buffer usa `vector<EncodedPacket>`, enquanto o leitor retorna `EncodedVideoPackets`, com `vector<AvPacketPtr>` e parâmetros compartilhados. Falta a integração entre esses contratos.

**Pendência de aplicação:** o `main` ainda chama `updatePreEventBuffer()`, `startMotion()`, `appendMotionPackets()`, `finishMotion()` e `currentMotionInfo()`, que não constam da classe atual. Isso é uma incompatibilidade identificável por inspeção. Não foi feita nesta elaboração uma nova compilação do executável completo; não há confirmação de que ele compile no estado atual.

Há também um contrato incompatível para capacidade zero: o `main` passa ao buffer a contagem calculada de pré-evento, cujo valor padrão é zero, enquanto o construtor atual rejeita zero. Esse caminho não foi executado nesta elaboração; é uma pendência identificada por inspeção, a ser tratada ao integrar o buffer por lotes, sem confundir sua capacidade com a janela antiga de pré-evento.

**Pendência documental:** o estado do repositório no `AGENTS.md` ainda menciona `d18008a` e ausência da integração direta FFmpeg, enquanto o HEAD é `cfbca46` e essa integração existe no working tree. `NOTES.md` ainda mostra descarte de frames habilitado, e `PRE_EVENT.md` descreve métodos e um alvo CTest que não estão na configuração atual. O arquivo de testes de pré-evento conserva chamadas à API antiga.

**Situação:** não resolvido. Os arquivos não foram corrigidos silenciosamente para produzir este relatório.

**Referências:** [EncodedVideoBuffer.hpp](motion_mog2/include/EncodedVideoBuffer.hpp), linhas 10–16; [EncodedVideoBuffer.cpp](motion_mog2/src/EncodedVideoBuffer.cpp), linhas 5–8; [main.cpp](motion_mog2/src/main.cpp), linhas 45, 56, 71, 115, 128, 154, 161 e 182; [PreEventConfig.cpp](motion_mog2/src/PreEventConfig.cpp), linha 27; [pre_event_tests.cpp](motion_mog2/tests/pre_event_tests.cpp), linhas 50–52; [CMakeLists.txt](motion_mog2/CMakeLists.txt); [AGENTS.md](AGENTS.md); [NOTES.md](motion_mog2/NOTES.md).

## 4. Evidências de validação disponíveis

Nenhum teste ou transmissão foi iniciado para redigir este relatório. Os resultados abaixo são de execuções anteriores, consultadas em logs ou registradas nesta sessão de desenvolvimento.

| Componente/cenário | Evidência disponível | Resultado e limite |
| --- | --- | --- |
| AVPacket, parâmetros de codec, parâmetros da stream e coleção de pacotes | Log CTest de 11/10, 21:24, no build configurado com ASan | Quatro testes com `Test Passed`; validação de ownership e dados, não de MP4 gravado |
| Integração CPU da fonte e preservação dos parâmetros | Execuções normais e com ASan registradas na sessão | Código zero; atualização de FPS, configuração e vida útil verificadas |
| Integração CPU do filtro e da fila | Execuções normais e com ASan registradas na sessão | Código zero; originais comparados com demux independente e decoder comparado com saída filtrada |
| Teste legado da fonte | Compilação manual e execuções normais/com ASan registradas na sessão | Código zero; três saídas Annex B e preservação do original |
| Controle OpenCV com MP4 local | Execução registrada na sessão | Três frames via NVDEC, 1920×1080 |
| Leitor customizado com MP4 local | Execução registrada na sessão e inspeção no debugger | Três frames, cinco pacotes e FPS válido |
| Controle OpenCV com RTSP local | `opencv_control.log` | Três frames via NVDEC, 1920×1080 |
| Leitor customizado com RTSP local | `video_stream_reader.log` e resultados do executor | Três frames, dez pacotes, FPS válido e código zero; aviso 461 permanece |
| Buffer por lotes | Código do construtor e teste sintético existentes | Não comprova append, fechamento, retirada ou repetição de lotes; não validado como buffer funcional |
| Aplicação completa com MOG2 e novo buffer | Código parcialmente incompatível, sem execução recente correspondente | Integração e build completo não comprovados |

O log CTest consultado está em [LastTest.log](motion_mog2/build-codec-parameters-raii/Testing/Temporary/LastTest.log). A configuração ASan consta em `motion_mog2/build-codec-parameters-raii/CMakeCache.txt`. As execuções CPU de integração registraram `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1`, sem erros de memória ou vazamentos reportados nos cenários exercitados; isso não garante ausência de todos os defeitos possíveis.

### 4.1. Trechos preservados das execuções CPU

```text
custom_extradata: bytes=35 prefix=01 64 00 2a ff e1 00 13
filter_output_extradata: bytes=32 prefix=00 00 00 01 67 64 00 2a
custom_decoder_packet: bytes=69192 prefix=00 00 00 01 67 64 00 2a
reference_original_packet: bytes=69161 prefix=00 01 0e 25 65 b8 42 ff

PASS: fonte entrega tres pacotes Annex B, preserva originais e extradata e mantem vida util segura.
PASS: updateFormat preserva FPS validos, aceita uma nova taxa e atualiza os demais campos.
```

Esses trechos são saídas registradas na sessão, não arquivos novos de log gerados para este relatório.

### 4.2. Resultado preservado do arquivo local

```text
reader FPS=59.9401
PASS: tres frames via NVDEC, 5 pacotes com configuracao compartilhada e vida util segura.
```

### 4.3. Resultado preservado do RTSP local

ffprobe registrou:

```json
{
  "codec_name": "h264",
  "width": 1920,
  "height": 1080,
  "r_frame_rate": "60000/1001",
  "avg_frame_rate": "0/0"
}
```

O leitor registrou:

```text
[rtsp @ 0x59752af3a200] method SETUP failed: 461 Unsupported Transport
reader FPS=59.9401
PASS: tres frames via NVDEC, 10 pacotes com configuracao compartilhada e vida util segura.
```

O servidor registrou a leitura da sessão correspondente com TCP e o encerramento posterior. O `avg_frame_rate=0/0` exportado pelo ffprobe não é uma medição de FPS igual a zero; o cenário produziu FPS válido no leitor. Não foram feitos benchmarks de taxa real de chegada ou processamento.

**Arquivos do teste concluído:** `/tmp/tcc-rtsp-nvdec.PCWc7x/ffprobe.json`, `opencv_control.log`, `video_stream_reader.log`, `mediamtx.log`, `ffmpeg.log` e `mediamtx.yml`.

**Arquivos da preparação inicial interrompida:** `/tmp/tcc-rtsp-nvdec.hSRzaH/mediamtx.log` e `mediamtx.yml`.

Logs em `/tmp` e no cache do IDE podem desaparecer. Os trechos essenciais foram transcritos acima para tornar o relatório utilizável sem depender exclusivamente desses caminhos.

## 5. Situação no Git

As etapas iniciais da integração FFmpeg constam do histórico da branch atual, incluindo alocação e clone de pacote, abertura de entrada, formato, extradata, leitura e retirada da fila. O HEAD atual é `cfbca46`.

A configuração ampliada de build, a integração atual do leitor, os wrappers de parâmetros, a coleção com configuração, o filtro de bitstream e os testes associados estão, ao menos em parte, em alterações locais ou arquivos ainda não rastreados. Não devem ser atribuídos a um commit inexistente.

Há mudanças preexistentes no buffer, no `main`, em headers e em testes. Este relatório não resolve essas diferenças nem agrupa etapas em um commit. Também não atribui autoria ou motivação a pessoas apenas a partir da existência de arquivos.

Para reconstituir o desenvolvimento, devem ser considerados em conjunto o histórico da branch e o working tree. A correção histórica `5e63053` foi examinada separadamente por pertencer a outra branch.

## 6. Pendências e próximos passos do MVP

### 6.1. Próxima fase: buffer por lotes

A infraestrutura de aquisição e de posse dos pacotes está disponível. O objetivo discutido para o próximo MVP é acumular os dados codificados durante 1000 leituras bem-sucedidas de frames decodificados, entregar o lote e iniciar outro, inicialmente sem condicionamento por movimento.

Ainda faltam:

1. Definir e adaptar o estado do buffer para reutilizar pacotes e parâmetros existentes, separando capacidade em frames de quantidade de pacotes.
2. Implementar a entrada por leitura, transferindo ownership sem copiar desnecessariamente o vídeo.
3. Implementar o fechamento/retirada do lote e o reinício da contagem, sem descartar dados antes da entrega.
4. Testar limites pequenos e dois ciclos consecutivos antes de validar 1000 frames.
5. Integrar primeiro em um teste isolado, sem misturar a API antiga de movimento do `main`.

Contar 1000 frames decodificados não garante automaticamente 1000 pacotes, nem um futuro MP4 com exatamente 1000 imagens: a decodificação pode consumir pacotes antecipadamente e os limites de um trecho reproduzível precisam considerar dependências e keyframes.

O `AGENTS.md` ainda proíbe expressamente implementar o buffer de 1000 frames. Esse escopo precisa ser atualizado ou explicitamente revisto antes de executar a nova fase; o relatório não altera essa instrução.

### 6.2. Robustez e funcionalidades posteriores

Continuam pendentes, sem ordem rígida de implementação além das etapas individuais:

- Resolver a discussão UDP/TCP e tratar a negociação de transporte sem assumir que o teste local fixa a escolha de produção.
- Complementar a validação de FPS no construtor e avaliar metadados ausentes ou variáveis.
- Testar câmera real, execução prolongada, memória, atraso, encerramento e falhas de rede.
- Reconciliar `main`, testes antigos e documentação com a API efetivamente adotada.
- Validar a detecção de movimento no fluxo integrado atual, com critérios de qualidade e eventos observáveis.
- Implementar futuramente o remux para MP4: criação da stream de saída, parâmetros e extradata, timestamps/base de tempo, limites por keyframe e finalização do contêiner.
- Verificar reprodução e conteúdo dos arquivos produzidos. Nada disso é comprovado pelos testes de aquisição já realizados.

## 7. Conclusão para apresentação

O projeto superou a principal dificuldade de integração entre a fonte FFmpeg e o decoder NVDEC: os dados destinados à decodificação agora recebem a adaptação de bitstream necessária, enquanto os pacotes originais e a configuração da stream permanecem disponíveis com ownership controlado. Testes CPU, verificações com AddressSanitizer, inspeções no debugger e testes curtos em arquivo e RTSP local sustentam esse avanço. A próxima entrega do MVP é o buffer por lotes; a aplicação completa de detecção, retenção e gravação de trechos reproduzíveis em MP4 ainda não está concluída.
