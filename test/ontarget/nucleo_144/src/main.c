/**
 ******************************************************************************
 * @file    UART/UART_TwoBoards_ComDMA/Src/main.c
 * @author  MCD Application Team
 * @brief   This sample code shows how to use UART HAL API to transmit
 *          and receive a data buffer with a communication process based on
 *          DMA transfer.
 *          The communication is done using 2 Boards.
 ******************************************************************************
 * @attention
 *
 * <h2><center>&copy; Copyright (c) 2017 STMicroelectronics.
 * All rights reserved.</center></h2>
 *
 * This software component is licensed by ST under BSD 3-Clause license,
 * the "License"; You may not use this file except in compliance with the
 * License. You may obtain a copy of the License at:
 *                        opensource.org/licenses/BSD-3-Clause
 *
 ******************************************************************************
 */

/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include <stdio.h>

extern int init_posix(void);

/** @addtogroup STM32H7xx_HAL_Examples
 * @{
 */

/** @addtogroup UART_TwoBoards_ComDMA
 * @{
 */

/* Private typedef -----------------------------------------------------------*/
/* Private define ------------------------------------------------------------*/
#define TRANSMITTER_BOARD

/* Private macro -------------------------------------------------------------*/
/* Private variables ---------------------------------------------------------*/
/* UART handler declaration */
UART_HandleTypeDef UartHandle;
__IO ITStatus UartReady        = RESET;
__IO uint32_t UserButtonStatus = 0; /* set to 1 after User Button interrupt  */

/* Buffer used for transmission :
   Size should be a Multiple of cache line size (32 bytes) */
ALIGN_32BYTES(uint8_t aTxBuffer[]) =
    " *****UART_TwoBoards communication based on DMA*****  *****UART_TwoBoards communication based on DMA*****   "
    "*****UART_TwoBoards communication based on DMA***** ";

/* Buffer used for reception :
   Size should be a Multiple of cache line size (32 bytes) */
ALIGN_32BYTES(uint8_t aRxBuffer[RXBUFFERSIZE]);

/* Private function prototypes -----------------------------------------------*/
static void     SystemClock_Config(void);
static void     Error_Handler(void);
static uint16_t Buffercmp(uint8_t* pBuffer1, uint8_t* pBuffer2, uint16_t BufferLength);
static void     CPU_CACHE_Enable(void);

/* Private functions ---------------------------------------------------------*/

/**
 * @brief  Main program
 * @param  None
 * @retval None
 */
