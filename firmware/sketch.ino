/*
 * FreeRTOS task scheduling on the ESP32
 * -------------------------------------
 * The same job that a single loop() would do, split into five tasks that the
 * real-time kernel schedules. Every mechanism an RTOS gives you is used once,
 * and only once, so the code stays readable:
 *
 *   sensorTask   periodic producer, uses vTaskDelayUntil for a jitter-free rate
 *   controlTask  consumer, blocks on a queue instead of polling
 *   displayTask  reads shared state under a mutex
 *   alarmTask    sleeps until an interrupt notifies it (highest priority)
 *   statsTask    reports uptime, free heap and each task's stack headroom
 *
 * Hardware (all simulated in Wokwi, no parts required)
 *   GPIO 34  potentiometer, stands in for a temperature sensor
 *   GPIO 4   button to GND, raises the alarm
 *   GPIO 2   green LED, the fan output
 *   GPIO 5   red LED, the alarm output
 *   UART     115200 baud status output
 */

/* ---------------- pins and constants ---------------- */
static const int PIN_SENSOR = 34;
static const int PIN_BUTTON = 4;
static const int PIN_FAN    = 2;
static const int PIN_ALARM  = 5;

static const float TEMP_MIN = 15.0f;    /* pot fully left  */
static const float TEMP_MAX = 60.0f;    /* pot fully right */
static const float FAN_ON   = 35.0f;    /* switch the fan on above this  */
static const float FAN_OFF  = 32.0f;    /* and off again below this      */

/* ---------------- what the sensor task sends ---------------- */
typedef struct {
    uint32_t sequence;
    uint32_t timestamp_ms;
    float    temperature_c;
} Reading;

/* ---------------- shared state, protected by a mutex ---------------- */
typedef struct {
    float    temperature_c;
    bool     fanOn;
    bool     alarmActive;
    uint32_t readings;
    uint32_t alarms;
} SystemState;

static SystemState  state = {0};
static SemaphoreHandle_t stateMutex;
static QueueHandle_t     readingQueue;
static TaskHandle_t      alarmTaskHandle = NULL;

static TaskHandle_t hSensor, hControl, hDisplay, hStats;

/* ------------------------------------------------------------------ */
/* Interrupt handler                                                   */
/*                                                                     */
/* An ISR must be short and must not block, so it does no work itself. */
/* It unblocks the alarm task and asks the scheduler to switch to that */
/* task immediately on the way out of the interrupt. This pattern is   */
/* called deferred interrupt processing, and it is the reason an RTOS  */
/* can keep interrupt latency low while still doing real work.         */
/* ------------------------------------------------------------------ */
static volatile uint32_t lastPressMs = 0;

void IRAM_ATTR buttonIsr()
{
    uint32_t now = millis();
    if (now - lastPressMs < 200)      /* crude debounce, ISR-safe */
        return;
    lastPressMs = now;

    BaseType_t higherPriorityTaskWoken = pdFALSE;
    vTaskNotifyGiveFromISR(alarmTaskHandle, &higherPriorityTaskWoken);
    portYIELD_FROM_ISR(higherPriorityTaskWoken);
}

/* ------------------------------------------------------------------ */
/* sensorTask - a periodic producer                                    */
/*                                                                     */
/* vTaskDelayUntil sleeps until a fixed point in time rather than for  */
/* a fixed length of time. If this pass took 3 ms, the next one starts */
/* 247 ms later, so the sampling rate does not drift. vTaskDelay would */
/* add the execution time to every period.                             */
/* ------------------------------------------------------------------ */
static void sensorTask(void *arg)
{
    const TickType_t period = pdMS_TO_TICKS(250);
    TickType_t lastWake = xTaskGetTickCount();
    uint32_t sequence = 0;

    for (;;) {
        int raw = analogRead(PIN_SENSOR);           /* 12 bit: 0 to 4095 */

        Reading r;
        r.sequence      = sequence++;
        r.timestamp_ms  = millis();
        r.temperature_c = TEMP_MIN + (TEMP_MAX - TEMP_MIN) * raw / 4095.0f;

        /* Never block forever inside a periodic task: if the consumer has
         * stalled, drop the sample and say so rather than stop sampling. */
        if (xQueueSend(readingQueue, &r, pdMS_TO_TICKS(10)) != pdPASS) {
            Serial.println("! queue full, sample dropped");
        }

        vTaskDelayUntil(&lastWake, period);
    }
}

