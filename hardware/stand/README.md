# Clawdmeter 3D Stand

Soporte impreso en 3D para el Waveshare ESP32-C6-Touch-AMOLED-2.16 (pantalla cuadrada AMOLED 480×480).

## Características

- **Dos piezas**: cuna (cradle) + pata (leg kickstand)
- **Tilt adjustable**: bisagra con muescas cada 10° + bloqueo con tornillo M3
- **Plegable**: la pata se pliega para almacenaje plano
- **Snap-fit**: el dispositivo se inserta sin tornillos (retención elástica)
- **Cable libre**: USB-C cuelga desde la parte inferior

---

## Hardware Necesario

| Ítem | Cantidad |
|------|----------|
| Tornillo M3 × 20mm | 1 |
| Tuerca M3 hexagonal | 1 |
| Pad de goma antideslizante 10×10mm | 1 |

---

## Exportar desde OpenSCAD

### Opción A: Interfaz Gráfica (GUI)

1. Abre `clawdmeter_stand.scad` en OpenSCAD
2. **Exportar cradle:**
   - En la parte superior, cambia `RENDER = "assembly";` a `RENDER = "cradle";`
   - Presiona F5 (Preview) para ver la pieza
   - Menu **File → Export as STL** → `clawdmeter_cradle.stl`
3. **Exportar leg:**
   - Cambia `RENDER = "cradle";` a `RENDER = "leg";`
   - Presiona F5
   - Menu **File → Export as STL** → `clawdmeter_leg.stl`

### Opción B: Línea de Comandos (CLI)

```bash
# Exportar cradle
openscad -D 'RENDER="cradle"' -o clawdmeter_cradle.stl clawdmeter_stand.scad

# Exportar leg
openscad -D 'RENDER="leg"' -o clawdmeter_leg.stl clawdmeter_stand.scad
```

---

## Parámetros OpenSCAD

Todos los parámetros están en la cabecera del archivo (líneas 13–45) y pueden ajustarse:

```openscad
DEVICE_W       = 46.0    // ancho PCB
DEVICE_H       = 46.0    // alto PCB
DEVICE_D       = 8.3     // grosor PCB
CRADLE_CLEAR   = 0.3     // holgura snap-fit
WALL           = 2.5     // espesor paredes cuna
HINGE_R        = 14.0    // radio disco dentado
HINGE_TEETH    = 36      // 10° por paso (360/36)
LEG_L          = 80.0    // largo pata
M3_D           = 3.4     // diámetro agujero M3
```

Cambiar parámetros y reimportar los STL para customizar.

---

## Impresión Recomendada

### Cuna (cradle)

| Parámetro | Valor |
|-----------|-------|
| Material | PETG o ABS |
| Relleno | 25% |
| Perímetros | 3 |
| Orientación | Boca abajo (cara frontal en la cama) |
| Soporte | Sí, solo en las orejetas de bisagra (mínimo) |
| Tiempo est. | ~2–3h a 0.15mm |

**Nota:** Imprimir boca abajo reduce el soporte y mejora la calidad de la cara frontal. Las orejetas de la bisagra necesitan soporte puntual.

### Pata (leg)

| Parámetro | Valor |
|-----------|-------|
| Material | PETG o ABS |
| Relleno | 30% |
| Perímetros | 4 |
| Orientación | Plana (como sale del archivo STL) |
| Soporte | No necesita |
| Tiempo est. | ~1–1.5h a 0.15mm |

**Nota:** La pata es simple y no necesita soporte. Imprimir plana maximiza resistencia en la bisagra.

---

## Ensamble

### Paso 1: Preparar las piezas

1. Retira soportes (si los hay) y limpia rebabas
2. Prueba la cuna: inserta y saca el Clawdmeter varias veces para soltar las pestañas snap-fit (deben ceder sin dañarse)

### Paso 2: Preparar la bisagra

1. **En la pata:**
   - Verifica que el agujero M3 está limpio y puede pasar el tornillo sin fricción
   - Coloca la **tuerca M3 en el hueco hexagonal** de la cara interior del disco (asiento cuadrado de 0.5–1mm de profundidad)
   - La tuerca debe encajar sin sobresalir

2. **En la cuna:**
   - Verifica que los canales de la bisagra (orejetas) están limpios

### Paso 3: Ensamblar

1. Alinea el disco de la pata con los oídos de la cuna (bisagra)
2. Inserta el **tornillo M3×20** a través de la cuna → disco de la pata → tuerca
3. **Aprieta con moderación** (1–2 Nm) hasta que:
   - La pata puede girar pero con fricción tactil (sienta los clicks de 10°)
   - No hay juego de lado a lado