int main(void)
{
    /* Enable the CPU Cache */
    CPU_CACHE_Enable();

    init_posix();

    /* STM32H7xx HAL library initialization:
         - Systick timer is configured by default as source of time base, but user
           can eventually implement his proper time base source (a general purpose
           timer for example or other time source), keeping in mind that Time base
           duration should be kept 1ms since PPP_TIMEOUT_VALUEs are defined and
           handled in milliseconds basis.
         - Set NVIC Group Priority to 4
         - Low Level Initialization
       */
    HAL_Init();

    /* Configure the system clock to 400 MHz */
    SystemClock_Config();

    /* Configure LED1, LED2 and LED3 */
    BSP_LED_Init(LED1);
    BSP_LED_Init(LED2);
    BSP_LED_Init(LED3);

    /*##-1- Configure the UART peripheral ######################################*/
    /* Put the USART peripheral in the Asynchronous mode (UART Mode) */
    /* UART configured as follows:
        - Word Length = 8 Bits
        - Stop Bit = One Stop bit
        - Parity = None
        - BaudRate = 9600 baud
        - Hardware flow control disabled (RTS and CTS signals) */
    UartHandle.Instance = USARTx;

    UartHandle.Init.BaudRate               = 9600;
    UartHandle.Init.WordLength             = UART_WORDLENGTH_8B;
    UartHandle.Init.StopBits               = UART_STOPBITS_1;
    UartHandle.Init.Parity                 = UART_PARITY_NONE;
    UartHandle.Init.HwFlowCtl              = UART_HWCONTROL_NONE;
    UartHandle.Init.Mode                   = UART_MODE_TX_RX;
    UartHandle.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
    if (HAL_UART_DeInit(&UartHandle) != HAL_OK)
    {
        Error_Handler();
    }
    if (HAL_UART_Init(&UartHandle) != HAL_OK)
    {
        Error_Handler();
    }

#ifdef TRANSMITTER_BOARD

    /* Configure User push-button in Interrupt mode */
    BSP_PB_Init(BUTTON_USER, BUTTON_MODE_EXTI);

    /* Wait for User push-button press before starting the Communication.
       In the meantime, LED1 is blinking */
    while (UserButtonStatus == 0)
    {
        /* Toggle LED1*/
        BSP_LED_Toggle(LED1);
        HAL_Delay(100);
    }

    BSP_LED_Off(LED1);

    /* The board sends the message and expects to receive it back */

    /*##-2- Start the transmission process #####################################*/
    /* While the UART in reception process, user can transmit data through
       "aTxBuffer" buffer */
    if (HAL_UART_Transmit_DMA(&UartHandle, (uint8_t*) aTxBuffer, TXBUFFERSIZE) != HAL_OK)
    {
        Error_Handler();
    }

    /*##-3- Wait for the end of the transfer ###################################*/
    while (UartReady != SET)
    {
    }

    /* Reset transmission flag */
    UartReady = RESET;

    /*##-4- Put UART peripheral in reception process ###########################*/
    if (HAL_UART_DeInit(&UartHandle) != HAL_OK)
    {
        Error_Handler();
    }
    if (HAL_UART_Init(&UartHandle) != HAL_OK)
    {
        Error_Handler();
    }
    if (HAL_UART_Receive_DMA(&UartHandle, (uint8_t*) aRxBuffer, RXBUFFERSIZE) != HAL_OK)
    {
        Error_Handler();
    }

#else

    /* The board receives the message and sends it back */

    /*##-2- Put UART peripheral in reception process ###########################*/
    if (HAL_UART_Receive_DMA(&UartHandle, (uint8_t*) aRxBuffer, RXBUFFERSIZE) != HAL_OK)
    {
        Error_Handler();
    }

    /*##-3- Wait for the end of the transfer ###################################*/
    /* While waiting for message to come from the other board, LED1 is
       blinking according to the following pattern: a double flash every half-second */
    while (UartReady != SET)
    {
        BSP_LED_On(LED1);
        HAL_Delay(100);
        BSP_LED_Off(LED1);
        HAL_Delay(100);
        BSP_LED_On(LED1);
        HAL_Delay(100);
        BSP_LED_Off(LED1);
        HAL_Delay(500);
    }

    /* Reset transmission flag */
    UartReady = RESET;
    BSP_LED_Off(LED1);

    /*##-4- Start the transmission process #####################################*/
    /* While the UART in reception process, user can transmit data through
       "aTxBuffer" buffer */
    if (HAL_UART_DeInit(&UartHandle) != HAL_OK)
    {
        Error_Handler();
    }
    if (HAL_UART_Init(&UartHandle) != HAL_OK)
    {
        Error_Handler();
    }
    if (HAL_UART_Transmit_DMA(&UartHandle, (uint8_t*) aTxBuffer, TXBUFFERSIZE) != HAL_OK)
    {
        Error_Handler();
    }

#endif /* TRANSMITTER_BOARD */

    /*##-5- Wait for the end of the transfer ###################################*/
    while (UartReady != SET)
    {
    }

    /* Reset transmission flag */
    UartReady = RESET;

    /* Invalidate cache prior to access by CPU */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-align"
    SCB_InvalidateDCache_by_Addr((uint32_t*) aRxBuffer, RXBUFFERSIZE);
#pragma GCC diagnostic pop

    /*##-6- Compare the sent and received buffers ##############################*/
    if (Buffercmp((uint8_t*) aTxBuffer, (uint8_t*) aRxBuffer, RXBUFFERSIZE))
    {
        Error_Handler();
    }

    /* Turn on LED1 if test passes then enter infinite loop */
    BSP_LED_On(LED1);

    /* Infinite loop */
    while (1)
    {
    }
}

