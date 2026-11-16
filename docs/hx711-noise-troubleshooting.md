# HX711: diagnóstico de ruido y estabilidad

## Qué muestran las lecturas actuales

Estas lecturas no son una variación normal de pocos gramos:

```text
raw: 646227
raw: 646846
raw: 731459
raw: 729345
```

El salto de aproximadamente `85 000` cuentas ocurre entre dos lecturas. El
factor de calibración no puede causar por sí solo un salto en `raw`; el factor
solo convierte las cuentas en gramos. Por eso primero hay que estabilizar la
señal eléctrica y mecánica, y después volver a calibrar.

La caída gradual posterior de `731459` a `717169` también puede indicar que la
estructura se está acomodando, que la celda está siendo forzada lateralmente o
que la alimentación se recupera después de una perturbación.

## Hardware recomendado

### Capacitores

Coloca los capacitores físicamente cerca del módulo HX711:

```text
HX711 VCC ---||--- GND    100 nF cerámico
HX711 VCC ---||--- GND    10 uF electrolítico o tantalio
```

El capacitor de `100 nF` filtra ruido de alta frecuencia. El de `10 uF`
ayuda con cambios de corriente y ruido de baja frecuencia. Respeta la
polaridad del capacitor electrolítico.

El servo debe tener su propia alimentación de 5 V. Coloca también un capacitor
de `470–1000 uF` cerca del servo. Une el GND de la fuente del servo con el GND
del ESP32 y del HX711, pero no alimentes el servo desde el regulador de 3.3 V
del ESP32.

### Alimentación

Para una primera prueba aislada:

1. Desconecta temporalmente el servo.
2. Desconecta temporalmente Wi-Fi si es posible o mantén el ESP32 lejos del
   HX711.
3. Alimenta el HX711 con una fuente regulada y estable.
4. Verifica con multímetro el voltaje directamente en VCC y GND del HX711.

El HX711 acepta alimentación dentro de su rango, pero la lógica `DOUT` debe
ser segura para el ESP32-C3. Si el módulo está alimentado a 5 V y su salida
sube a 5 V, no conectes `DOUT` directamente al GPIO7; usa un divisor o
conversor de nivel. Alimentar el módulo a 3.3 V evita ese riesgo en muchos
módulos, aunque la estabilidad concreta depende de la placa utilizada.

### Cableado de la celda

Comprueba continuidad y apriete de:

```text
Celda E+ -> HX711 E+
Celda E- -> HX711 E-
Celda A+ -> HX711 A+
Celda A- -> HX711 A-
```

Los colores no son universales. Si el fabricante usa otro código de colores,
usa su hoja técnica y no el color como única referencia.

Mantén los cuatro cables de la celda cortos, separados del cable del servo y
trenzados por pares cuando sea posible. No dejes una protoboard floja entre la
celda y el HX711 durante la prueba.

### Montaje mecánico

La plataforma debe cargar la celda únicamente en la dirección para la que fue
construida. Revisa que:

- La celda esté firmemente atornillada.
- La plataforma no roce la base, tornillos, cables ni paredes.
- El peso se coloque siempre en el centro.
- No haya fuerza lateral ni torsión.
- El servo no toque ni mueva la plataforma.

Una plataforma que roza puede producir cambios grandes aunque el peso no
cambie.

## Configuración de baja variación

El HX711 debe trabajar a `10 SPS` para obtener menos ruido. Revisa el pin
`RATE` de tu módulo: normalmente debe estar en nivel bajo para 10 muestras por
segundo. Si está en nivel alto, puede operar a 80 SPS y presentar más ruido.

La librería ya promedia varias muestras, pero para diagnosticar estabilidad
conviene esperar entre 30 y 60 segundos después de encender y no mover nada.
El promedio no corrige una mala conexión, una plataforma que roza o una fuente
ruidosa.

## Procedimiento de prueba

1. Desconecta el servo o no lo muevas.
2. Deja la plataforma vacía y sin contacto.
3. Enciende el ESP32 y espera 60 segundos.
4. Ejecuta:

   ```text
   hx711 reset
   hx711 tare
   hx711 status
   ```

5. Observa `raw` durante al menos 30 segundos.
6. Si `raw` salta decenas de miles de cuentas, detén la calibración y revisa
   alimentación, cables y montaje.
7. Coloca un peso conocido, por ejemplo `241 g`, sin tocar la plataforma.
8. Ejecuta:

   ```text
   hx711 calibrate 241
   hx711 read
   ```

9. Retira el peso, espera a que vuelva cerca de cero y repite la colocación.
   El resultado debe regresar aproximadamente al mismo valor cada vez.

No recalibres mientras la lectura todavía está subiendo o bajando. El factor
solo es válido para una señal estable.

## Fuentes consultadas

- [HX711 datasheet / SparkFun](https://www.sparkfun.com/datasheets/Sensors/ForceFlex/hx711_english.pdf):
  referencia de alimentación, desacoplo y selección de 10 SPS frente a
  80 SPS.
- [SparkFun HX711 Load Cell Amplifier](https://www.sparkfun.com/sparkfun-load-cell-amplifier-hx711.html):
  esquema de referencia con desacoplo de alimentación.
- [Random Nerd Tutorials: ESP32 con celda y HX711](https://randomnerdtutorials.com/esp32-load-cell-hx711/):
  referencia práctica de conexión y calibración.
- [Manual de módulo HX711 y celda](https://shillehtek.com/blogs/shillehtek-product-manuals/hx711-weighing-pressure-sensor-module-large-presoldered-manual):
  referencia de cableado de una celda de cuatro hilos.

