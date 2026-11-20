# RTOS Monitor with I2C Slave (STM32H743I-EVAL <-> Raspberry Pi)

A live system-monitor dashboard for the **STM32H743I-EVAL** board, built on **FreeRTOS**, that also acts as an **I2C slave (address 0x2A)** so a **Raspberry Pi** can read the monitor data over I2C. Eight tasks run at different priorities and share data through a queue, a mutex and a semaphore. The 640x480 LCD shows what the RTOS is doing in real time, and every 100 ms the same values are published to the Raspberry Pi as a 32-byte I2C frame.

## Features

- **CPU load** measured from FreeRTOS run-time statistics, using the Cortex-M7 DWT cycle counter as the timer (100 % minus the time spent in the IDLE task, averaged over 500 ms)
- **Task table** with each task's priority, state (RUN / RDY / BLK / SUS) and stack usage as a percentage, from the stack high-water mark
- **Heap monitor**: free, used and minimum-ever-free bytes of the 32 KB FreeRTOS heap
- **Uptime and tick counter** (1 kHz tick)
- **TAMPER button** (interrupt) and **WAKEUP button** (polled and debounced), each with a press counter
- **ADC**: the on-board potentiometer (PA0_C, ADC1 channel 0) shown as volts, raw value and a bar
- **LED mirror**: the dashboard shows the state of the green and orange LEDs as tracked in software
- **System log**: the last 4 events with timestamps (a ring buffer of 8)
- **I2C slave to Raspberry Pi**: a dedicated I2C task publishes a checksummed 32-byte frame; the I2C transfers themselves are handled by interrupts, so the Pi never waits on the display
- **Self-tests** that log a message if the DWT counter is not running or run-time statistics are switched off
- Flicker-free drawing: labels and frames are drawn once, and only changing values are redrawn

## Hardware

| Item | Details |
|---|---|
| Board | STM32H743I-EVAL (STM32H743XIH6 MCU) |
| Display | On-board LCD, 640x480, landscape |
| Inputs | TAMPER button, WAKEUP button, potentiometer RV1 |
| Debugger | On-board ST-LINK (USB) |
| I2C master | Raspberry Pi (I2C bus 1) |

### I2C wiring

| Raspberry Pi | STM32H743I-EVAL |
|---|---|
| GPIO2 / SDA (pin 3) | PB7 (I2C1_SDA) |
| GPIO3 / SCL (pin 5) | PB6 (I2C1_SCL) |
| GND (pin 6) | GND |

The Raspberry Pi header already has pull-up resistors on SDA and SCL, so no extra resistors are needed. Both boards use 3.3 V logic.

### Important: I2C1 is the board's own bus

On the STM32H743I-EVAL, I2C1 (PB6/PB7) is also the bus that connects the MFX I/O expander (address 0x42) and the audio codec (address 0x1A). The ST BSP uses this bus as an **I2C master** for the board LEDs, the joystick and the LCD start-up probe. A bus cannot have the BSP master and the Raspberry Pi master active at the same time on one peripheral, so this project is set up as follows:

| Part | Behaviour in this project |
|---|---|
| LCD start-up | The BSP uses I2C1 during `BSP_LCD_Init()` only. |
| After LCD start-up | `BSP_I2C1_DeInit()` releases I2C1, then the project initialises it as an **I2C slave at 0x2A**. |
| Board LEDs (LED1 to LED4) | **Disabled** (they are on the MFX, over I2C1). The dashboard still shows a software mirror of the green and orange LEDs. |
| Joystick | **Disabled** (it is on the MFX, over I2C1). The joystick value is always 0. |
| TAMPER and WAKEUP buttons | Work normally (direct GPIO). |

Because the BSP master uses the bus during LCD start-up, **power up the board with the Raspberry Pi's SDA and SCL wires disconnected** (leave GND connected), wait for the dashboard, then connect SDA and SCL.

## Software requirements

