# Specs: buffer único (EncodedPacketQueue) e gravação por evento

Branch: `agent/tcc-stepwise-codex-test`. Pasta: `motion_mog2/`.

## Como usar

1. Copie esta pasta para `motion_mog2/specs/` e faça commit (`docs: adiciona specs da fila unica`).
2. Mande ao Codex **uma spec por vez**, nesta ordem, com o prompt:

   > Leia `AGENTS.md` e `motion_mog2/specs/NN-nome.md`. Execute somente essa spec. Ao terminar, responda com o relatório pedido na seção "Relatório".

3. Depois de cada spec:
   - rode você mesmo os testes manuais da seção "Aceitação" que exigem GPU/RTSP;
   - faça push e peça a revisão do diff antes de mandar a próxima.
4. Não pule etapas. Cada uma depende da anterior estar revisada e verde.

| Spec | O que entrega | Precisa de GPU para validar? |
|---|---|---|
| 01 | `AGENTS.md` atualizado para o modo "implementação por spec" | Não |
| 02 | Classe `EncodedPacketQueue` + testes unitários | Não |
| 03 | Fonte FFmpeg usando a classe, comportamento idêntico ao atual | Sim (paridade de MP4) |
| 04 | MOG2 no `main`, só log (gravação contínua inalterada) | Sim |
| 05 | Gravação por evento com pré-evento; remove o vetor pendente e o `EncodedVideoBuffer` do `main` | Sim |
| 06 | Encerramento (EOF/Ctrl+C) sem perder pacotes do evento | Sim |

## Decisões assumidas (confirme antes da spec 05)

- **Pré-evento:** padrão de 5 s, configurável pela CLI. Convertido em pacotes por `calculatePreEventFrameCount` (1 pacote = 1 frame, contrato de `av_read_frame`).
- **Margem:** 32 pacotes (folga máxima do decoder no OpenCV 4.10) + (`motion_start_frame_count` − 1).
- **Teto sem movimento:** pré-evento + margem + 20 s de pacotes. Isso supõe GOP ≤ 20 s. Acima do teto, descarta GOP mesmo abaixo do mínimo.
- **Sem keyframe disponível no `started`:** o evento espera o próximo keyframe para abrir o MP4.
- **Fora de escopo nesta série:** thread separada para escrita, timeout de leitura RTSP e política UDP/TCP.
