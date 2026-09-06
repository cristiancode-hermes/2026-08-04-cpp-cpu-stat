# cpu-stat — Linux per-core CPU utilization monitor

Monitor de utilización de CPU por núcleo en **C++23** puro. Lee `/proc/stat` y desglosa los 8 contadores por core (user, nice, system, idle, iowait, irq, softirq, steal) con porcentajes, umbrales de color, load average y modo watch en vivo.

## ⚡ Uso

```bash
# Compilar
g++ -std=c++23 -O2 -Wall -Wextra src/main.cpp -o cpu-stat

# Uso básico
./cpu-stat

# Modo watch (refresco cada N segundos)
./cpu-stat --watch 2

# Salida JSON para scripting
./cpu-stat --json

# Sin colores (pipes/redirección)
./cpu-stat --no-color

# Self-test
./cpu-stat --selftest

# Ayuda
./cpu-stat --help
```

## 📊 Ejemplo de salida

```
cpu-stat — Linux per-core CPU utilization monitor (C++23)

  CORE       user   nice    sys   idle iowait   irq softirq  steal
  total      2.1    0.0    1.6   96.3    0.0    0.0    0.0    0.0
  cpu0       0.0    0.0    4.3   95.7    0.0    0.0    0.0    0.0
  cpu1       0.0    0.0    0.0  100.0    0.0    0.0    0.0    0.0
  cpu2       4.5    0.0    0.0   95.5    0.0    0.0    0.0    0.0
  cpu3       0.0    0.0    4.5   95.5    0.0    0.0    0.0    0.0

  load          0.29  0.24  0.15   running      1/1066  pid69305
  uptime      22d 07:43:15
```

## 🔧 Flags

| Flag | Descripción |
|------|-------------|
| `--help` / `-h` | Muestra ayuda |
| `--watch N` | Refresco cada N segundos (Ctrl+C para salir) |
| `--json` | Salida JSON válida |
| `--no-color` | Desactiva colores ANSI |
| `--selftest` | Ejecuta 15 tests de verificación |

## 📐 Arquitectura

```
/proc/stat ──► parse_cpu_line() ──► CpuTimes
                                          │
                                    compute_pct()
                                          │
                                     CpuPct ──► render_table()
                                                    │
/proc/loadavg ──► SystemContext ─────────────────────┘
/proc/uptime   ──►
```

- Parsing con `std::from_chars` — sin excepciones, sin `atof` en hot path
- Delta de jiffies entre dos muestras para porcentajes precisos
- 8 contadores por core + total agregado
- Codificación por colores: rojo ≥50%, amarillo ≥25%, verde <25%
- Exit codes: 0 (éxito), 2 (error/selftest fallido)

## 🧪 Self-test

```bash
$ ./cpu-stat --selftest
15/15 selftests passed
```

Cubre: parsing de líneas típicas, formato de 4 campos mínimos, multi-dígito, rechazo de no-CPU, delta con valores conocidos, delta cero, multi-core, lectura real de `/proc/stat`, loadavg, uptime, formato de duración y whitespace variable.

## 📦 Dependencias

**Cero.** Solo la biblioteca estándar de C++23 y GCC 14. Compilación directa:

```bash
g++ -std=c++23 -O2 -Wall -Wextra src/main.cpp -o cpu-stat
```

## 🧰 Tecnologías

- **C++23** — `std::print`, `std::from_chars`, `std::expected`, `std::string_view`
- **Linux `/proc` filesystem** — `/proc/stat`, `/proc/loadavg`, `/proc/uptime`
- **ANSI escape codes** — salida con color en terminal

## 📄 Licencia

MIT
