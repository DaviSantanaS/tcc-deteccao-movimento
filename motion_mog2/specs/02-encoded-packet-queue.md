# Spec 02 — Classe `EncodedPacketQueue` e testes unitários

## Objetivo

Criar uma fila thread-safe de `AvPacketPtr` com uma **fronteira**: o número de pacotes da frente já contabilizados pelo decoder (soma dos N de `PROP_NUMBER_OF_RAW_PACKAGES_SINCE_LAST_GRAB`). Ela substituirá, nas próximas specs, a deque da fonte, o vetor pendente do `main` e o `EncodedVideoBuffer`. **Nesta spec a classe não é usada por nenhum código de produção.**

## Modelo

```
frente                       fronteira               fim
[ região contabilizada      ][ região à frente        ]
  counted_packet_count_        só o produtor acrescenta
```

- O produtor (thread interna do OpenCV) só chama `pushPacket`.
- O consumidor (thread do `main`) avança a fronteira, apara e retira pacotes **somente da região contabilizada**. A exceção é `takeAllPackets`, usada no encerramento.
- A fronteira é um contador, nunca um iterador: `push_back` em `std::deque` invalida iteradores.
- "Keyframe" = `(packet->flags & AV_PKT_FLAG_KEY) != 0`.
- "GOP" = de um keyframe até o pacote anterior ao próximo keyframe.

## Escopo permitido

- Criar `motion_mog2/include/buffers/EncodedPacketQueue.hpp`
- Criar `motion_mog2/src/buffers/EncodedPacketQueue.cpp`
- Criar `motion_mog2/tests/encoded_packet_queue_tests.cpp`
- `motion_mog2/CMakeLists.txt`: somente o novo alvo de teste.

Dependência autorizada: `find_package(Threads REQUIRED)` e `Threads::Threads`, só no alvo de teste.

## Interface

```cpp
class EncodedPacketQueue {
public:
    void pushPacket(AvPacketPtr encoded_packet);
    void advanceCountedPackets(std::size_t packet_count);

    std::size_t getCountedPacketCount() const;
    std::size_t getQueuedPacketCount() const;

    std::size_t discardCountedPacketsBeforeFirstKeyframe();
    std::size_t trimCountedPackets(
        std::size_t minimum_kept_packet_count,
        std::size_t maximum_counted_packet_count
    );

    [[nodiscard]] std::vector<AvPacketPtr> takeCountedPacketsFromLookback(
        std::size_t lookback_packet_count
    );
    [[nodiscard]] std::vector<AvPacketPtr> takeCountedPackets();
    [[nodiscard]] std::vector<AvPacketPtr> takeAllPackets();

private:
    mutable std::mutex queue_mutex_;
    std::deque<AvPacketPtr> encoded_packets_;
    std::size_t counted_packet_count_ = 0;
};
```

- Não copiável e não movível (`= delete` nos quatro), como a fonte atual.
- Todo método público trava `queue_mutex_` uma única vez durante toda a operação.
- Nenhum método escreve arquivo nem chama a API de saída do FFmpeg. A escrita do MP4 acontece no chamador, fora do lock.
- Métodos privados auxiliares (por exemplo, "achar o próximo keyframe na região contabilizada a partir da posição i") podem existir. Devem assumir o lock já adquirido e não travá-lo de novo.

## Comportamento

| Método | Regra |
|---|---|
| `pushPacket` | Lança `std::invalid_argument` se o ponteiro for nulo. Faz `push_back`. Não altera a fronteira. |
| `advanceCountedPackets(n)` | Se `n > tamanho − contabilizados`, lança `std::runtime_error` com a mensagem `"Fila de AVPacket dessincronizada: esperados <n>, disponiveis <tamanho − contabilizados>."` e não muda o estado. Caso contrário, soma `n`. `n == 0` não faz nada. |
| `discardCountedPacketsBeforeFirstKeyframe` | Enquanto houver contabilizados e a frente não for keyframe: `pop_front` e decrementa a fronteira. Retorna quantos descartou. |
| `trimCountedPackets(min, max)` | Ver abaixo. Retorna o total descartado. |
| `takeCountedPacketsFromLookback(l)` | Ver abaixo. |
| `takeCountedPackets` | Move os contabilizados, em ordem, para o vetor retornado. Fronteira = 0. |
| `takeAllPackets` | Move todos os pacotes (as duas regiões), em ordem. Fronteira = 0. |

### `trimCountedPackets(min, max)`

Lança `std::invalid_argument` se `max == 0` ou `max < min`, antes de alterar qualquer estado.

1. Chama a lógica de `discardCountedPacketsBeforeFirstKeyframe`.
2. **Corte normal:** enquanto existir keyframe na posição `p` com `1 ≤ p < contabilizados`, sendo `p` o primeiro keyframe depois da frente, e `contabilizados − p ≥ min`: descarta as posições `[0, p)`.
3. **Teto:** enquanto `contabilizados > max`:
   - se existir keyframe `p` com `1 ≤ p < contabilizados`, descarta `[0, p)`, mesmo que fique abaixo de `min`;
   - se não existir, descarta todos os contabilizados e para.

Pós-condições:
- a região contabilizada está vazia ou começa em keyframe;
- contabilizados ≤ `max`;
- a região à frente nunca é alterada.

### `takeCountedPacketsFromLookback(l)`