/* ------------------------------------------------------------------ */
/* controlTask - a consumer that blocks                                */
/*                                                                     */
/* xQueueReceive with portMAX_DELAY puts this task to sleep until a    */
/* sample arrives. While it sleeps it uses no CPU at all. A superloop  */
/* would have to poll a flag and burn cycles doing nothing.            */
/* ------------------------------------------------------------------ */
static void controlTask(void *arg)
{
    Reading r;
    bool fan = false;

    for (;;) {
        if (xQueueReceive(readingQueue, &r, portMAX_DELAY) != pdPASS)
            continue;

        /* Hysteresis: separate switch-on and switch-off thresholds stop the
         * fan chattering when the temperature sits right on the limit. */
        if (!fan && r.temperature_c >= FAN_ON)  fan = true;
        if ( fan && r.temperature_c <= FAN_OFF) fan = false;
        digitalWrite(PIN_FAN, fan ? HIGH : LOW);

        if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            state.temperature_c = r.temperature_c;
            state.fanOn         = fan;
            state.readings      = r.sequence + 1;
            xSemaphoreGive(stateMutex);
        }
    }
}

/* ------------------------------------------------------------------ */
/* displayTask - reads shared state                                    */
/*                                                                     */
/* The mutex is held only long enough to copy the struct, never while  */
/* printing. Holding a lock across a slow operation is how a system    */
/* ends up with priority inversion and missed deadlines.               */
/* ------------------------------------------------------------------ */
static void displayTask(void *arg)
{
    const TickType_t period = pdMS_TO_TICKS(500);
    TickType_t lastWake = xTaskGetTickCount();

    for (;;) {
        SystemState copy;
        if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            copy = state;
            xSemaphoreGive(stateMutex);

            Serial.printf("T=%5.1f C  fan=%-3s  alarm=%-3s  samples=%lu\n",
                          copy.temperature_c,
                          copy.fanOn ? "ON" : "off",
                          copy.alarmActive ? "ON" : "off",
                          (unsigned long)copy.readings);
        }
        vTaskDelayUntil(&lastWake, period);
    }
}

/* ------------------------------------------------------------------ */
/* alarmTask - the highest priority task, and idle almost always       */
/*                                                                     */
/* ulTaskNotifyTake is a lightweight binary semaphore built into every */
/* task. It is faster and uses less RAM than a real semaphore object,  */
/* and it is the normal way to wake one specific task from an ISR.     */
/* ------------------------------------------------------------------ */
static void alarmTask(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);   /* sleeps here */

        if (xSemaphoreTake(stateMutex, portMAX_DELAY) == pdTRUE) {
            state.alarmActive = true;
            state.alarms++;
            xSemaphoreGive(stateMutex);
        }

        Serial.println("*** ALARM raised by button interrupt");
        for (int i = 0; i < 6; i++) {              /* 3 s of blinking */
            digitalWrite(PIN_ALARM, HIGH);
            vTaskDelay(pdMS_TO_TICKS(250));
            digitalWrite(PIN_ALARM, LOW);
            vTaskDelay(pdMS_TO_TICKS(250));
        }

        if (xSemaphoreTake(stateMutex, portMAX_DELAY) == pdTRUE) {
            state.alarmActive = false;
            xSemaphoreGive(stateMutex);
        }
        Serial.println("*** ALARM cleared");
    }
}

