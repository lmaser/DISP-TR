# DISP-TR: auditoria de Reverse Dispersion

**Estado:** investigación y diseño, sin cambios en el DSP de producción
**Fecha:** 2026-08-08
**Alcance:** determinar si puede añadirse una inversión profesional de la dispersión sin cachear granos ni invertir bloques de convolución de forma frágil.

## 1. Resultado ejecutivo

Sí, es posible ofrecer un modo profesional sin granos, pero hay que distinguir dos productos DSP diferentes:

1. **Reverse dispersion / dispersión invertida:** una respuesta causal que invierte el contorno de retardos por frecuencia. Puede realizarse con una nueva red de allpass estable y de bajo coste. Es la opción recomendada para DISP-TR.
2. **Reverse temporal exacto:** la inversión de la respuesta impulsional completa. No es un simple cambio de orden de filtros: para un allpass causal, la respuesta invertida es esencialmente la realización anticausal/inversa y necesita muestras futuras, latencia fija o una aproximación FIR/FFT.

La primera opción no necesita granos, convolución ni PDC adicional si se acepta que es una inversión del contorno de dispersión y no una reproducción sample-accurate de la respuesta impulsional invertida. La segunda opción es viable, pero debe ser un modo avanzado con latencia/PDC, memoria y complejidad claramente asumidas.

## 2. Estado real del código auditado

En la copia auditada de `DISP-TR/Source` no existe actualmente ningún parámetro o implementación llamado `reverse`, ni caché de `grain`. El término reverse corresponde, por tanto, a un intento anterior o a otra rama que no está presente en este workspace. No se debe presentar ese intento como comportamiento actual del plugin.

La implementación presente es:

- una cascada de `TR::DSP::FirstOrderAllPass` en `TR-Shared/SimpleDSP/TRPhaseQuadrature.h`;
- coeficiente de primer orden calculado desde la frecuencia mediante `tan(pi*f/sr)`;
- frecuencias de etapa distribuidas alrededor de `Freq` en `PluginProcessor.cpp:updateCoefficientsInto`;
- caminos adicionales de series, jitter, alternancia, modulación y feedback.

La red allpass conserva aproximadamente la magnitud unitaria antes de las mezclas, filtros y feedback. Su efecto principal es modificar fase y retardo de grupo.

## 3. Qué significa invertir la dispersión

Para una respuesta lineal e invariable en el tiempo, el orden de los factores de una cascada conmuta:

```text
H(z) = H1(z) * H2(z) * ... * HN(z)
```

Por ello, ejecutar las mismas etapas en orden inverso **no** invierte la respuesta. El resultado teórico es el mismo, aparte de diferencias numéricas y de estado. Tampoco basta con invertir la lista de frecuencias.

La inversión temporal de una respuesta causal `h[n]` produce una respuesta que, en general, empieza en el futuro. Para un allpass, la inversión de fase se relaciona con `H(z^-1)` o con la fase conjugada, cuya realización estable requiere una versión anticausal o un retardo adicional. Un filtro causal no puede producir de forma exacta una respuesta impulsional que dependa de muestras futuras sin introducir lookahead.

La condición de causalidad también explica por qué el retardo de grupo de un allpass causal no puede reflejarse libremente alrededor de cero. El diseño profesional consiste en añadir un retardo constante suficiente y sintetizar una curva de retardo de grupo positiva, no en negar directamente todos los retardos.

## 4. Alternativas evaluadas

| Alternativa | Resultado | Coste/riesgo | Decisión |
|---|---|---|---|
| Invertir el orden de las etapas | No invierte la dispersión | Puede aparentar funcionar por estados/transitorios | Rechazar |
| Invertir muestras por granos | Produce un efecto de reverse reconocible | Fronteras de bloque, transitorios, tamaño de grano, automatización y discontinuidades | Rechazar como implementación principal |
| Invertir bloques de convolución sin arquitectura OLA/OLS completa | Puede funcionar en casos aislados | Inconsistencia entre bloques y cambios de parámetros | Rechazar |
| Red allpass causal con curva de retardo complementaria | Invierte el contorno perceptual de la dispersión | Es una aproximación de grupo de retardo, no reverse temporal exacto | **Recomendada** |
| FIR de fase conjugada con lookahead y convolución particionada | Mejor aproximación al reverse impulsional | Latencia/PDC, CPU, memoria, rediseño y crossfade de IR | Opcional, modo avanzado |
| Procesamiento anticausal por bloques con lookahead | Puede aproximar la inversión exacta | Latencia alta y complejidad de streaming | No recomendado por defecto |

## 5. Propuesta profesional para DISP-TR

### 5.1 Modo recomendado: Reverse Dispersion causal

