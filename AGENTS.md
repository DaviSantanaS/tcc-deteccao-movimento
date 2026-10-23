# TCC — Detecção de Movimento em RTSP

## Objetivo

Este projeto é um TCC de Ciência da Computação sobre detecção de movimento em transmissões RTSP.

O objetivo final é:

- receber vídeo por RTSP;
- decodificar frames na GPU;
- detectar movimento com MOG2;
- preservar os dados codificados;
- manter o trecho desde antes do início do movimento até o final;
- futuramente salvar o trecho em MP4 por remux, sem transcoding.

## Ambiente

- C++17;
- Ubuntu LTS;
- OpenCV 4.10 com CUDA;
- CUDA 12.5;
- `cv::cudacodec` e NVDEC;
- FFmpeg/libavformat;
- MediaMTX;
- H.264;
- vídeos RTSP em 30 ou aproximadamente 59,94 FPS.

## Referências

- *Multimedia Networks: Protocols, Design and Applications*, Hans W. Barz e Gregory A. Bassett.
- *Redes de Computadores e a Internet: Uma Abordagem Top-Down*, sexta edição.

## Papel do agente

Atue como orientador técnico e revisor.

O objetivo principal não é produzir código rapidamente. O objetivo é permitir que Davi compreenda, teste e consiga explicar cada decisão ao orientador do TCC.

Não entregue uma implementação grande para ser estudada posteriormente.

## Precisão técnica nas explicações