/* ------------------------------------------------------------------ */
/* statsTask - how much stack is each task actually using              */
/*                                                                     */
/* uxTaskGetStackHighWaterMark returns the smallest amount of free     */
/* stack a task has ever had, in words. Guessing stack sizes and then  */
/* measuring them is the accepted way to size them; a number close to  */
/* zero means the next deep call will overflow and corrupt memory.     */
/* ------------------------------------------------------------------ */
static void statsTask(void *arg)
{
    const TickType_t period = pdMS_TO_TICKS(5000);
    TickType_t lastWake = xTaskGetTickCount();

    for (;;) {
        vTaskDelayUntil(&lastWake, period);

        Serial.println("--- task report ---------------------------------");
        Serial.printf("uptime %lu s, free heap %u bytes\n",
                      (unsigned long)(millis() / 1000),
                      (unsigned)ESP.getFreeHeap());
        Serial.printf("  %-10s prio %d  stack free %u words\n", "sensor",
                      (int)uxTaskPriorityGet(hSensor),
                      (unsigned)uxTaskGetStackHighWaterMark(hSensor));
        Serial.printf("  %-10s prio %d  stack free %u words\n", "control",
                      (int)uxTaskPriorityGet(hControl),
                      (unsigned)uxTaskGetStackHighWaterMark(hControl));
        Serial.printf("  %-10s prio %d  stack free %u words\n", "display",
                      (int)uxTaskPriorityGet(hDisplay),
                      (unsigned)uxTaskGetStackHighWaterMark(hDisplay));
        Serial.printf("  %-10s prio %d  stack free %u words\n", "alarm",
                      (int)uxTaskPriorityGet(alarmTaskHandle),
                      (unsigned)uxTaskGetStackHighWaterMark(alarmTaskHandle));
        Serial.printf("  queue holds %u of %u\n",
                      (unsigned)uxQueueMessagesWaiting(readingQueue), 8u);
        Serial.println("-------------------------------------------------");
    }
}

/* ------------------------------------------------------------------ */
void setup()
{
    Serial.begin(115200);
    delay(300);
    Serial.println("\nFreeRTOS task scheduling demo");

    pinMode(PIN_FAN, OUTPUT);
    pinMode(PIN_ALARM, OUTPUT);
    pinMode(PIN_BUTTON, INPUT_PULLUP);
    analogReadResolution(12);

    /* Create the kernel objects before any task can use them. */
    readingQueue = xQueueCreate(8, sizeof(Reading));
    stateMutex   = xSemaphoreCreateMutex();
    if (readingQueue == NULL || stateMutex == NULL) {
        Serial.println("kernel objects could not be created, halting");
        for (;;) delay(1000);
    }

    /* xTaskCreatePinnedToCore(function, name, stack words, arg,
     *                         priority, handle, core)
     * Higher number means higher priority. The alarm task is highest so it
     * preempts everything the moment the interrupt notifies it. */
    xTaskCreatePinnedToCore(alarmTask,   "alarm",   2560, NULL, 4, &alarmTaskHandle, 1);
    xTaskCreatePinnedToCore(sensorTask,  "sensor",  2560, NULL, 3, &hSensor,  1);
    xTaskCreatePinnedToCore(controlTask, "control", 2560, NULL, 2, &hControl, 1);
    xTaskCreatePinnedToCore(displayTask, "display", 3072, NULL, 1, &hDisplay, 1);
    xTaskCreatePinnedToCore(statsTask,   "stats",   3072, NULL, 1, &hStats,   1);

    attachInterrupt(digitalPinToInterrupt(PIN_BUTTON), buttonIsr, FALLING);

    Serial.println("tasks started, turn the knob and press the button");
}

void loop()
{
    /* Nothing here on purpose. The Arduino loop is itself a FreeRTOS task,
     * and everything this program does lives in the five tasks above.
     * Delaying keeps this task off the CPU instead of spinning. */
    vTaskDelay(pdMS_TO_TICKS(1000));
}
