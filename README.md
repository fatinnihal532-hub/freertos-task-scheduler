# FreeRTOS Task Scheduling on the ESP32

The job an `while(1)` superloop would do, rewritten as five FreeRTOS tasks so
that the kernel decides what runs and when. Every RTOS mechanism appears exactly
once, so the code can be read end to end in a sitting.

**[Run it in your browser](https://wokwi.com/projects/475219714844478465)** on Wokwi, no hardware required.

| Task | Priority | What it does | How it waits |
|---|---|---|---|
| `alarmTask` | 4 | blinks the alarm LED for 3 s | task notification from an ISR |
| `sensorTask` | 3 | reads the sensor every 250 ms | `vTaskDelayUntil` |
| `controlTask` | 2 | hysteresis control of the fan | blocks on a queue |
| `displayTask` | 1 | prints the status line twice a second | `vTaskDelayUntil` |
| `statsTask` | 1 | reports stack and heap every 5 s | `vTaskDelayUntil` |

## Why an RTOS at all

A superloop has one failure mode that gets worse as the program grows: the
slowest thing in the loop sets the response time of everything else. Print a
long status line over a 9600-baud UART and the sensor is not read for 80 ms.
Add a second slow job and the numbers stop being predictable.

An RTOS removes the coupling. Each job states how often it needs to run and how
important it is, and the kernel preempts a low-priority task the instant a
higher-priority one becomes ready. The tasks never call each other; they pass
data through a queue.

## The five mechanisms, and why each one is there

**Periodic scheduling without drift.** `vTaskDelayUntil` sleeps until a fixed
point in time; `vTaskDelay` sleeps for a fixed length of time. If a pass takes
3 ms, the first still runs every 250 ms and the second runs every 253 ms. Over
an hour that is a minute of accumulated error, which is why sample rates are
built on the first one.

**A queue instead of a shared flag.** `sensorTask` writes a `Reading` struct
into a queue; `controlTask` blocks on that queue and uses no CPU until something
arrives. The queue copies the data, so neither task can see a half-written
struct. The sensor never blocks forever on a full queue: it waits 10 ms, then
drops the sample and reports it, because a producer that stalls is worse than a
sample that is lost.

**A mutex around shared state.** Several tasks touch the same `SystemState`
struct. The mutex is held only long enough to copy it, never while printing.
Holding a lock across a slow operation is how a low-priority task ends up
blocking a high-priority one, which is the bug that famously nearly ended the
Mars Pathfinder mission.

**Deferred interrupt processing.** The button ISR does no work. It calls
`vTaskNotifyGiveFromISR` to wake the alarm task and `portYIELD_FROM_ISR` to make
the scheduler switch to it on the way out of the interrupt. The interrupt itself
lasts a few microseconds; the three seconds of blinking happen in a task, where
they can be preempted. Doing that blinking inside the ISR would block every
other interrupt in the system for three seconds.

**Measuring, not guessing, stack sizes.** Every task is given its own stack, and
an overflow corrupts memory somewhere else entirely, which is a miserable bug to
chase. `uxTaskGetStackHighWaterMark` reports the least free stack a task has
ever had. `statsTask` prints it for all four tasks every five seconds, so the
sizes in `xTaskCreatePinnedToCore` can be trimmed to the measured need with a
margin, rather than picked by superstition.

## Sample output

```
T= 28.4 C  fan=off  alarm=off  samples=41
T= 36.2 C  fan=ON   alarm=off  samples=43
*** ALARM raised by button interrupt
--- task report ---------------------------------
uptime 20 s, free heap 298160 bytes
  sensor     prio 3  stack free 2036 words
  control    prio 2  stack free 2104 words
  display    prio 1  stack free 2392 words
  alarm      prio 4  stack free 2188 words
  queue holds 0 of 8
-------------------------------------------------
```

The exact numbers depend on the core version; the shape of the report does not.

## Run it

1. Open [wokwi.com](https://wokwi.com) and start a new **ESP32** project.
2. Paste in [`firmware/sketch.ino`](firmware/sketch.ino) and
   [`diagram.json`](diagram.json). No libraries to install: FreeRTOS is already
   part of the ESP32 Arduino core.
3. Press play and open the serial monitor.
4. Turn the potentiometer past 35 to switch the fan on, and back below 32 to
   switch it off. The gap between the two numbers is the hysteresis.
5. Press the button while the status lines are printing. The alarm task
   preempts everything, which is what the higher priority buys.

## Things worth trying

- Drop `alarmTask` to priority 1 and press the button. The alarm now waits its
  turn; watch the delay appear in the serial output.
- Remove the `vTaskDelayUntil` from `displayTask` and replace it with a busy
  loop. The scheduler still works, but the idle task stops running and the
  watchdog complains. That is what starvation looks like.
- Shrink a task's stack to 512 words and watch the high-water mark reach zero.

## Possible extensions

- Pin the tasks to different cores and use a queue set to merge two producers
- Add a software timer for a watchdog that resets the board if `sensorTask`
  stops feeding the queue
- Replace the serial output with an I2C LCD, and note that the LCD driver then
  needs its own mutex because two tasks want the bus
