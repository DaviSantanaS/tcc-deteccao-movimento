# Notas de decisões e pontos a validar

## `allowFrameDrop`

Estado atual: `src/input/VideoStreamReader.cpp` configura:

```cpp
reader_parameters.allowFrameDrop = false;
reader_parameters.rawMode = true;
reader_parameters.udpSource = true;
```

A aplicação usa essa configuração para analisar os frames entregues ao `read()`
e manter o avanço da fronteira da fila conforme a contagem raw. Os pacotes
originais para remux ficam na `EncodedPacketQueue` da fonte FFmpeg; a gravação
acontece na thread principal, fora do lock da fila. O comentário do `main`
registra que o I/O pode atrasar a leitura RTSP e deixa uma thread de escrita
para uma etapa futura.

### Discussão histórica em aberto

A configuração `allowFrameDrop = true` foi considerada para reduzir o atraso
quando o processamento não acompanha o FPS da fonte. Ela não é a configuração
em uso. Se houver descarte de frames decodificados, o MOG2 deixa de analisá-los,
o que pode afetar movimentos curtos.

Frame analisado e pacote original preservado são dados distintos no código:
`getNextPacket()` guarda o clone do pacote e prepara a saída Annex B para o
NVDEC; `read()` usa `PROP_NUMBER_OF_RAW_PACKAGES_SINCE_LAST_GRAB` para avançar a
fronteira. Essa separação, por si só, não valida o funcionamento com descarte
habilitado; qualquer mudança precisa medir a detecção e a contagem dos pacotes.

A hipótese histórica de que perder poucos frames interfere pouco em eventos
longos continua sem validar eventos rápidos. Permanece em aberto definir a
duração mínima de movimento que se deseja detectar, por exemplo na passagem de
um objeto diante da câmera. Nenhuma mudança de configuração decorre desta nota.

As decisões e limitações registradas na execução das etapas E06 a E11 estão em
[PRE_EVENT.md](PRE_EVENT.md#decisões-e-limitações-da-execução-do-plano-v2).
