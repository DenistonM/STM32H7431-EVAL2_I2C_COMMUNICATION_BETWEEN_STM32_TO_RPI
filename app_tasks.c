#include "main.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"
#include "app_tasks.h"
#include "dashboard.h"
#include <stdio.h>
#include <string.h>

extern I2C_HandleTypeDef hi2c1;

/* The EVAL LEDs and joystick are behind the MFX I/O expander on I2C1 (PB6/PB7),
   which is now the Raspberry Pi slave bus. Using them would make the BSP and the
   slave fight over I2C1, so they are disabled here. */
#undef  BSP_LED_On
#undef  BSP_LED_Off
#undef  BSP_LED_Toggle
#define BSP_LED_On(x)       ((void)(x))
#define BSP_LED_Off(x)      ((void)(x))
#define BSP_LED_Toggle(x)   ((void)(x))
#define BSP_JOY_GetState(x) ((uint32_t)JOY_NONE)
/* Fixed 32-byte frame: bytes 0..23 = I2C_Data_t (little endian),
   bytes 24..30 = 0, byte 31 = XOR of bytes 0..30.
   The Pi must read 32 bytes (or fewer). Padding means a longer read can
   never leave the slave without data (which would stretch SCL -> Errno 110). */
#define I2C_FRAME_LEN  32U
#define I2C_RX_LEN     4U
static uint8_t i2cTxBuf[I2C_FRAME_LEN];    /* newest data, written by I2CTask */
static uint8_t i2cTxSnap[I2C_FRAME_LEN];   /* frozen copy actually sent to the Pi */
static uint8_t i2cRxBuf[I2C_RX_LEN];       /* Pi writes (register/command) are accepted, ignored */
volatile SystemStats_t gStats;
static TaskStatus_t sTaskStat[TASK_COUNT + 3];   /* app tasks + IDLE + spare */
TaskHandle_t gTaskHandle[TASK_COUNT];
static QueueHandle_t joyQueue;
static SemaphoreHandle_t lcdMutex;
static SemaphoreHandle_t tamperSem;
volatile uint32_t i2cErrCount, i2cErrCode;
/* DWT cycle counter = free-running 32-bit timer at CPU speed, used for CPU load */
void App_RunTimeTimerInit(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->LAR = 0xC5ACCE55U;                 /* unlock (needed on Cortex-M7) */
    DWT->CYCCNT = 0U;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

uint32_t App_RunTimeCounter(void)
{
    return DWT->CYCCNT;
}

static const char *JoyName(uint32_t ev)
{
    switch (ev)
    {
        case JOY_SEL:   return "Joystick: SELECT";
        case JOY_UP:    return "Joystick: UP";
        case JOY_DOWN:  return "Joystick: DOWN";
        case JOY_LEFT:  return "Joystick: LEFT";
        case JOY_RIGHT: return "Joystick: RIGHT";
        default:        return "Joystick: multiple";
    }
}

/* ---- ADC: potentiometer RV1 on PA0_C = ADC1 channel 0 ---- */
static ADC_HandleTypeDef hadc1;

static HAL_StatusTypeDef Adc_Init(void)
{
    RCC_PeriphCLKInitTypeDef clk = {0};
    ADC_ChannelConfTypeDef   ch  = {0};

    /* HSI is switched off in SystemClock_Config, so give the ADC a clock from HSE (25 MHz) */
    clk.PeriphClockSelection = RCC_PERIPHCLK_CKPER | RCC_PERIPHCLK_ADC;
    clk.CkperClockSelection  = RCC_CLKPSOURCE_HSE;
    clk.AdcClockSelection    = RCC_ADCCLKSOURCE_CLKP;
    if (HAL_RCCEx_PeriphCLKConfig(&clk) != HAL_OK) { return HAL_ERROR; }

    __HAL_RCC_ADC12_CLK_ENABLE();

    hadc1.Instance                      = ADC1;
    hadc1.Init.ClockPrescaler           = ADC_CLOCK_ASYNC_DIV2;      /* 12.5 MHz */
    hadc1.Init.Resolution               = ADC_RESOLUTION_12B;
    hadc1.Init.ScanConvMode             = ADC_SCAN_DISABLE;
    hadc1.Init.EOCSelection             = ADC_EOC_SINGLE_CONV;
    hadc1.Init.LowPowerAutoWait         = DISABLE;
    hadc1.Init.ContinuousConvMode       = DISABLE;
    hadc1.Init.NbrOfConversion          = 1;
    hadc1.Init.DiscontinuousConvMode    = DISABLE;
    hadc1.Init.ExternalTrigConv         = ADC_SOFTWARE_START;
    hadc1.Init.ExternalTrigConvEdge     = ADC_EXTERNALTRIGCONVEDGE_NONE;
    hadc1.Init.ConversionDataManagement = ADC_CONVERSIONDATA_DR;
    hadc1.Init.Overrun                  = ADC_OVR_DATA_OVERWRITTEN;
    hadc1.Init.LeftBitShift             = ADC_LEFTBITSHIFT_NONE;
    hadc1.Init.OversamplingMode         = DISABLE;
    if (HAL_ADC_Init(&hadc1) != HAL_OK) { return HAL_ERROR; }

    if (HAL_ADCEx_Calibration_Start(&hadc1, ADC_CALIB_OFFSET, ADC_SINGLE_ENDED) != HAL_OK)
    {
        return HAL_ERROR;
    }

    ch.Channel      = ADC_CHANNEL_0;
    ch.Rank         = ADC_REGULAR_RANK_1;
    ch.SamplingTime = ADC_SAMPLETIME_64CYCLES_5;
    ch.SingleDiff   = ADC_SINGLE_ENDED;
    ch.OffsetNumber = ADC_OFFSET_NONE;
    ch.Offset       = 0;
    return HAL_ADC_ConfigChannel(&hadc1, &ch);
}

/* One conversion + light low-pass filter (3/4 old + 1/4 new) */
static void Adc_Update(void)
{
    if (gStats.adcOk == 0U) { return; }

    HAL_ADC_Start(&hadc1);
    if (HAL_ADC_PollForConversion(&hadc1, 5) == HAL_OK)
    {
        uint32_t raw = (uint32_t)HAL_ADC_GetValue(&hadc1);
        gStats.adcRaw = (gStats.adcRaw == 0U) ? raw : (((gStats.adcRaw * 3U) + raw) / 4U);
    }
    HAL_ADC_Stop(&hadc1);
}

static void InputTask(void *pvParameters)
{
    uint32_t last = 0;
    uint32_t now;
    uint32_t loop = 0;
    uint32_t wakeIdle, wakeLast = 0;
    (void)pvParameters;

    Dashboard_Log("FreeRTOS scheduler started, 7 tasks");
    if (gStats.adcOk == 0U) { Dashboard_Log("ADC init failed"); }

    /* Wakeup button is polled; whatever level it has now is "not pressed" */
    wakeIdle = (uint32_t)BSP_PB_GetState(BUTTON_WAKEUP);

    for (;;)
    {
        uint32_t w;

        now = (uint32_t)BSP_JOY_GetState(JOY1);
        gStats.joystick = now;

        if (now != last)
        {
            last = now;
            xQueueSend(joyQueue, &now, 0);
        }

        /* Wakeup button: two equal samples in a row = debounced */
        w = ((uint32_t)BSP_PB_GetState(BUTTON_WAKEUP) != wakeIdle) ? 1U : 0U;
        if ((w == wakeLast) && (w != gStats.wakePressed))
        {
            gStats.wakePressed = w;
            if (w != 0U)
            {
                gStats.wakeCount++;
                Dashboard_Log("Wakeup button pressed");
            }
        }
        wakeLast = w;

        /* Potentiometer every 100 ms */
        if ((++loop % 5U) == 0U)
        {
            Adc_Update();
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static void MemoryTask(void *pvParameters)
{
    uint32_t prevIdle = 0, prevTotal = 0, cpuHigh = 0, warned = 0;
    (void)pvParameters;
    Dashboard_Log("Heap and CPU monitor running");

    /* Self-test 1: is the DWT cycle counter really counting? */
    {
        uint32_t c1 = App_RunTimeCounter();
        for (volatile uint32_t k = 0; k < 1000U; k++) { }
        if (App_RunTimeCounter() == c1)
        {
            Dashboard_Log("DWT cycle counter NOT counting");
        }
    }
    for (;;)
    {
        UBaseType_t n;

        gStats.freeHeap    = (uint32_t)xPortGetFreeHeapSize();
        gStats.minFreeHeap = (uint32_t)xPortGetMinimumEverFreeHeapSize();

        /* CPU load = 100 % - share of time spent in the IDLE task (last 500 ms).
           Unsigned subtraction keeps this correct when the 32-bit counter wraps. */
        n = uxTaskGetSystemState(sTaskStat, TASK_COUNT + 3, NULL);
        if (n > 0)
        {
            uint32_t total = 0, idle = 0, dTot, dIdle;
            for (UBaseType_t i = 0; i < n; i++)
            {
                total += (uint32_t)sTaskStat[i].ulRunTimeCounter;
                if (strcmp(sTaskStat[i].pcTaskName, "IDLE") == 0)
                {
                    idle = (uint32_t)sTaskStat[i].ulRunTimeCounter;
                }
            }
            if ((total == 0U) && (warned == 0U))
            {
                /* Self-test 2: FreeRTOS gives all-zero run times = stats not enabled */
                warned = 1U;
                Dashboard_Log("Run-time stats OFF: check FreeRTOSConfig.h");
            }
            dTot  = total - prevTotal;
            dIdle = idle  - prevIdle;
            prevTotal = total;
            prevIdle  = idle;
            if (dTot != 0U)
            {
                uint32_t busy = (dTot > dIdle) ? (dTot - dIdle) : 0U;
                gStats.cpuLoad = (uint32_t)((((uint64_t)busy * 100U) + (dTot / 2U)) / dTot);
            }

            if ((cpuHigh == 0U) && (gStats.cpuLoad >= 80U))
            {
                cpuHigh = 1U;
                Dashboard_Log("Warning: CPU load above 80%");
            }
            else if ((cpuHigh != 0U) && (gStats.cpuLoad < 60U))
            {
                cpuHigh = 0U;
                Dashboard_Log("CPU load back to normal");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

static void WorkloadTask(void *pvParameters)
{
    volatile uint32_t sink = 0;
    (void)pvParameters;
    for (;;)
    {
        for (uint32_t i = 0; i < 200000; i++)
        {
            sink += i;
        }
        gStats.workloadCount++;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static void LedMonitorTask(void *pvParameters)
{
    uint32_t lastWork = 0;
    uint32_t pass = 0;
    (void)pvParameters;
    for (;;)
    {
        if ((++pass % 5) == 0)
        {
            BSP_LED_Toggle(LED_GREEN);
            gStats.ledGreen ^= 1U;          /* mirror for the dashboard */
        }

        if (gStats.workloadCount != lastWork)
        {
            lastWork = gStats.workloadCount;
            BSP_LED_Toggle(LED_ORANGE);
            gStats.ledOrange ^= 1U;
        }

        if (gStats.joystick != JOY_NONE)
        {
            BSP_LED_On(LED_BLUE);
            gStats.ledBlue = 1U;
        }
        else
        {
            BSP_LED_Off(LED_BLUE);
            gStats.ledBlue = 0U;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void BSP_PB_Callback(Button_TypeDef Button)
{
    BaseType_t woken = pdFALSE;

    if (Button == BUTTON_TAMPER)
    {
        xSemaphoreGiveFromISR(tamperSem, &woken);
    }
    portYIELD_FROM_ISR(woken);
}

static void ButtonTask(void *pvParameters)
{
    (void)pvParameters;
    for (;;)
    {
        if (xSemaphoreTake(tamperSem, portMAX_DELAY) == pdTRUE)
        {
            gStats.tamperCount++;
            Dashboard_Log("Tamper button pressed");
        }
    }
}

static void StatusTask(void *pvParameters)
{
    uint32_t n = 0;
    (void)pvParameters;
    for (;;)
    {
        if (xSemaphoreTake(lcdMutex, pdMS_TO_TICKS(100)) == pdTRUE)
        {
            Dashboard_DrawStatusTick(n++);
            xSemaphoreGive(lcdMutex);
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

static void DisplayTask(void *pvParameters)
{
    uint32_t ev;
    uint32_t pressCount = 0;
    uint32_t lastEvent = 0;
    TickType_t lastwake;
    (void)pvParameters;

    xSemaphoreTake(lcdMutex, portMAX_DELAY);
    Dashboard_DrawStatic();                 /* frames and labels: drawn once */
    xSemaphoreGive(lcdMutex);
    Dashboard_Log("Dashboard ready");

    lastwake = xTaskGetTickCount();
    for (;;)
    {
        while (xQueueReceive(joyQueue, &ev, 0) == pdPASS)
        {
            if (ev != 0)
            {
                pressCount++;
                lastEvent = ev;
                Dashboard_Log(JoyName(ev));
            }
        }

        if (xSemaphoreTake(lcdMutex, pdMS_TO_TICKS(100)) == pdTRUE)
        {
            Dashboard_Update(pressCount, lastEvent);
            xSemaphoreGive(lcdMutex);
        }

        vTaskDelayUntil(&lastwake, pdMS_TO_TICKS(200));
    }
}

/* ---- I2C slave: Raspberry Pi reads I2C_Data_t from us ---- */

/* Master addressed us. dir is from the MASTER's point of view. */
void HAL_I2C_AddrCallback(I2C_HandleTypeDef *h, uint8_t dir, uint16_t addr)
{
    (void)addr;
    if (dir == I2C_DIRECTION_RECEIVE)      /* master reads -> we transmit */
    {
        /* Freeze the frame at address-match so a task update can never be
           half-sent. Task writes happen inside a critical section, so this
           copy always sees a complete frame. */
        memcpy(i2cTxSnap, i2cTxBuf, sizeof(i2cTxSnap));
        HAL_I2C_Slave_Seq_Transmit_IT(h, i2cTxSnap, I2C_FRAME_LEN, I2C_FIRST_AND_LAST_FRAME);
    }
    else                                   /* master writes -> take up to 4 bytes, ignore */
    {
        HAL_I2C_Slave_Seq_Receive_IT(h, i2cRxBuf, I2C_RX_LEN, I2C_FIRST_AND_LAST_FRAME);
    }
}

/* Transfer finished: listen for the next one */
void HAL_I2C_ListenCpltCallback(I2C_HandleTypeDef *h)
{
    HAL_I2C_EnableListen_IT(h);
}

void HAL_I2C_ErrorCallback(I2C_HandleTypeDef *h)
{
	i2cErrCount++;
	i2cErrCode = HAL_I2C_GetError(h);
    /* AF (NACK) is normal: the master NACKs the last byte it reads.
       Whatever the error was, go back to listening. */
    HAL_I2C_EnableListen_IT(h);
}

/* Copy gStats into the buffer the ISR sends; critical section = no torn reads */
static void I2C_UpdateSnapshot(void)
{
    I2C_Data_t d;
    d.joystick      = gStats.joystick;
    d.tamperCount   = gStats.tamperCount;
    d.wakeCount     = gStats.wakeCount;
    d.adcRaw        = gStats.adcRaw;
    d.cpuLoad       = gStats.cpuLoad;
    d.workloadCount = gStats.workloadCount;

    uint8_t frame[I2C_FRAME_LEN];
    memset(frame, 0, sizeof(frame));
    memcpy(frame, &d, sizeof(d));
    for (uint32_t i = 0; i < I2C_FRAME_LEN - 1U; i++) { frame[I2C_FRAME_LEN - 1U] ^= frame[i]; }

    taskENTER_CRITICAL();
    memcpy(i2cTxBuf, frame, sizeof(frame));
    taskEXIT_CRITICAL();
}

static void I2CTask(void *pvParameters)
{
    (void)pvParameters;

    /* Arm listen mode only now: scheduler running, LCD already initialised. */
    if (HAL_I2C_EnableListen_IT(&hi2c1) != HAL_OK)
        Dashboard_Log("I2C listen FAILED");
    else
        Dashboard_Log("I2C listening on 0x2A");

    for (;;)
    {
        I2C_UpdateSnapshot();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void App_CreateTasks(void)
{
    BaseType_t ok = pdPASS;
    joyQueue = xQueueCreate(8, sizeof(uint32_t));
    if (joyQueue == NULL)
    {
        Error_Handler();
    }
    lcdMutex = xSemaphoreCreateMutex();
    if (lcdMutex == NULL)
    {
        Error_Handler();
    }
    tamperSem = xSemaphoreCreateBinary();
    if (tamperSem == NULL)
    {
        Error_Handler();
    }
    gStats.adcOk = (Adc_Init() == HAL_OK) ? 1U : 0U;    /* non-fatal: dashboard shows n/a */
    ok &= xTaskCreate(InputTask,      "Input",   INPUT_STACK_WORDS,    NULL, 4, &gTaskHandle[TASK_INPUT]);
    ok &= xTaskCreate(StatusTask,     "Status",  STATUS_STACK_WORDS,   NULL, 3, &gTaskHandle[TASK_STATUS]);
    ok &= xTaskCreate(ButtonTask,     "Button",  BUTTON_STACK_WORDS,   NULL, 3, &gTaskHandle[TASK_BUTTON]);
    ok &= xTaskCreate(DisplayTask,    "Display", DISPLAY_STACK_WORDS,  NULL, 2, &gTaskHandle[TASK_DISPLAY]);
    ok &= xTaskCreate(MemoryTask,     "Memory",  MEMORY_STACK_WORDS,   NULL, 2, &gTaskHandle[TASK_MEMORY]);
    ok &= xTaskCreate(WorkloadTask,   "Work",    WORKLOAD_STACK_WORDS, NULL, 1, &gTaskHandle[TASK_WORK]);
    ok &= xTaskCreate(LedMonitorTask, "LedMon",  LEDMON_STACK_WORDS,   NULL, 1, &gTaskHandle[TASK_LEDMON]);
    ok &= xTaskCreate(I2CTask,        "I2C",     256,                  NULL, 2, NULL);   /* not on dashboard */

    if (ok != pdPASS)
    {
        Error_Handler();
    }
    if (BSP_PB_Init(BUTTON_TAMPER, BUTTON_MODE_EXTI) != BSP_ERROR_NONE)
    {
        Error_Handler();
    }
    if (BSP_PB_Init(BUTTON_WAKEUP, BUTTON_MODE_GPIO) != BSP_ERROR_NONE)   /* polled in InputTask */
    {
        Error_Handler();
    }
}
