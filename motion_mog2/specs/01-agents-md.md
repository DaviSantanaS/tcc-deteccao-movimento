# Spec 01 — Atualizar `AGENTS.md` para implementação por spec

## Objetivo

Hoje o `AGENTS.md` manda o agente só orientar, implementar um método por vez e esperar autorização. Também traz nomes desatualizados. Esta spec cria um modo explícito em que o agente implementa uma spec inteira de `motion_mog2/specs/` e corrige a seção de estado.

## Escopo permitido

- `AGENTS.md` (raiz do repositório). Nenhum outro arquivo.

## Alterações

### 1. Nova seção logo antes de "## Modo padrão de edição"

Inclua o texto abaixo, ajustando só a formatação:

```markdown
## Modo de implementação por spec

Quando o pedido mandar executar um arquivo de `motion_mog2/specs/`, este modo substitui "Modo padrão de edição", "Regra principal", "Verificação de entendimento", "Limite de cada alteração" e "Proibições atuais", somente para aquela spec:

- Implemente tudo o que a spec pede, e somente isso. Altere apenas os arquivos listados em "Escopo permitido".
- Se a spec contradisser o código atual, pare e relate a divergência em vez de improvisar.
- Siga as convenções de nomes e de qualidade deste arquivo.
- Pode compilar e rodar os testes que a spec manda. Sem GPU, OpenCV com CUDA ou RTSP disponíveis, diga exatamente o que não foi possível rodar. Nunca declare um teste como aprovado sem tê-lo executado e visto a saída.
- Faça um único commit ao final, com a mensagem indicada na spec. Não faça push.
- Termine com o relatório pedido na spec: arquivos alterados, comandos executados com o resultado real, o que ficou sem verificação e qualquer decisão tomada que a spec não cobria.
- Não adicione dependências além das autorizadas na spec.
- Transcoding continua proibido.
```

### 2. Corrigir "## Estado conferido do repositório"

- Troque `FfmpegRawVideoSource` por `FfmpegEncodedVideoSource` em todo o arquivo. Isso inclui a seção "Precisão técnica nas explicações".
- Troque `isFull()` por `checkIfFull()`.
- Acrescente: `VideoStreamReader` usa `allowFrameDrop = false`, `rawMode = true` e `udpSource = true` (`motion_mog2/src/input/VideoStreamReader.cpp`).
- Acrescente: está planejada a substituição da fila da fonte, do vetor pendente do `main` e do `EncodedVideoBuffer` por uma fila única (`EncodedPacketQueue`), conforme `motion_mog2/specs/`.
- Atualize o commit base para o commit atual da branch (`git rev-parse --short HEAD`, antes do seu commit).

## Aceitação

- `git diff --stat` mostra só `AGENTS.md`.
- `grep -n "FfmpegRawVideoSource\|isFull()" AGENTS.md` não retorna nada.

## Commit

`docs: adiciona modo de implementacao por spec ao AGENTS`

## Relatório

Diff resumido por seção e saída dos dois comandos de aceitação.
