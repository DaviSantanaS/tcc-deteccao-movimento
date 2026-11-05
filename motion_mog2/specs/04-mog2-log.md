# Spec 04 — Integrar o MOG2 no `main` (só detecção e log)

## Objetivo

Chamar `Mog2MotionDetector::detectMotion` a cada frame lido e registrar início e fim de movimento no log. **A gravação contínua atual não muda.** Esta etapa também fixa a CLI final, para que a spec 05 só troque a lógica de gravação.

## Contexto (código atual)

- `include/detectors/Mog2MotionDetector.hpp`:
  - construtor: `(largura, altura, fps, limiar_percentual, frames_inicio, frames_fim, learning_rate)`;
  - `detectMotion(gpu_mat, decoded_frame_index, cuda_stream)` retorna `MotionState { detected, started, active, ended, foreground_ratio }`.
- O detector ignora os primeiros `round(fps × 1 s)` frames (warmup). O construtor só valida o limiar.
- `started` dispara no frame em que a contagem consecutiva atinge `motion_start_frame_count`, ou seja, o movimento começou `start − 1` frames antes. `ended` dispara após `motion_end_frame_count` frames ociosos.
- `include/config/PreEventConfig.hpp`: `parsePreEventSeconds`, `calculatePreEventFrameCount`.
- `src/main.cpp`: hoje a CLI é `[fonte] [diretorio_saida] [pacotes_por_trecho]`.
- `run_mog2_test.sh` (raiz do repositório) chama o binário com uma ordem antiga: `url limiar inicio fim pre_evento`, sem diretório de saída.

## Escopo permitido

- `src/main.cpp`
- `run_mog2_test.sh`: só a chamada do binário.

## Alterações

### 1. Nova CLI (final)

```
motion_mog2 [fonte] [diretorio_saida] [pre_evento_s] [limiar_%] [inicio_frames] [fim_frames]
Padrões: rtsp://127.0.0.1:8554/video ./recordings 5 1.0 2 3
```

- `pre_evento_s`: `parsePreEventSeconds`. Nesta spec é só lido e mostrado no log (`PRE_EVENT seconds=... packets=...`, com `calculatePreEventFrameCount`).
- `limiar_%`: `std::stod` com conferência de consumo total da string. O intervalo é validado pelo construtor do detector.
- `inicio_frames` e `fim_frames`: inteiros ≥ 1, com validação no estilo da validação atual do alvo de pacotes. Rejeite 0, negativo, texto e valores acima de `int`.
- `--help`/`-h` e mais de 6 argumentos: mesma política de hoje. Atualize o texto de uso.
- `learning_rate`: constante `-1.0` (aprendizado automático do OpenCV), com comentário.
- O alvo da gravação contínua deixa de ser argumento. Use a constante local `continuous_recording_packet_target = 1000`, com o comentário `// Temporario: removido na spec 05 (gravacao por evento).`

### 2. Detecção

- Construir o detector depois do `VideoStreamReader`, com `getWidth()`, `getHeight()` e `getFramesPerSecond()`.
- Logar `MOG2 warmup_frames=<getWarmupFrameCount()>`.
- No laço, **somente quando um frame novo foi lido** (`new_decoded_frame_required` verdadeiro) e depois de `cuda_stream.waitForCompletion()`:
  - chamar `detectMotion(decoded_frame.decoded_frame_gpu, decoded_frame.decoded_frame_index, cuda_stream)`;
  - em `started`: `MOTION_STARTED frame=<índice>`;
  - em `ended`: `MOTION_ENDED frame=<índice>`.
- Não chamar o detector em iterações que só processam sobras do vetor pendente.
- Não usar o resultado da detecção para nada além do log.

### 3. Script

Atualize a chamada do binário em `run_mog2_test.sh` para a nova ordem, passando `"${RUN_DIR}/recordings"` como diretório de saída. Mantenha os nomes das variáveis do script.

## Aceitação

**Agente:** compilar, se possível. Rodar `bash -n run_mog2_test.sh`.

**Davi (GPU):**
- `motion_mog2 --help` mostra a CLI nova.
- Argumentos inválidos (`0`, `-1`, `abc` em `inicio_frames`; limiar `0`) encerram com erro e código ≠ 0.
- Com um vídeo que tenha movimento conhecido: os logs `MOTION_STARTED`/`MOTION_ENDED` aparecem perto dos instantes esperados, e nenhum aparece antes de `warmup_frames`.
- Paridade da gravação contínua: mesmo procedimento `framemd5` da spec 03, comparando com o alvo 1000. Deve dar `PARIDADE_OK`.

## Commit

`feat: integra MOG2 ao main com log de inicio e fim de movimento`

## Relatório

- Diff resumido.
- Exemplo do texto de `--help`.
- O que foi executado e o que fica para o Davi.
