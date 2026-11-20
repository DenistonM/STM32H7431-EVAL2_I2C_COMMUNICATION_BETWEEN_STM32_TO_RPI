#include "main.h"
#include "FreeRTOS.h"
#include "task.h"
#include "app_tasks.h"

I2C_HandleTypeDef hi2c1;

static void SystemClock_Config(void);
static void MPU_Config(void);
static void CPU_CACHE_Enable(void);
static void I2C1_Init(void);

/* The EVAL board's LEDs and joystick sit on the MFX I/O expander, which is on
   I2C1 (PB6/PB7) - the SAME bus the Raspberry Pi uses. The BSP uses it only as
   a master, so the BSP may use I2C1 during LCD start-up only; afterwards it is
   released and becomes the Pi slave. */
extern int32_t BSP_I2C1_DeInit(void);

int main(void)
{
    MPU_Config();

    CPU_CACHE_Enable();

    HAL_Init();

    SystemClock_Config();

    /* Board LEDs / joystick are NOT initialised: they live on the MFX over I2C1. */

    /* Configure I2C1 peripheral clock */
    RCC_PeriphCLKInitTypeDef PeriphClkInitStruct = {0};

    PeriphClkInitStruct.PeriphClockSelection =
        RCC_PERIPHCLK_I2C123;

    PeriphClkInitStruct.I2c123ClockSelection =
        RCC_I2C123CLKSOURCE_D2PCLK1;

    if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInitStruct) != HAL_OK)
    {
        Error_Handler();
    }


    /* Initialize LCD */
    BSP_LCD_Init(0, LCD_ORIENTATION_LANDSCAPE);

    UTIL_LCD_SetFuncDriver(&LCD_Driver);

    /* LCD start-up is finished: hand I2C1 over to the Pi slave.
       Keep the Pi's SDA/SCL wires disconnected (or the Pi script stopped)
       until the dashboard is visible, because the BSP master used the bus
       a moment ago. */
    (void)BSP_I2C1_DeInit();
    I2C1_Init();

    /* Create FreeRTOS tasks */
    App_CreateTasks();

    /* Start FreeRTOS scheduler */
    vTaskStartScheduler();

    /* Reached only if scheduler fails */
    while (1)
    {
    }
}


void HAL_I2C_MspInit(I2C_HandleTypeDef *hi2c)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};

    if (hi2c->Instance == I2C1)
    {
        /* Enable GPIOB clock */
        __HAL_RCC_GPIOB_CLK_ENABLE();

        /* Enable I2C1 clock */
        __HAL_RCC_I2C1_CLK_ENABLE();
        /* Configure PB6 = SCL, PB7 = SDA (same settings as the working test) */
        GPIO_InitStruct.Pin = GPIO_PIN_6 | GPIO_PIN_7;
        GPIO_InitStruct.Mode = GPIO_MODE_AF_OD;
        GPIO_InitStruct.Pull = GPIO_PULLUP;
        GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
        GPIO_InitStruct.Alternate = GPIO_AF4_I2C1;
        HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

        /* Enable I2C1 interrupts */
        HAL_NVIC_SetPriority(I2C1_EV_IRQn, 5, 0);
        HAL_NVIC_EnableIRQ(I2C1_EV_IRQn);

        HAL_NVIC_SetPriority(I2C1_ER_IRQn, 5, 0);
        HAL_NVIC_EnableIRQ(I2C1_ER_IRQn);
    }
}


static void I2C1_Init(void)
{
    hi2c1.Instance = I2C1;

    /*
     * Timing must be calculated for the actual
     * I2C kernel clock and desired bus speed.
     * Do not use an arbitrary timing value.
     */

    hi2c1.Init.OwnAddress1 = (I2C_SLAVE_ADDR_7BIT << 1);
    hi2c1.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
    hi2c1.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
    hi2c1.Init.OwnAddress2 = 0;
    hi2c1.Init.OwnAddress2Masks = I2C_OA2_NOMASK;
    hi2c1.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
    hi2c1.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;

    /* Timing value proven to work with the Raspberry Pi in the "I2c testing"
       project (I2C1 kernel clock = 100 MHz). */
    hi2c1.Init.Timing = 0x00606092;

    if (HAL_I2C_Init(&hi2c1) != HAL_OK)
    {
        Error_Handler();
    }
    if (HAL_I2CEx_ConfigAnalogFilter(&hi2c1, I2C_ANALOGFILTER_ENABLE) != HAL_OK)
    {
        Error_Handler();
    }
    if (HAL_I2CEx_ConfigDigitalFilter(&hi2c1, 0) != HAL_OK)
    {
        Error_Handler();
    }
}
/* ---- Everything below is copied unchanged from the ST BSP example ---- */

