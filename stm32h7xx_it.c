/**
  ******************************************************************************
  * @file    Src/stm32h7xx_it.c
  * @brief   Interrupt Service Routines for the Snake game project.
  ******************************************************************************
  */

/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "stm32h7xx_it.h"
#include "FreeRTOS.h"
#include "task.h"

/******************************************************************************/
/*            Cortex-M7 Processor Exceptions Handlers                         */
/******************************************************************************/

void NMI_Handler(void)
{
}

void HardFault_Handler(void)
{
  while (1)
  {
  }
}

void MemManage_Handler(void)
{
  while (1)
  {
  }
}

void BusFault_Handler(void)
{
  while (1)
  {
  }
}

void UsageFault_Handler(void)
{
  while (1)
  {
  }
}



void DebugMon_Handler(void)
{
}



/**
  * @brief  SysTick Handler: drives HAL_GetTick() / HAL_Delay().
  */
extern void xPortSysTickHandler(void);

void SysTick_Handler(void)
{
  HAL_IncTick();
  if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED)
  {
    xPortSysTickHandler();
  }
}

/******************************************************************************/
/*                 STM32H7xx Peripherals Interrupt Handlers                   */
/******************************************************************************/

/**
  * @brief  EXTI lines 9 to 5.
  *         Only needed if the joystick is initialised with JOY_MODE_EXTI.
  *         With JOY_MODE_GPIO (polling) this interrupt never fires, so keeping
  *         it is harmless. Touch is polled in the game, so no TS handler here.
  */
void EXTI9_5_IRQHandler(void)
{
  BSP_JOY_IRQHandler(JOY1, JOY_ALL);
}

/**
  * @brief  EXTI lines 15 to 10: TAMPER push-button (pause / resume).
  */
void EXTI15_10_IRQHandler(void)
{
  BSP_PB_IRQHandler(BUTTON_TAMPER);
}

extern I2C_HandleTypeDef hi2c1;

void I2C1_EV_IRQHandler(void)
{
    if (hi2c1.Instance != NULL) { HAL_I2C_EV_IRQHandler(&hi2c1); }
}

void I2C1_ER_IRQHandler(void)
{
    if (hi2c1.Instance != NULL) { HAL_I2C_ER_IRQHandler(&hi2c1); }
}