/**
 * @brief  System Clock Configuration
 *         The system Clock is configured as follow :
 *            System Clock source            = PLL (HSE BYPASS)
 *            SYSCLK(Hz)                     = 400000000 (CPU Clock)
 *            HCLK(Hz)                       = 200000000 (AXI and AHBs Clock)
 *            AHB Prescaler                  = 2
 *            D1 APB3 Prescaler              = 2 (APB3 Clock  100MHz)
 *            D2 APB1 Prescaler              = 2 (APB1 Clock  100MHz)
 *            D2 APB2 Prescaler              = 2 (APB2 Clock  100MHz)
 *            D3 APB4 Prescaler              = 2 (APB4 Clock  100MHz)
 *            HSE Frequency(Hz)              = 8000000
 *            PLL_M                          = 4
 *            PLL_N                          = 400
 *            PLL_P                          = 2
 *            PLL_Q                          = 4
 *            PLL_R                          = 2
 *            VDD(V)                         = 3.3
 *            Flash Latency(WS)              = 4
 * @param  None
 * @retval None
 */
static void SystemClock_Config(void)
{
    RCC_ClkInitTypeDef RCC_ClkInitStruct;
    RCC_OscInitTypeDef RCC_OscInitStruct;
    HAL_StatusTypeDef  ret = HAL_OK;

    /*!< Supply configuration update enable */
    HAL_PWREx_ConfigSupply(PWR_LDO_SUPPLY);

    /* The voltage scaling allows optimizing the power consumption when the device is
       clocked below the maximum system frequency, to update the voltage scaling value
       regarding system frequency refer to product datasheet.  */
    __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

    while (!__HAL_PWR_GET_FLAG(PWR_FLAG_VOSRDY))
    {
    }

    /* Enable HSE Oscillator and activate PLL with HSE as source */
    RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
    RCC_OscInitStruct.HSEState       = RCC_HSE_BYPASS;
    RCC_OscInitStruct.HSIState       = RCC_HSI_OFF;
    RCC_OscInitStruct.CSIState       = RCC_CSI_OFF;
    RCC_OscInitStruct.PLL.PLLState   = RCC_PLL_ON;
    RCC_OscInitStruct.PLL.PLLSource  = RCC_PLLSOURCE_HSE;

    RCC_OscInitStruct.PLL.PLLM     = 4;
    RCC_OscInitStruct.PLL.PLLN     = 400;
    RCC_OscInitStruct.PLL.PLLFRACN = 0;
    RCC_OscInitStruct.PLL.PLLP     = 2;
    RCC_OscInitStruct.PLL.PLLR     = 2;
    RCC_OscInitStruct.PLL.PLLQ     = 4;

    RCC_OscInitStruct.PLL.PLLVCOSEL = RCC_PLL1VCOWIDE;
    RCC_OscInitStruct.PLL.PLLRGE    = RCC_PLL1VCIRANGE_2;
    ret                             = HAL_RCC_OscConfig(&RCC_OscInitStruct);
    if (ret != HAL_OK)
    {
        Error_Handler();
    }

    /* Select PLL as system clock source and configure  bus clocks dividers */
    RCC_ClkInitStruct.ClockType = (RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_D1PCLK1 |
                                   RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2 | RCC_CLOCKTYPE_D3PCLK1);

    RCC_ClkInitStruct.SYSCLKSource   = RCC_SYSCLKSOURCE_PLLCLK;
    RCC_ClkInitStruct.SYSCLKDivider  = RCC_SYSCLK_DIV1;
    RCC_ClkInitStruct.AHBCLKDivider  = RCC_HCLK_DIV2;
    RCC_ClkInitStruct.APB3CLKDivider = RCC_APB3_DIV2;
    RCC_ClkInitStruct.APB1CLKDivider = RCC_APB1_DIV2;
    RCC_ClkInitStruct.APB2CLKDivider = RCC_APB2_DIV2;
    RCC_ClkInitStruct.APB4CLKDivider = RCC_APB4_DIV2;
    ret                              = HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_4);
    if (ret != HAL_OK)
    {
        Error_Handler();
    }
}
/**
 * @brief  Tx Transfer completed callback
 * @param  UartHandle: UART handle.
 * @note   This example shows a simple way to report end of DMA Tx transfer, and
 *         you can add your own implementation.
 * @retval None
 */