static void SystemClock_Config(void)
{
  RCC_ClkInitTypeDef RCC_ClkInitStruct;
  RCC_OscInitTypeDef RCC_OscInitStruct;
  HAL_StatusTypeDef ret = HAL_OK;

  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);
  while ((PWR->D3CR & (PWR_D3CR_VOSRDY)) != PWR_D3CR_VOSRDY) {}

  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.HSIState = RCC_HSI_OFF;
  RCC_OscInitStruct.CSIState = RCC_CSI_OFF;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;

  RCC_OscInitStruct.PLL.PLLM = 5;
  RCC_OscInitStruct.PLL.PLLN = 160;
  RCC_OscInitStruct.PLL.PLLFRACN = 0;
  RCC_OscInitStruct.PLL.PLLP = 2;
  RCC_OscInitStruct.PLL.PLLR = 2;
  RCC_OscInitStruct.PLL.PLLQ = 4;

  RCC_OscInitStruct.PLL.PLLVCOSEL = RCC_PLL1VCOWIDE;
  RCC_OscInitStruct.PLL.PLLRGE = RCC_PLL1VCIRANGE_2;
  ret = HAL_RCC_OscConfig(&RCC_OscInitStruct);
  if (ret != HAL_OK) { Error_Handler(); }

  RCC_ClkInitStruct.ClockType = (RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_D1PCLK1 | RCC_CLOCKTYPE_PCLK1 | \
                                 RCC_CLOCKTYPE_PCLK2  | RCC_CLOCKTYPE_D3PCLK1);

  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.SYSCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB3CLKDivider = RCC_APB3_DIV2;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_APB1_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_APB2_DIV2;
  RCC_ClkInitStruct.APB4CLKDivider = RCC_APB4_DIV2;
  ret = HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_4);
  if (ret != HAL_OK) { Error_Handler(); }

  __HAL_RCC_CSI_ENABLE();
  __HAL_RCC_SYSCFG_CLK_ENABLE();
  HAL_EnableCompensationCell();
}

void Error_Handler(void)
{
  /* No LED here: the LEDs are on the MFX (I2C1), which is now the Pi bus. */
  __disable_irq();
  while (1) {}
}

#ifdef USE_FULL_ASSERT
void assert_failed(uint8_t *file, uint32_t line)
{
  while (1) {}
}
#endif

static void MPU_Config(void)
{
  MPU_Region_InitTypeDef MPU_InitStruct;

  HAL_MPU_Disable();

  /* Strongly ordered / no access for undefined regions */
  MPU_InitStruct.Enable = MPU_REGION_ENABLE;
  MPU_InitStruct.BaseAddress = 0x00;
  MPU_InitStruct.Size = MPU_REGION_SIZE_4GB;
  MPU_InitStruct.AccessPermission = MPU_REGION_NO_ACCESS;
  MPU_InitStruct.IsBufferable = MPU_ACCESS_NOT_BUFFERABLE;
  MPU_InitStruct.IsCacheable = MPU_ACCESS_NOT_CACHEABLE;
  MPU_InitStruct.IsShareable = MPU_ACCESS_SHAREABLE;
  MPU_InitStruct.Number = MPU_REGION_NUMBER0;
  MPU_InitStruct.TypeExtField = MPU_TEX_LEVEL0;
  MPU_InitStruct.SubRegionDisable = 0x87;
  MPU_InitStruct.DisableExec = MPU_INSTRUCTION_ACCESS_DISABLE;
  HAL_MPU_ConfigRegion(&MPU_InitStruct);

  /* SDRAM: write-through */
  MPU_InitStruct.Enable = MPU_REGION_ENABLE;
  MPU_InitStruct.BaseAddress = SDRAM_DEVICE_ADDR;
  MPU_InitStruct.Size = MPU_REGION_SIZE_32MB;
  MPU_InitStruct.AccessPermission = MPU_REGION_FULL_ACCESS;
  MPU_InitStruct.IsBufferable = MPU_ACCESS_NOT_BUFFERABLE;
  MPU_InitStruct.IsCacheable = MPU_ACCESS_CACHEABLE;
  MPU_InitStruct.IsShareable = MPU_ACCESS_NOT_SHAREABLE;
  MPU_InitStruct.Number = MPU_REGION_NUMBER1;
  MPU_InitStruct.TypeExtField = MPU_TEX_LEVEL0;
  MPU_InitStruct.SubRegionDisable = 0x00;
  MPU_InitStruct.DisableExec = MPU_INSTRUCTION_ACCESS_ENABLE;
  HAL_MPU_ConfigRegion(&MPU_InitStruct);

  /* NOR and SRAM regions from the ST example were removed: the game does not use them */

  HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);
}

static void CPU_CACHE_Enable(void)
{
  SCB_EnableICache();
  SCB_EnableDCache();
}

/* ---- Minimal C-library stubs ----
   The game does no file or console I/O. Defining these here removes the linker
   warnings "_close is not implemented and will always fail" and similar. */
#include <errno.h>
#include <sys/stat.h>

int  _close(int fd)                       { (void)fd; return -1; }
int  _fstat(int fd, struct stat *st)      { (void)fd; st->st_mode = S_IFCHR; return 0; }
int  _getpid(void)                        { return 1; }
int  _isatty(int fd)                      { (void)fd; return 1; }
int  _kill(int pid, int sig)              { (void)pid; (void)sig; errno = EINVAL; return -1; }
long _lseek(int fd, long off, int whence) { (void)fd; (void)off; (void)whence; return 0; }
int  _read(int fd, char *buf, int len)    { (void)fd; (void)buf; (void)len; return 0; }
int  _write(int fd, char *buf, int len)   { (void)fd; (void)buf; return len; }