4. Usa una llave hexagonal M3 o destornillador Phillips para ajustar

### Paso 4: Instalar el pad antideslizante

1. Limpia la cara inferior del pie de la pata (polvo + residuos de impresión)
2. Retira la capa adhesiva del pad 10×10mm
3. **Presiona centrado en el pie** (equidistante del borde)
4. Presiona 30s para activar adhesivo

### Paso 5: Colocar el dispositivo

1. Con el Clawdmeter en la mano, **inserta primero la esquina inferior en la cuna** (el borde donde está USB-C)
2. Desliza hacia adentro hasta que encaje con un *click* (las pestañas snap-fit sellan lateralmente)
3. Confirma que la pantalla no sobresale del marco frontal

---

## Ajuste de Ángulos

### Usar la bisagra con muescas

- La bisagra tiene **36 muescas**, cada una espaciada **10°**
- Gira la pata lentamente; sentirás *detents* cada 10°
- **Posiciones recomendadas:**
  - **0°** (plegado) — almacenaje plano
  - **30°** (bajo) — referencia rápida en el escritorio
  - **45°** (medio) — posición estándar de uso
  - **60°** (alto) — mayor inclinación hacia el usuario
  - **90°** (vertical) — casi perpendicular al escritorio

### Bloqueo con tornillo

1. Una vez en el ángulo deseado, aprieta el tornillo M3 **con firmeza** (2–3 Nm)
2. La pata queda bloqueada en esa posición
3. Para cambiar ángulo: afloja el tornillo, reposiciona, aprieta de nuevo

---

## Ajustes y Troubleshooting

### La pata es muy floja / muy rígida

- **Muy floja**: Aprieta más el tornillo M3 (aumento +0.5 Nm)
- **Muy rígida**: Afloja ligeramente; la fricción debe permitir giro suave con resistance tactil

### El dispositivo no entra / está muy apretado

- Aumenta `CRADLE_CLEAR` en el SCAD (línea 20) de 0.3 a 0.4–0.5 mm
- Reexporta y reimpr

ime la cuna

### La tuerca se suelta en la bisagra

- Aprieta el tornillo M3 **con llave** (no solo con los dedos)
- Verifica que el agujero hexagonal tiene profundidad suficiente (~1.5 mm)
- Si sigue soltándose, añade un pequeño punto de **Loctite azul** (removible) en la tuerca

---

## Parámetros Técnicos del Diseño

| Parámetro | Valor | Notas |
|-----------|-------|-------|
| Diámetro agujero M3 | 3.4 mm | Clearance ajustado para tolerancia |
| Hueco tuerca M3 | Hexagonal 6.1mm | Capacidad + 0.1mm clearance |
| Radio cuna interior | 6.1 mm | Sigue R5.8 del PCB + 0.3 clearance |
| Tamaño snap-tab | 1.5 mm × 14 mm | Flexiona suavemente sin fatiga |
| Espesor pared cuna | 2.5 mm | Resistencia + paredes finas (impresión rápida) |
| Espesor pata | 4 mm | Rigidez sin exceso de peso |
| Pad goma recess | 1.5 mm | Apenas visible en el pie |
| Holgura entre orejas | 0.4 mm | Movimiento suave sin juego |

---

## Archivos

- `clawdmeter_stand.scad` — Modelo paramétrico (edita parámetros aquí)
- `clawdmeter_cradle.stl` — Pieza 1, lista para laminar
- `clawdmeter_leg.stl` — Pieza 2, lista para laminar
- `README.md` — Este archivo

---

## Licencia

Este diseño es parte del proyecto **Clawdmeter** ([github.com/HermannBjorgvin/Clawdmeter](https://github.com/HermannBjorgvin/Clawdmeter)), de código abierto bajo la licencia que use el proyecto principal.

---

## Notas de Diseño

- **Detentes radiales + M3 bloqueo**: Cualquier ángulo es posible, pero cada 10° hay una posición con feedback tactil natural. El tornillo asegura que no se desliza bajo vibraciones.
- **Snap-fit vs tornillos**: Snap-fit permite cambios rápidos del dispositivo sin herramientas. Las pestañas son pasivas (solo retención, sin compresión exagerada), así que la vida de fatiga es excelente.
- **Pata plegable**: La bisagra integral permite plegar 180° plano, ideal para viajeros o almacenaje en mochilas.
- **Cable libre**: USB-C cuelga desde abajo, no interrumpe la vista frontal de la pantalla.

---

¿Necesitas ajustes? Edita los parámetros en `clawdmeter_stand.scad` (líneas 13–45) y reimporta los STL.