1. `alvo = contabilizados > l ? contabilizados − l : 0`.
2. `k` = o último keyframe com posição ≤ `alvo` na região contabilizada. Se não houver, o primeiro keyframe com posição > `alvo` nessa região.
3. Se não houver keyframe nenhum na região contabilizada: descarta todos os contabilizados e retorna vetor vazio.
4. Caso contrário: descarta `[0, k)`, move `[k, contabilizados)` para o retorno e faz fronteira = 0. O primeiro pacote retornado é sempre keyframe.

## Testes (`tests/encoded_packet_queue_tests.cpp`)

Siga o estilo de `tests/encoded_video_buffer_tests.cpp`:
- `main` com `try/catch`;
- falha com `throw std::runtime_error("...")` e retorno 1;
- uma linha `PASS: ...` por grupo.

Pacote sintético: `allocateAvPacket()` + `av_new_packet(packet.get(), 1)`, com `pts = dts = id`, e `AV_PKT_FLAG_KEY` quando for keyframe. Para conferir a ordem, use o `pts` como identificador. Notação abaixo: `K` = keyframe, `p` = não keyframe, índices = `pts`.

1. `pushPacket(nullptr)` lança `std::invalid_argument`.
2. `advanceCountedPackets(0)` não faz nada. Avançar além do disponível lança `std::runtime_error` e não altera as contagens.
3. Avanços sucessivos somam, e `getCountedPacketCount() ≤ getQueuedPacketCount()`.
4. `[p p K p]`, todos contabilizados: `discard...` retorna 2 e a frente passa a ser o `K`.
5. GOP de 4, `min = 6`, `max = 100`:
   - 10 pacotes contabilizados (`K p p p K p p p K p`) → após o trim, ficam 6, com frente `pts 4`;
   - empurra e contabiliza mais 2 → trim não descarta nada (8 pacotes).
6. Contabilizados `[K p p]` e, à frente, `[K p]` → `trim(0, 100)` não descarta nada. A região à frente fica intacta.
7. Teto com keyframe: GOP de 10, 20 contabilizados, `min = 12`, `max = 15` → descarta o primeiro GOP e ficam 10.
8. Teto sem keyframe: contabilizados `K` + 20 `p`, mais 3 pacotes à frente, `trim(5, 10)` → contabilizados = 0 e restam os 3 à frente.
9. Lookback: GOP de 4, 12 contabilizados, `l = 5` → alvo 7 → retorna `pts 4..11` (8 pacotes), contabilizados = 0, e os pacotes à frente continuam na fila.
10. `l` maior que o número de contabilizados → retorna desde `pts 0`.
11. Contabilizados `[p p p K p]`, `l = 4` → alvo 1 → retorna `pts 3, 4`.
12. Contabilizados sem keyframe → retorna vazio e contabilizados = 0.
13. `takeCountedPackets` retorna exatamente os contabilizados, em ordem, e mantém os da frente.
14. `takeAllPackets` retorna tudo, em ordem, e a fila fica vazia.
15. `trim` com `max == 0` ou `max < min` lança `std::invalid_argument` sem alterar o estado.
16. **Concorrência:**
    - Uma `std::thread` produtora empurra 100 000 pacotes (`pts` 0 a 99 999, keyframe a cada 30).
    - O consumidor, em laço, avança `min(disponível, 1 + i % 7)`. A cada 5 iterações chama `trimCountedPackets(60, 200)`. A cada 11 iterações chama `takeCountedPackets()`.
    - Ao final: `join` e depois `takeAllPackets()`.
    - Verificar que a concatenação dos vetores retornados tem `pts` estritamente crescente, e que retornados + descartados == 100 000.
    - Sem `sleep`. O teste deve ser determinístico no resultado.

CMake: alvo `encoded_packet_queue_tests` com o teste, `src/buffers/EncodedPacketQueue.cpp` e `src/ffmpeg/AvPacket.cpp`. Linkar `PkgConfig::LIBAVCODEC` e `Threads::Threads`. Adicionar o `add_test`.

## Aceitação

O `CMakeLists.txt` exige OpenCV mesmo para este teste. Se o ambiente não tiver OpenCV, compile direto, a partir de `motion_mog2/`:

```bash
g++ -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -I include \
  tests/encoded_packet_queue_tests.cpp src/buffers/EncodedPacketQueue.cpp src/ffmpeg/AvPacket.cpp \
  $(pkg-config --cflags --libs libavcodec) -pthread -o /tmp/encoded_packet_queue_tests && /tmp/encoded_packet_queue_tests
```

Repita o teste de concorrência com `-fsanitize=thread`, em outro build sem ASan:

```bash
g++ -std=c++17 -fsanitize=thread -g -I include \
  tests/encoded_packet_queue_tests.cpp src/buffers/EncodedPacketQueue.cpp src/ffmpeg/AvPacket.cpp \
  $(pkg-config --cflags --libs libavcodec) -pthread -o /tmp/encoded_packet_queue_tests_tsan && /tmp/encoded_packet_queue_tests_tsan
```

Pronto quando os dois executam com todos os `PASS` e sem relatório de sanitizer.

## Commit

`feat: adiciona EncodedPacketQueue com fronteira e corte por GOP`

## Relatório

- Arquivos criados.
- Saída completa dos dois comandos.
- Qualquer caso da tabela que você tenha interpretado de forma diferente do texto.