Definir una curva de retardo objetivo:

```text
tauReverse(f) = D - tauForward(f)
```

`D` debe ser mayor o igual que el máximo retardo del contorno forward, con margen de seguridad. Después se aproxima esa curva mediante una red de allpass estable. La red puede conservar la infraestructura actual de estados y procesamiento en tiempo real, pero el cálculo de coeficientes debe ser específico para la curva invertida; no debe limitarse a recorrer etapas hacia atrás.

Hay dos niveles posibles:

- **Versión conservadora:** mantener el número de etapas y mapear de forma simétrica la distribución logarítmica actual. Es sencilla y barata, pero la inversión será solo aproximada.
- **Versión recomendada:** añadir un sintetizador offline/de baja frecuencia de coeficientes basado en el ajuste de retardo de grupo, con límite de orden, error y estabilidad. Se recalcula al cambiar parámetros estabilizados y se aplica mediante el crossfade de red ya disponible.

El diseño de allpass basado en una curva de retardo de grupo es una técnica establecida en audio; Abel y Smith describen la síntesis robusta de redes allpass de dispersión y el uso de retardo puro para hacer realizable la curva objetivo.

### 5.2 Modo opcional: reverse impulsional con latencia

Si se exige que el impulso completo sea lo más parecido posible al invertido, la arquitectura adecuada es:

1. medir o generar la respuesta compleja de la red forward;
2. construir una fase conjugada y añadir un retardo lineal `D` para hacerla causal;
3. sintetizar una FIR regularizada y limitada en longitud;
4. procesar con overlap-save/overlap-add o convolución particionada;
5. cruzar IR nuevas sin cambiar la respuesta entre bloques;
6. declarar el PDC correspondiente.

Esta opción ya no es una extensión pequeña del motor actual. Debe reservarse para un modo explícito, con límites de longitud/CPU y una política clara de automatización.

## 6. Feedback, jitter y automatización

La semántica debe fijarse antes de programar:

- `Reverse` debe afectar a la red de dispersión, no invertir silenciosamente filtros, mezcla, paneo o limiter.
- Con `feedback`, el modo reverse puede conservar el feedback en la misma topología, pero su sonido no será el inverso temporal de la red completa. Debe documentarse así.
- Con `jitter`, la inversión debe aplicarse a cada conjunto de coeficientes generado, no a la señal ya procesada. Los coeficientes deben estar limitados y suavizados como en el camino actual.
- Cambios de `Reverse` y de parámetros estructurales deben usar el mecanismo de actualización/crossfade existente. No se permiten asignaciones ni buffers temporales en el hilo de audio.
- Si se implementa solo la red allpass causal, el PDC permanece en cero. Si se implementa el modo FIR/lookahead, el PDC debe declararse siempre y de forma independiente del estado de bypass/mix según la política global de los plugins.

## 7. Fases propuestas

### Fase A: contrato y referencia

- Decidir si el control se llama `Reverse Dispersion` para evitar prometer reverse temporal exacto.
- Congelar la definición de forward: curva de retardo, número de etapas, jitter, feedback y `mix`.
- Generar una referencia offline del impulso y del retardo de grupo a 44.1, 48 y 96 kHz.

### Fase B: prototipo causal

- Crear un diseñador aislado de coeficientes, sin tocar todavía el procesador principal.
- Comparar forward y reverse por error de retardo de grupo, magnitud, estabilidad y respuesta a transitorios.
- Probar con el tamaño de bloque real del plugin y automatización de parámetros.

### Fase C: integración

- Integrar el banco reverse con doble estado y crossfade de red.
- Añadir pruebas de determinismo, ausencia de asignaciones RT y equivalencia entre tamaños de bloque.
- Verificar especialmente `jitter`, `alt`, `series` y `feedback`.

### Fase D: decisión sobre modo exacto

- Solo si la versión causal no alcanza el resultado auditivo deseado, prototipar la variante FIR/lookahead.
- Medir CPU, memoria, latencia y PDC en DAW antes de decidir si entra en producto.

## 8. Criterios de aceptación

- Sin inestabilidad ni coeficientes fuera de `|a| < 1`.
- Magnitud aproximadamente unitaria en el banco allpass antes de feedback, filtros y mezcla.
- Curva de retardo reverse con tendencia opuesta a forward, sin zonas no causales.
- Igual resultado dentro de tolerancia al cambiar el tamaño de bloque.
- Reset determinista y sin colas espurias al activar/desactivar el modo.
- Sin asignaciones, locks ni copias de granos en `processBlock`.
- Pruebas a 44.1, 48, 88.2 y 96 kHz; 48 kHz es la referencia operativa del proyecto.
- Si existe lookahead: PDC correcto, latencia estable y crossfade de cambios sin clics.