void HAL_UART_TxCpltCallback(UART_HandleTypeDef* UartHandle)
{
    /* Set transmission flag: transfer complete */
    UartReady = SET;

    (void) UartHandle;
#ifdef TRANSMITTER_BOARD
    /* Turn LED2 on: Transfer in transmission process is correct */
    BSP_LED_On(LED2);
#else
    /* Turn LED2 off: Transfer in transmission process is correct */
    BSP_LED_Off(LED2);
#endif /* TRANSMITTER_BOARD */
}

/**
 * @brief  Rx Transfer completed callback
 * @param  UartHandle: UART handle
 * @note   This example shows a simple way to report end of DMA Rx transfer, and
 *         you can add your own implementation.
 * @retval None
 */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef* UartHandle)
{
    /* Set transmission flag: transfer complete */
    UartReady = SET;

    (void) UartHandle;
#ifdef TRANSMITTER_BOARD
    /* Turn LED2 off: Transfer in reception process is correct */
    BSP_LED_Off(LED2);
#else
    /* Turn LED2 on: Transfer in reception process is correct */
    BSP_LED_On(LED2);
#endif /* TRANSMITTER_BOARD */
}

/**
 * @brief  UART error callbacks
 * @param  UartHandle: UART handle
 * @note   This example shows a simple way to report transfer error, and you can
 *         add your own implementation.
 * @retval None
 */
void HAL_UART_ErrorCallback(UART_HandleTypeDef* UartHandle)
{
    (void) UartHandle;
    Error_Handler();
}

/**
 * @brief EXTI line detection callbacks
 * @param GPIO_Pin: Specifies the pins connected EXTI line
 * @retval None
 */
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
    if (GPIO_Pin == USER_BUTTON_PIN)
    {
        UserButtonStatus = 1;
    }
}

/**
 * @brief  Compares two buffers.
 * @param  pBuffer1, pBuffer2: buffers to be compared.
 * @param  BufferLength: buffer's length
 * @retval 0  : pBuffer1 identical to pBuffer2
 *         >0 : pBuffer1 differs from pBuffer2
 */
static uint16_t Buffercmp(uint8_t* pBuffer1, uint8_t* pBuffer2, uint16_t BufferLength)
{
    while (BufferLength--)
    {
        if ((*pBuffer1) != *pBuffer2)
        {
            return BufferLength;
        }
        pBuffer1++;
        pBuffer2++;
    }

    return 0;
}

/**
 * @brief  This function is executed in case of error occurrence.
 * @param  None
 * @retval None
 */
static void Error_Handler(void)
{
    /* Turn LED_RED on */
    BSP_LED_On(LED_RED);

    while (1)
    {
    }
}

#ifdef USE_FULL_ASSERT

/**
 * @brief  Reports the name of the source file and the source line number
 *         where the assert_param error has occurred.
 * @param  file: pointer to the source file name
 * @param  line: assert_param error line source number
 * @retval None
 */
void assert_failed(uint8_t* file, uint32_t line)
{
    /* User can add his own implementation to report the file name and line number,
       ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */

    /* Infinite loop */
    while (1)
    {
    }
}
#endif

/**
 * @brief  CPU L1-Cache enable.
 * @param  None
 * @retval None
 */
static void CPU_CACHE_Enable(void)
{
    /* Enable I-Cache */
    SCB_EnableICache();

    /* Enable D-Cache */
    SCB_EnableDCache();
}

/**
 * @}
 */

/**
 * @}
 */

/************************ (C) COPYRIGHT STMicroelectronics *****END OF FILE****/