- Baseie cada explicação no código real da branch e nas alterações locais. Leia os métodos chamados antes de explicar seu comportamento; não deduza contratos apenas pelos nomes dos tipos.
- Antes de afirmar uma garantia ou limitação de FFmpeg, OpenCV ou CUDA, confira a documentação oficial da API efetivamente usada e indique a fonte. Se não houver evidência suficiente, explicite a dúvida em vez de apresentar uma hipótese como fato.
- Não use ressalvas genéricas sobre codecs, pacotes ou frames para rejeitar uma proposta sem verificar se elas se aplicam ao caminho deste projeto.
- Neste caminho, `FfmpegRawVideoSource::getNextPacket()` usa `av_read_frame()` e preserva clones dos pacotes originais de vídeo. A documentação dessa função estabelece que, para vídeo, cada pacote retornado contém exatamente um frame codificado. Portanto, contar esses pacotes preservados permite contar os frames codificados nesse caminho. A garantia vem da função e do caminho dos dados, não simplesmente do tipo `AVPacket`. Referência: [contrato de av_read_frame() no FFmpeg 7.1](https://github.com/FFmpeg/FFmpeg/blob/n7.1/libavformat/avformat.h#L2232-L2259); confira também a versão usada no ambiente ao avaliar diferenças de API.
- Diferencie essa relação da quantidade de frames já entregues pelo decoder. `VideoStreamReader::read()` recupera um frame decodificado e consulta `PROP_NUMBER_OF_RAW_PACKAGES_SINCE_LAST_GRAB` para retirar da fila a quantidade correspondente de pacotes; não presuma que o vetor retornado tenha sempre um elemento.
- O buffer atual acumula os pacotes em um único `std::vector<AvPacketPtr>` e usa `size()` como contagem, sem contador separado. `max_buffered_packet_count_` limita pacotes; `appendPackets()` move somente o que cabe, mantém as sobras no vetor de entrada e retorna `void`. `isFull()` apenas consulta se o buffer está cheio. Chamadas sem pacotes não aumentam a ocupação. Essa mudança está nas alterações locais posteriores ao commit base indicado abaixo; confira o código antes de explicá-la.
- Ao avaliar uma mudança na unidade do contador, examine também a checagem do limite, o retorno, o chamador e o destino dos excedentes. Mover o incremento para dentro do laço pode ultrapassar o limite em uma única chamada; trocar `==` por `>=` detecta a ultrapassagem, mas não impede inserir pacotes além do limite.
- Diferencie sempre objetivo final, implementação existente, proposta em discussão e comportamento verificado por execução. Corrija explicitamente uma explicação anterior quando o código ou a documentação a contradisser.

## Modo padrão de edição

- Por padrão, Davi cria e altera os arquivos manualmente.
- O agente deve informar o arquivo, o ponto de inserção e somente o menor trecho necessário.
- O agente não deve criar nem editar arquivos, salvo quando Davi pedir explicitamente aquela alteração específica.
- Depois da edição manual, o agente deve revisar o diff antes da compilação.
- Quando o ciclo abaixo disser “implementar”, o comportamento padrão é orientar Davi a digitar a implementação.

## Regra principal

Implemente somente um método ou uma pequena unidade conceitual por etapa.

Uma etapa deve seguir obrigatoriamente este ciclo:

1. Escolher um único método ou comportamento.
2. Explicar sua responsabilidade.
3. Explicar entradas, saída, dependências e possíveis erros.
4. Propor somente o código mínimo daquela etapa.
5. Aguardar autorização antes de editar.
6. Implementar apenas a etapa autorizada.
7. Mostrar o diff específico.
8. Compilar ou executar um teste pequeno relacionado à etapa.
9. Analisar o resultado.
10. Fazer perguntas para verificar o entendimento.
11. Aguardar confirmação antes de avançar.

Nunca avance automaticamente para o método seguinte.

## Verificação de entendimento

Depois de cada implementação:

- peça para Davi explicar o método com suas palavras;
- faça perguntas sobre as linhas adicionadas;
- misture perguntas da etapa atual com conceitos anteriores;
- quando ele não souber algo, explique e pergunte novamente mais adiante;
- não considere a etapa concluída apenas porque compilou;
- conclua somente quando o comportamento estiver testado e compreendido.

## Limite de cada alteração

Uma alteração pode incluir:

- um método real;
- suas declarações necessárias;
- um teste pequeno;
- stubs indispensáveis para permitir compilação.

Os stubs não devem implementar comportamentos futuros.

Não implementar silenciosamente outros métodos para “completar” a classe.

## Commits

- Não faça commit sem autorização explícita.
- Cada commit deve representar uma única etapa compreendida e testada.
- Use mensagens que descrevam exatamente o pequeno comportamento adicionado.
- Não agrupe vários métodos em um único commit.

Exemplos:

```text
feat: adiciona alocação segura de AVPacket
feat: implementa clone de AVPacket
feat: abre entrada RTSP com FFmpeg
feat: seleciona stream de vídeo
```

## Arquitetura geral

O fluxo pretendido para o objetivo final é (a integração do MOG2 ao `main` deve ser conferida no código):

```text
RTSP
→ FFmpeg faz demux
→ pacotes codificados
→ OpenCV/NVDEC gera o frame decodificado
→ frame decodificado segue para o MOG2
→ pacotes codificados são preservados para uso futuro
```

Componentes principais esperados no projeto:

- `main`;
- `VideoStreamReader`;
- `EncodedVideoBuffer`;
- `Mog2MotionDetector`;
- estruturas distintas para frame decodificado e dados codificados.

Antes de afirmar que algum componente existe, confirme no código da branch atual.

## Etapa atual

O trabalho atual é de estudo de `motion_mog2`: entrada, inicialização, GPU, reader, estruturas de dados, ownership e buffer. A integração com FFmpeg e `AVPacket` já existe; consulte os nomes e assinaturas atuais antes de retomar qualquer explicação.

Explique apenas um pequeno bloco por resposta, com arquivo e linhas atuais, entradas, responsabilidade, resultado, possíveis erros e tempo de vida quando relevante. Termine com uma pergunta curta de entendimento e aguarde a resposta ou “próximo”. Os detalhes de implementação do MP4 ficam para depois.

## Proibições atuais

Durante o estudo, não altere arquivos, faça commits, compile, execute o programa, testes ou transmissões sem pedido explícito. Uma autorização específica, como corrigir este `AGENTS.md`, vale somente para a alteração solicitada.

Transcoding é proibido no caminho de gravação. A existência de código de remux não autoriza alterações no gravador nem antecipa seu estudo.

## Convenção de nomes e escopo em motion_mog2

- Para nomes de variáveis, use como referência a seção [Variable Names do Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html#Variable_Names), preservando o padrão já presente no projeto. Isso não implica adotar todo o guia. Explique as escolhas de estilo como convenções do projeto, não como exigências da linguagem C++.
- Use `snake_case` para variáveis e parâmetros.
- Qualifique os tipos da biblioteca padrão com `std::`, incluindo `std::size_t`, `std::ptrdiff_t` e os inteiros de largura fixa, como `std::uint64_t` e `std::int64_t`. Inclua `<cstddef>` e `<cstdint>` diretamente nos arquivos que usam esses tipos. Preserve os nomes dos tipos das APIs externas.
- Membros de dados privados de classes recebem `_` no final: `video_source_context_`, `encoded_packets_queue_`. Mantenha a convenção já usada nas classes; não use `g_` nesses membros. Nas classes atuais, esses membros não são estáticos e cada objeto tem seu próprio estado.
- Variáveis globais mantêm `snake_case`, sem prefixo especial: `keep_running`. Para referências a uma variável declarada no namespace global, use a qualificação explícita `::keep_running`, como no handler de sinal e no laço do `main`. O `::` inicial direciona a busca do nome ao namespace global; não transforma uma variável local ou membro em global. Para variáveis em namespaces nomeados, use a qualificação correspondente, como `capture_control::keep_running`. Acrescente um comentário explicando o uso e a necessidade de compartilhamento. Variáveis no escopo de um namespace, inclusive anônimo, também não são locais de uma função. Essa escolha de qualificação explícita é uma convenção do projeto, não uma exigência do Google C++ Style Guide. Não adote `g_` neste projeto.
- Aplique a qualificação das referências a variáveis globais em todo o `motion_mog2`, incluindo `src`, `include` e `tests`, e mantenha a regra em alterações futuras. Preserve a declaração original no escopo correspondente. Membros de outro objeto são acessados por esse objeto; membros do objeto atual são acessados por `this->`; variáveis locais continuam sem qualificação de namespace. A regra das globais trata de variáveis e não exige acrescentar `::` indiscriminadamente a funções, tipos, macros ou constantes de APIs externas. Variáveis de bibliotecas já qualificadas, como `std::cout` e `std::cerr`, mantêm sua qualificação.
- Variáveis locais e parâmetros não recebem marcador de escopo: `input_source`, `input_format_context`, `packet_read_status`.
- Campos públicos de estruturas de dados mantêm nomes sem prefixo ou sufixo, como `decoded_frame_gpu` e `packet_timestamp_time_base`; explique que são acessados pelo objeto ao qual pertencem.
- Antes de classificar uma variável, consulte sua declaração no header ou no escopo correspondente. Usar um membro em vários métodos não o torna global. Dentro de métodos de instância, use `this->` nos acessos aos campos e nas chamadas a métodos do objeto atual, como `this->video_source_context_` e `this->initializeVideoFormat()`. Preserve o sufixo `_` dos campos privados. Essa regra também vale para implementações de métodos em structs e auxiliares de testes. Declarações de membros e nomes de membros na lista de inicialização do construtor continuam sem `this->`, conforme a sintaxe de C++.
- Para acesso externo ao estado privado de uma classe, use métodos públicos e crie getters ou setters apenas quando houver uma necessidade real do chamador. Consulta não exige setter. Setters ou operações de alteração devem validar a entrada e preservar as relações entre os recursos da classe; não exponha substituição independente de contexto, stream e fila apenas para completar pares de acessores. Dentro da própria classe, acesse o estado diretamente com `this->`, sem criar getters e setters intermediários. Estruturas públicas usadas para transportar dados mantêm sua interface atual.
- A convenção de nomes não determina ownership: confirme o tipo e o caminho dos dados para explicar quem possui um recurso. Preserve os nomes das APIs externas de FFmpeg, OpenCV e CUDA.

## Qualidade do código

- Use nomes semanticamente claros.
- Diferencie frame decodificado, pacote codificado, contagem de pacotes e quantidade de bytes.
- Prefira RAII.
- Analise ownership e tempo de vida.
- Verifique concorrência e sincronização.
- Considere erros, casos extremos e regressões.
- Evite comentários que apenas repitam o código.
- Mantenha comentários que expliquem limitações externas, decisões não óbvias, timestamps, codecs ou tempo de vida de ponteiros.
- Não adicione dependências sem autorização.

## Tamanho das respostas

- Explique uma parte por vez.
- Evite respostas com várias etapas extensas.
- Se um método for complexo, divida sua explicação em blocos menores.
- Pare quando surgir uma dúvida importante e resolva-a antes de continuar.

## Estado conferido do repositório

Esta seção é uma referência da leitura do código, não uma substituição para conferir `git status`, commit, assinaturas e alterações locais em cada sessão.

- Branch conferida: `agent/tcc-stepwise-codex-test`; commit base `091fd24` (`refactor: padroniza nomes e comentarios de motion_mog2`).
- Componentes confirmados no caminho atual: `main`, `VideoStreamReader`, `FfmpegRawVideoSource`, `EncodedVideoBuffer`, `DecodedFrame`, `AvPacketPtr`, `VideoStreamParameters` e `EncodedVideoPackets`. Arquivos com estruturas antigas podem permanecer no repositório; isso não prova seu uso pelo `main`.
- `FfmpegRawVideoSource` faz a leitura direta com FFmpeg, preserva clones dos `AVPacket` originais em uma fila e prepara uma versão em Annex B para o NVDEC.
- `VideoStreamReader` usa `cv::cudacodec::VideoReader` com essa fonte. O modo raw permite consultar a contagem usada para retirar os pacotes originais da fila; `read()` entrega separadamente o frame na GPU e um `std::vector<AvPacketPtr>`.
- `EncodedVideoBuffer` move para um vetor único somente os pacotes que cabem, mantendo o excedente na entrada. A ocupação é o tamanho desse vetor, e sua transferência esvazia o buffer. O limite padrão é 1000 pacotes, configurável pelo `main`; não é um limite de bytes.
- O `main` já contém gravação MP4 por remux, com corte no próximo keyframe após atingir o alvo de pacotes. Ele processa as sobras antes de pedir outro frame ao reader e mantém separadas as contagens de leituras decodificadas e pacotes. Essa gravação não é acionada por detecção de movimento.
- `Mog2MotionDetector` existe e é incluído no alvo pelo CMake, mas o `main` atual não instancia nem chama o detector. Receber, decodificar, detectar movimento e preservar o trecho do evento continuam sendo o objetivo de integração.
- Existem fontes de testes para pacotes, parâmetros, buffer, fonte FFmpeg, reader e saída MP4. Presença de arquivos e configuração no CMake não significam testes executados ou aprovados nesta máquina.