## 9. Decisión recomendada

Implementar primero **Reverse Dispersion causal mediante curva de retardo complementaria**. No implementar reverse por granos, no invertir el orden de stages y no introducir FFT/convolución hasta medir que la versión causal no cubre el objetivo sonoro.

La inversión temporal exacta debe considerarse otra funcionalidad, con el nombre y la latencia explícitos. Así se mantiene DISP-TR ligero, determinista y coherente con el flujo profesional de un plugin en tiempo real.

## 10. Primera ejecución del arnés

Se añadió `DISP-TR/Tests/disp_reverse_audit.py` y se ejecutó a 44.1, 48 y 96 kHz.

Resultados confirmados:

- invertir el orden de stages produce una diferencia máxima de aproximadamente `6.7e-16`: no es una inversión funcional;
- la magnitud del banco allpass se mantiene en torno a error numérico;
- el primer ajuste automático de una red de 16 allpass por frecuencias no alcanza todavía la curva complementaria: el error RMS queda entre aproximadamente 126 y 276 muestras según sample rate;
- por tanto, este primer diseñador **no está listo para integración**.

Este fallo es deliberado y útil: evita integrar una red que conserve la magnitud pero no invierta suficientemente el retardo perceptual. La siguiente iteración debe mejorar el sintetizador de retardo de grupo, posiblemente aumentando el orden efectivo, usando una parametrización más adecuada o incorporando una síntesis Abel-Smith acotada, antes de tocar `PluginProcessor`.

## 11. Segunda iteración: secciones allpass de segundo orden

La primera parametrización estaba limitada por usar exclusivamente allpass de primer orden definidos por una frecuencia de transición. A 96 kHz, esa parametrización concentra una parte importante de la respuesta fuera de la banda audible.

El arnés se amplió con un prototipo de secciones allpass de segundo orden:

- ocho secciones para el caso de 16 stages;
- resonancias distribuidas entre 12 kHz y el límite audible configurado;
- radio de polos `0.9`, siempre menor que uno;
- sin cambio de magnitud ideal, al tratarse de secciones allpass.

Resultados a 44.1, 48 y 96 kHz:

- el retardo medio de la banda alta supera claramente al de la banda grave;
- el criterio de inversión de contorno pasa en los tres sample rates;
- la magnitud se mantiene unitaria dentro del error numérico;
- todas las secciones son estables.

Esta candidata demuestra que el efecto reverse es viable sin granos, pero todavía no es el diseño final. La siguiente fase debe convertir la red fija en una parametrización controlada por `Freq`, `Shape`, `Amount` y `Stages`, y debe comprobar que el radio no genera colas excesivas ni una resonancia audible artificial.

## 12. Tercera iteración: parametrización y streaming

Se añadió al arnés una candidata parametrizada:

```text
fReverse[i] = maxAudible - fForward[2*i]
```

Las etapas se agrupan por pares en secciones allpass de segundo orden. El radio se mantiene acotado por debajo de uno y se deriva de `Shape`, de forma que el control conserva influencia sobre la concentración del efecto sin permitir polos inestables.

Casos probados:

- 44.1, 48 y 96 kHz;
- `Freq` bajo y medio;
- `Shape` bajo y medio;
- 16 stages, reducidos a 8 secciones de segundo orden;
- bloques de 16, 32, 64, 128, 256, 512 y 1024 muestras.

La candidata parametrizada pasa actualmente los checks analíticos del arnés:

- mayor retardo medio en la banda alta que en la banda grave;
- magnitud unitaria dentro del error numérico;
- radios estables;
- salida idéntica entre todos los tamaños de bloque probados.

Esto no autoriza todavía la integración. Falta validar la cola temporal y el carácter audible con impulsos y música, ajustar la relación entre `Amount` y número de secciones, y comprobar `jitter`, `alt`, `series` y `feedback`.

## 13. Primera validación de combinaciones DSP

El arnés incluye ahora variantes de streaming con:

- `series` 1 y 2;
- `feedback` 0, 0.25, 0.5 y 0.75;
- jitter determinista actualizado por bloque;
- bloques de 128 muestras;
- impulsos de 8192 muestras.

En 44.1, 48 y 96 kHz todas las variantes probadas son finitas y permanecen acotadas. A 48 kHz, el peor caso (`series=2`, `feedback=0.75`) alcanza aproximadamente 0.51 de amplitud máxima y tarda unos 6314 samples en caer a -80 dB, aproximadamente 131 ms. No es una inestabilidad, pero confirma que `Reverse + Feedback` puede crear colas largas y debe evaluarse auditivamente.

