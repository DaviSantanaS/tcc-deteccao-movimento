# Notas de decisões e pontos a validar

## `allowFrameDrop`

Status: decisão em aberto.

O `VideoStreamReader` atualmente configura:

```cpp
reader_params.allowFrameDrop = true;
```

A motivação é evitar que uma fonte ao vivo acumule atraso caso a decodificação/processamento não acompanhe o FPS do stream.

### Risco identificado

Se frames decodificados forem descartados, o MOG2 deixa de analisar esses frames. Isso pode fazer o detector perder parte de um movimento e, em casos extremos, um evento curto inteiro.

É importante separar isso da perda dos pacotes codificados usados pelo `EncodedVideoBuffer`: no OpenCV 4.10, o mecanismo de `allowFrameDrop` atua sobre a fila de frames de exibição/decodificação, enquanto os pacotes em `rawMode` são mantidos em uma fila separada. Portanto, `allowFrameDrop = true` não significa automaticamente perder o pacote H.264 que contém um keyframe.

### Hipótese atual

Para eventos de movimento com duração de muitos frames, a perda eventual de poucos frames tende a introduzir apenas uma pequena interferência temporal na detecção, sem necessariamente eliminar o evento. Porém, não devemos assumir que todo evento terá cerca de uma centena de frames: movimentos rápidos podem durar bem menos.

### O que precisa ser testado

- Executar os mesmos vídeos/streams com `allowFrameDrop = true` e `allowFrameDrop = false`.
- Comparar o número de frames processados com o número esperado da fonte.
- Comparar `MOTION_ON` e `MOTION_OFF` entre as duas configurações.
- Incluir movimentos curtos nos testes para verificar se algum evento é perdido.
- Medir se desabilitar o frame drop causa acúmulo de latência no cenário real de execução.

### Critério para decisão

Manter `allowFrameDrop = true` somente se os testes mostrarem que a redução de latência compensa a eventual perda de frames sem comprometer a detecção dos eventos relevantes para o TCC. Caso a integridade temporal dos frames seja mais importante e o hardware consiga acompanhar o stream sem atraso significativo, considerar `allowFrameDrop = false`.