- STM32CubeIDE
- FreeRTOS kernel files (included in `STM32CubeIDE/FreeRTOS`: `tasks.c`, `queue.c`, `list.c`, `port.c`, `heap_4.c`)
- STM32H7 HAL and BSP drivers (included in the `Drivers` folder)
- On the Raspberry Pi: Python 3, `smbus2` (`pip install smbus2`) and `i2c-tools` (`sudo apt install i2c-tools`). I2C must be enabled (`sudo raspi-config` -> Interface Options -> I2C).

## Getting started

### STM32 side

1. Clone or download this repository.
2. Open STM32CubeIDE and choose **File -> Import -> General -> Existing Projects into Workspace**.
3. Select the `RTOSMonitor_Multitask/STM32CubeIDE` folder and click **Finish**.
4. Do a clean build (Project -> Clean, then the hammer icon). You want 0 errors.
5. Connect the board to your PC with the ST-LINK USB cable.
6. Click **Debug** (bug icon), then **Resume (F8)**. A debug configuration, `RTOSMonitor_Multitask.launch`, is included. Make sure you start this one and not another launch configuration.
7. Wait until the dashboard is visible and the system log shows `I2C listening on 0x2A`.

### Raspberry Pi side

1. With the board running, connect SDA, SCL and GND as shown above.
2. Check that the board is found:
   ```bash
   i2cdetect -y 1
   ```
   Address **2a** should appear (0x1A and 0x42 are the board's own devices and may appear too).
3. Run the reader script:
   ```bash
   python3 read_stm32.py
   ```

Example output:
```
{'joystick': 0, 'tamperCount': 2, 'wakeCount': 1, 'adcRaw': 2048, 'cpuLoad': 37, 'workloadCount': 1520}
```

## I2C protocol

| Setting | Value |
|---|---|
| Role | STM32 = slave, Raspberry Pi = master |
| Slave address | `0x2A` (7-bit) |
| Bus | I2C1 on PB6 (SCL) and PB7 (SDA), alternate function 4 |
| Speed | Pi default (100 kHz); the STM32 timing register is `0x00606092` for a 100 MHz I2C kernel clock |
| Transfer | The Pi reads a **32-byte frame**. Writes of up to 4 bytes (for example a register byte) are accepted and ignored. |
| Update rate | The frame is refreshed every 100 ms |

### Frame format (all values little-endian)

| Bytes | Field | Meaning |
|---|---|---|
| 0-3 | `joystick` | Joystick state. Always 0 in this project (joystick disabled). |
| 4-7 | `tamperCount` | Number of TAMPER button presses |
| 8-11 | `wakeCount` | Number of WAKEUP button presses |
| 12-15 | `adcRaw` | Potentiometer, 12-bit value 0 to 4095 (filtered) |
| 16-19 | `cpuLoad` | CPU load in percent (0 to 100) |
| 20-23 | `workloadCount` | Number of workload-task cycles completed |
| 24-30 | padding | Zeros. They make a longer read safe, so the slave never runs out of data. |
| 31 | checksum | XOR of bytes 0 to 30 |

Python decoding:
```python
import struct
joystick, tamper, wake, adc, cpu, work = struct.unpack("<6I", frame[:24])
```

### How the I2C part works

1. `main()` initialises the LCD with the BSP, releases I2C1 (`BSP_I2C1_DeInit()`), then runs `I2C1_Init()`, which sets I2C1 up as a slave at 0x2A.
2. The **I2C task** calls `HAL_I2C_EnableListen_IT()` once the scheduler is running, then copies the current values into a 32-byte buffer every 100 ms (inside a critical section so a frame is never half-written).
3. When the Pi addresses 0x2A, the I2C interrupt calls `HAL_I2C_AddrCallback()`:
   - **Read:** the buffer is copied to a frozen snapshot and sent with `HAL_I2C_Slave_Seq_Transmit_IT()`.
   - **Write:** up to 4 bytes are received and ignored.
4. After every transfer, `HAL_I2C_ListenCpltCallback()` (and `HAL_I2C_ErrorCallback()`) turn listening back on, so the STM32 is always ready for the next read.

The I2C interrupts run at priority 5, which is allowed to preempt tasks but never calls a FreeRTOS function, so no RTOS call is made from the interrupt.

## What you will see

The dashboard screen has these areas:

| Area | Shows |
|---|---|
| Header | "RTOS MONITOR" title, CPU clock in MHz, RUNNING badge |
| Top row | Uptime, tick count, CPU load with a coloured bar (green, amber above 70 %, red above 90 %) |
| Tasks card | One row per task: name, priority, state, stack-use bar |
| Heap card | Free bytes, used bar, minimum free, used bytes |
| Workload card | Count of workload cycles completed |
| Input card | Joystick pad (always idle), TAMPER and WAKEUP counts |
| ADC card | Potentiometer voltage, bar and raw value |
| LEDs card | Software mirror of the heartbeat and workload LEDs |
| System log | Latest events with `[hh:mm:ss]` timestamps, including `I2C listening on 0x2A` |

Try it: press TAMPER and WAKEUP and turn the potentiometer. Each shows up on screen straight away and, within 100 ms, in the values printed by `read_stm32.py`.

## The tasks

FreeRTOS runs the highest-priority ready task first (a bigger number means higher priority).

| Task | Priority | Stack (words) | Job |
|---|---|---|---|
| Input | 4 | 384 | Debounces the WAKEUP button and reads the ADC every 100 ms. (Joystick polling is disabled.) |
| Status | 3 | 384 | Draws a small "Status tick" counter every 50 ms. This proves the display is shared safely. |
| Button | 3 | 256 | Sleeps until the TAMPER interrupt gives a semaphore, then counts and logs the press. |
| Display | 2 | 640 | Draws the dashboard every 200 ms (`vTaskDelayUntil`) and logs events from the queue. |
| Memory | 2 | 384 | Every 500 ms, reads heap figures and works out CPU load. Logs a warning above 80 % and a recovery below 60 %. |
| I2C | 2 | 256 | Arms I2C listening, then refreshes the 32-byte frame for the Raspberry Pi every 100 ms. Not shown in the task table. |
| Work | 1 | 256 | A deliberate busy loop (200,000 additions) to give the CPU something to do, then sleeps 20 ms. |
| LedMon | 1 | 256 | Updates the software LED mirror (the physical LEDs are disabled; see above). |

### How the tasks talk to each other

| Mechanism | Used for |
|---|---|
| Queue (`joyQueue`, 8 items) | Input task -> Display task: input events |
| Binary semaphore (`tamperSem`) | TAMPER interrupt -> Button task: wake up on a press |
| Mutex (`lcdMutex`) | Display and Status tasks take turns using the LCD |
| Shared struct (`gStats`) | Tasks publish values (CPU load, heap, counters) that the Display and I2C tasks read |
| I2C interrupt + buffer | I2C task -> I2C interrupt -> Raspberry Pi: the frame |

The TAMPER interrupt gives the semaphore with `xSemaphoreGiveFromISR()` and then calls `portYIELD_FROM_ISR()`, so the Button task runs immediately.

## Project structure

```
RTOSMonitor_Multitask/
├── Inc/                         # HAL and BSP configuration headers, main.h
├── Src/
│   ├── main.c                   # Clock, MPU, cache and LCD setup; hands I2C1 to the Pi slave; starts the scheduler
│   ├── stm32h7xx_it.c           # SysTick (feeds HAL and FreeRTOS), EXTI and I2C1 event/error handlers
│   └── system_stm32h7xx.c       # System init
├── STM32CubeIDE/
│   ├── Example/User/
│   │   ├── app_tasks.c          # The 8 tasks, queue, mutex, semaphore, ADC, I2C slave callbacks, run-time timer
│   │   └── dashboard.c          # All LCD drawing and the system log
│   ├── Inc/
│   │   ├── FreeRTOSConfig.h     # FreeRTOS settings
│   │   ├── app_tasks.h          # Task IDs, stack sizes, shared stats struct, I2C slave address and frame struct
│   │   └── dashboard.h
│   ├── FreeRTOS/                # Kernel source (tasks, queue, list, port, heap_4)
│   ├── STM32H743XIHX_FLASH.ld   # Linker script
│   └── RTOSMonitor_Multitask.launch   # Debug configuration
└── raspberry_pi/
    └── read_stm32.py            # Reads and decodes the 32-byte frame on the Raspberry Pi
```

`Src/game.c` is left over from an earlier game project and is not called by this application.

## Key FreeRTOS settings (`FreeRTOSConfig.h`)

| Setting | Value |
|---|---|
| Scheduler | Preemptive |
| Tick rate | 1000 Hz |
| Heap | 32 KB (`heap_4`, dynamic allocation) |
| Priorities | 7 levels |
| Run-time statistics | On, using the DWT cycle counter |
| Trace facility | On (needed for `uxTaskGetSystemState`) |
| Max interrupt priority that may call FreeRTOS | 5 |
| Software timers | Off |

## How it works

1. `main()` sets up the MPU, caches, HAL and a 400 MHz system clock.
2. It initialises the LCD. The board LEDs and joystick are not initialised, because they share I2C1 with the Raspberry Pi link.
3. `main()` releases the BSP's use of I2C1 and initialises I2C1 as a slave at address 0x2A.
4. `App_CreateTasks()` creates the queue, mutex, semaphore and 8 tasks, and initialises the ADC and the buttons.
5. `vTaskStartScheduler()` hands control to FreeRTOS. From here on, the tasks run and `main()` never continues.
6. `SysTick_Handler` calls both `HAL_IncTick()` and `xPortSysTickHandler()`, so the HAL delay functions and the RTOS tick share one timer.

**CPU load:** FreeRTOS counts how long each task runs, using the DWT cycle counter as its clock. The Memory task adds up all task times, finds the IDLE task's share over the last 500 ms, and reports `100 % - idle share`.

**Stack use:** `uxTaskGetStackHighWaterMark()` returns the smallest free stack the task has ever had, so the bar shows the worst-case usage so far. If a bar turns amber (over 80 %), increase that task's stack size in `app_tasks.h`.

## Troubleshooting

| Problem | Fix |
|---|---|
| `[Errno 110] Connection timed out` on the Pi | The STM32 is not answering. Check that the dashboard is running and the log shows `I2C listening on 0x2A`, that GND is connected, and that SDA and SCL are not swapped. Make sure you flashed `RTOSMonitor_Multitask`. |
| `i2cdetect` does not show 2a | Same checks as above. Also reset the board, wait for the dashboard, and reconnect SDA and SCL only after that. |
| Random extra addresses in `i2cdetect`, or the dashboard stays black | The Pi was connected while the board was starting. Disconnect SDA and SCL, power-cycle the board, wait for the dashboard, then reconnect. |
| Pi script prints `checksum mismatch` | Read exactly 32 bytes and use the frame format above. A shorter read is not checksummed. |
| Log shows `I2C listen FAILED` | `HAL_I2C_Init` did not leave I2C1 ready. Check that `BSP_I2C1_DeInit()` is called before `I2C1_Init()` in `main.c`. |
| Joystick and board LEDs do nothing | Expected: they are on the MFX over I2C1, which is used for the Pi link. Use the dashboard's software LED mirror instead. |
| Log shows "Run-time stats OFF" | Check `configGENERATE_RUN_TIME_STATS` in `FreeRTOSConfig.h`. |
| Log shows "DWT cycle counter NOT counting" | The debugger or core is blocking the DWT unit. Power-cycle the board and start a new debug session. |
| ADC shows `n/a` | ADC initialisation failed. The log shows "ADC init failed". Check the ADC clock settings in `Adc_Init()`. |
| Screen stays black with the Pi disconnected | Check that `BSP_LCD_Init()` succeeds and that the heap is large enough (32 KB). |
| Board freezes in `configASSERT` | A FreeRTOS check failed. Common causes: wrong interrupt priority, or a stack that is too small. |
| `undefined reference to ...` | Make sure `app_tasks.c`, `dashboard.c` and the FreeRTOS source files are in the build and not excluded. |

## Author

Deniston