El jitter probado cambia los coeficientes de forma determinista por bloque y mantiene la salida finita y acotada. Todavía falta probar el jitter sample-rate o audio-rate real del motor, además de `ALT`, modulación, filtros y cambio de parámetros durante una cola activa.

## 14. ALT, modulación, filtros y automatización

El arnés se amplió con una combinación exigente de `series=2`, `feedback=0.5`, jitter, alternancia y modulación de coeficientes. Después se aplicó un filtro paso alto a 20 Hz y paso bajo a 18 kHz.

También se simuló un cambio de parámetros en el sample 2048, con un crossfade de 256 samples entre dos bancos reverse.

Resultados a 44.1, 48 y 96 kHz:

- la combinación `ALT + modulación + filtros` permanece finita y acotada;
- no aparecen picos anómalos en la cola;
- el crossfade permanece finito;
- el criterio de ausencia de pico grande durante la automatización pasa.

En el caso de 48 kHz, el salto máximo dentro de la transición fue aproximadamente `0.00064`, frente a un salto de referencia del `99%` de aproximadamente `0.0735`. Esto es una validación del modelo offline; todavía debe repetirse con dos estados DSP reales dentro de un proceso JUCE para validar completamente la transición.

## 15. Probe C++ con estados reales

Se añadió un prototipo C++ independiente en:

- `DISP-TR/Tests/DispReverseStateProbe.cpp`
- `DISP-TR/Tests/DispReverseStateProbe.vcxproj`

El probe implementa estados persistentes de secciones allpass de segundo orden, doble banco para crossfade y procesamiento por bloques sin asignaciones dentro de `processBlock` del prototipo.

Compilación Release:

```text
Build succeeded
0 Warning(s)
0 Error(s)
```

Ejecución:

```text
sample_rate=44100 block_invariance=pass cpu_ms=7.6731
sample_rate=48000 block_invariance=pass cpu_ms=7.6957
sample_rate=96000 block_invariance=pass cpu_ms=7.711
DISP reverse state probe passed
```

El benchmark procesa 512.000 muestras por sample rate y confirma coste bajo para esta red aislada. El resultado valida el estado y el crossfade del prototipo, pero todavía no es una medición de `PluginProcessor` completo: quedan por medir routing, filtros, feedback global, limiter y el coste estéreo real dentro del plugin.

## 16. Primera integración en PluginProcessor

Se integró una ruta aislada mediante:

- `DISP-TR/Source/ReverseDispersionRuntime.h`;
- parámetro persistente `reverse` / `Reverse Dispersion`;
- bancos internos de segundo orden para series y crossfade;
- reset determinista al activar o desactivar el modo.

El probe específico del plugin es:

- `DISP-TR/Tests/DispReversePluginProbe.cpp`;
- `DISP-TR/Tests/DispReversePluginProbe.vcxproj`.

Resultados Release del `PluginProcessor` real:

```text
sample_rate=44100 block_delta=0 forward_delta=1.02384 peak=1.01928
sample_rate=48000 block_delta=0 forward_delta=1.005 peak=0.996528
sample_rate=96000 block_delta=0 forward_delta=0.942277 peak=0.45142
DISP reverse plugin probe passed
```

También pasa la persistencia del parámetro en un round-trip de preset.

### Limitaciones antes de release

La primera integración es deliberadamente conservadora y todavía no debe publicarse:

- sidechain no modifica aún la curva reverse por muestra;
- jitter nativo no se aplica todavía al banco de segundo orden;
- el modo DUAL comparte por ahora el banco reverse entre canales;
- falta comparar routing, filtros, limiter y CPU del plugin completo contra forward;
- falta escucha con impulsos, transitorios y material musical.

El forward permanece sin cambios. Estas limitaciones son de cobertura de la nueva ruta, no regresiones observadas en el motor existente.

## Referencias técnicas

- [Stanford Exploration Project: group delay of all-pass filters](https://sep.stanford.edu/sep/pvi/spec/paper_html/node22.html)
- [Abel and Smith, Robust design of very high-order allpass dispersion filters, DAFx 2006](https://www.dafx.de/paper-archive/2006/papers/p_013.pdf)
- [DAFx 2006 paper archive entry](https://www.dafx.de/paper-archive/details/FxpIAYIVGkUKoqVYzYCHQg)
- [Phase equalization using hybrid causal and noncausal IIR allpass filters](https://doi.org/10.1016/j.dsp.2025.105455)
- [Frequency-dependent Schroeder allpass reverberation](https://www.mdpi.com/2076-3417/10/1/187)
- [Live convolution with time-varying filters](https://www.mdpi.com/2076-3417/8/1/103)
