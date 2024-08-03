/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    lora_app.c
  * @author  MCD Application Team
  * @brief   Application of the LRWAN Middleware
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2021 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "platform.h"
#include "sys_app.h"
#include "lora_app.h"
#include "stm32_seq.h"
#include "stm32_timer.h"
#include "utilities_def.h"
#include "app_version.h"
#include "lorawan_version.h"
#include "subghz_phy_version.h"
#include "lora_info.h"
#include "LmHandler.h"
#include "adc_if.h"
#include "CayenneLpp.h"
#include "sys_sensors.h"
#include "flash_if.h"

/* USER CODE BEGIN Includes */
#include <stdio.h>
#include "i2c.h"
#include <stdlib.h>
#include <stdbool.h>
#include "usart.h"
#include "LoRaMacCrypto.h"
#include "project_config.h"

#include "flash_if.h"
#include "PWX_ST50H_Modbus.h"
/* USER CODE END Includes */

/* External variables ---------------------------------------------------------*/
/* USER CODE BEGIN EV */

/**
  * @brief LoRaWAN handler parameters for Confirmed Uplink
  */
static LmHandlerParams_t LmHandlerParamsConfirmed =
{
  .ActiveRegion =             ACTIVE_REGION,
  .DefaultClass =             LORAWAN_DEFAULT_CLASS,
  .AdrEnable =                LORAWAN_ADR_STATE,
  .IsTxConfirmed =            LORAMAC_HANDLER_CONFIRMED_MSG,
  .TxDatarate =               LORAWAN_DEFAULT_DATA_RATE,
  .TxPower =                  LORAWAN_DEFAULT_TX_POWER,
  .PingSlotPeriodicity =      LORAWAN_DEFAULT_PING_SLOT_PERIODICITY,
  .RxBCTimeout =              LORAWAN_DEFAULT_CLASS_B_C_RESP_TIMEOUT
};

//int MAX_WATER_LEVEL_SAMPLES 	= 1;
//int SAMPLE_INTERVAL_MS          = 60000;  	// 1 minute/s in milliseconds
//int TRANSMIT_INTERVAL_MS        = 60000; 	// 1 minute/s in milliseconds

bool hasJoined 					= false;
int transmissionType			= 0;		// 0: Scheduled	; 1: Unscheduled ; 2: System Diagnostic
bool skipScheduledTransmission	= false;
bool sendSystemDiagnostic		= false;
bool isLevelBreached			= false;
bool isModbusDefective			= false;
uint8_t systemDiagnostic 		= 0;

//! LTC Variables
float VBAT 					= 0.0;
float VIN 					= 0.0;
float VSYS 					= 0.0;
float IBAT 					= 0.0;
float IIN 					= 0.0;
float ICHARGE_DAC 			= 0.0;
uint16_t SYSTEM_STATUS 		= 0.0;
bool isInit  				= true;


//! Water Level 20m Variables
int readingCount 				= 10;
int continuousCounter			= 0;	// Should reach 2 before going back to normal
bool continuousMode				= false;

float waterLevel 				= 0.0;
float waterLevelMin				= 0.0;
float waterLevelMax				= 0.0;
float waterLevelLatest			= 0.0;
float rawLevelVal 				= 0.0;
int sampleIndex 				= 0;

unsigned long lastSampleTime 	= 0;
unsigned long lastTransmitTime 	= 0;
unsigned long currentTime		= 0;

ModBus_t ModbusResp;
float * waterLevelSamples;    //waterLevelSamples = (int*)malloc(sizeof(int)*MAX_WATER_LEVEL_SAMPLES);

//Flash Memory
int getDevnonce 				= 0;
uint32_t getAddress 			= 0;
bool devNonceInitialized 		= false;

//TX Process
bool justTransmitted 			= false;
bool sendFlag 					= false;
bool doneScanning 				= false;
bool _doneScanning 				= false;

/* USER CODE END EV */

/* Private typedef -----------------------------------------------------------*/
/**
  * @brief LoRa State Machine states
  */
typedef enum TxEventType_e
{
  /**
    * @brief Appdata Transmission issue based on timer every TxDutyCycleTime
    */
  TX_ON_TIMER,
  /**
    * @brief Appdata Transmission external event plugged on OnSendEvent( )
    */
  TX_ON_EVENT
  /* USER CODE BEGIN TxEventType_t */
  /* USER CODE END TxEventType_t */
} TxEventType_t;

/* USER CODE BEGIN PTD */


/**
 * Function that writes Devnonce variable to flash (Not Yet Working!)
 */
FLASH_IF_StatusTypedef write_devnonce_to_flash(uint32_t devnonce) {
    FLASH_IF_StatusTypedef status;
    uint32_t page_start_address = (DEVNONCE_FLASH_ADDRESS / FLASH_PAGE_SIZE) * FLASH_PAGE_SIZE;

    APP_LOG(TS_OFF, VLEVEL_M, "\r\n WRITING DEVNONCE TO FLASH \r\n");

    if (FLASH_IF_Init(NULL) != FLASH_IF_OK) {
        APP_LOG(TS_OFF, VLEVEL_M, "\r\n Error: Flash failed to initialize \r\n");
        return FLASH_IF_ERROR;
    }

    bool devnonceWritten = false;

    //! Iterate through the addresses in the page
    for (uint32_t address = page_start_address; address < page_start_address + FLASH_PAGE_SIZE; address += sizeof(uint64_t)) {
        uint64_t data;

        // Read data from current address
        status = FLASH_IF_Read(&data, (const void *)address, sizeof(uint64_t));
        if (status != FLASH_IF_OK) {
            APP_LOG(TS_OFF, VLEVEL_M, "Error: Flash read failed at 0x%x\r\n", address);
            return status;
        }

        //! Check if the data in the given address is empty (all bits are set to 1)
        if (data == UINT64_MAX) {
            //! Write devnonce to the current address
            status = FLASH_IF_Write((void *)address, &devnonce, sizeof(uint64_t));
            if (status != FLASH_IF_OK) {
                APP_LOG(TS_OFF, VLEVEL_M, "Error: Flash write failed at 0x%x\r\n", address);
                return status;
            } else {
            	APP_LOG(TS_OFF, VLEVEL_M, "Success: Flash write done at address 0x%x\r\n", address);
                devnonceWritten = true;
                break;
            }
        }
    }

    //! Erase entire page if all addresses are occupied
    if (!devnonceWritten) {
        APP_LOG(TS_OFF, VLEVEL_M, "\r\nErasing entire page");
        status = FLASH_IF_Erase((void *)page_start_address, FLASH_PAGE_SIZE);
        if (status != FLASH_IF_OK) {
            APP_LOG(TS_OFF, VLEVEL_M, "\r\nError: Flash erase failed\r\n");
            return status;
        }

        //! Retry writing devnonce after erasing the page
        status = write_devnonce_to_flash(devnonce);
        if (status != FLASH_IF_OK) {
            APP_LOG(TS_OFF, VLEVEL_M, "Error: Failed to write devnonce after erasing the page\r\n");
            return status;
        }
    }

    return FLASH_IF_OK;
}

/**
 *  Function that checks if Modbus sensor is working or not
 */

void CheckModbus(void){
	int defective = 0;
	for (int i = 0; i < 10; i++) {
	    if(waterLevelSamples[i] <= 0.0){
	    	defective += 1;
	    }
	}
	if (defective >= 10){
		isModbusDefective = true;
		 APP_LOG(TS_OFF, VLEVEL_M, "Error: Modbus Sensor Unresponsive\r\n");
	} else {
		isModbusDefective = false;
		APP_LOG(TS_OFF, VLEVEL_M,  "Modbus Sensor is Responsive\r\n");
	}
}


/**
 *  Function that parses modbus reply to water level distance value
 * Response: Distance Value [Float]
 */

float parseReply(uint8_t *response) {
    uint8_t highByte = response[3];
    uint8_t lowByte = response[4];

    uint16_t combinedValue = (highByte << 8) | lowByte;
    float distanceVal = (float)combinedValue/1000;

    return distanceVal;
}

/**
 *  Function to sort an array of floats
 * Response: [None]
 */
void sortArray(float *array, int size) {
    for (int i = 0; i < size - 1; i++) {
        for (int j = i + 1; j < size; j++) {
            if (array[i] > array[j]) {
                float temp = array[i];
                array[i] = array[j];
                array[j] = temp;
            }
        }
    }
}

/**
 *  Function to get the median value from a sorted array of floats
 * Response: [Array]
 */
float getMedianValue(float *array, int size) {
    if (size % 2 == 0) {
        return (array[size / 2 - 1] + array[size / 2]) / 2.0;
    } else {
        return array[size / 2];
    }
}

/**
 *  Function to get the average value from a sorted array of floats
 * Response:
 */
float getAverageValue(float *array, int size) {
    float sum = 0.0;
    for (int i = 0; i < size; i++) {
        sum += array[i];
    }
    return sum / size;
}

/**
 *  Function to average all water level readings before heartbeat
 *
 */
float averageWaterLevel() {
    float sum = 0.0;
    for (int i = 0; i < MAX_WATER_LEVEL_SAMPLES; i++) {
        sum += waterLevelSamples[i];
    }
    return sum / MAX_WATER_LEVEL_SAMPLES;
}

/**
 *  Single-shot water level distance value
 *
 */
void fetchSingleShotData() {
	HAL_GPIO_WritePin(GPIOA, GPIO_PIN_6, GPIO_PIN_SET);
	HAL_GPIO_WritePin(GPIOA, GPIO_PIN_7, GPIO_PIN_SET);

	HAL_Delay(1500);
	uint8_t msg[100]; //! Buffer for storing messages to be transmitted via UART
	uint8_t ModbusCommand[8] = {0x01,0x03,0x00,0x03,0x00,0x01,0x74,0x0A};
	uint16_t CommandSize = sizeof(ModbusCommand) / sizeof(ModbusCommand[0]);

	APP_LOG(TS_OFF, VLEVEL_M, " Data Fetch:\r\n");
	sendRaw(ModbusCommand, CommandSize, &ModbusResp);
	HAL_Delay(250);

	APP_LOG(TS_OFF, VLEVEL_M, " MODBUS RESPONSE (Hex): ");
	for (int x = 0; x < ModbusResp.rxIndex; x++){
		APP_LOG(TS_OFF, VLEVEL_M, "%02X ", ModbusResp.buffer[x]);
	}

	rawLevelVal = parseReply(ModbusResp.buffer);

	//APP_LOG(TS_OFF, VLEVEL_M, " Water Level: %d\r\n", rawLevelVal);
	sprintf((char*)msg, "Water Level: %f \r\n", rawLevelVal);
	HAL_UART_Transmit(&huart2, msg, strlen((char*)msg), 1000);

	APP_LOG(TS_OFF, VLEVEL_M, " \r\n");
	HAL_Delay(1000);

	HAL_GPIO_WritePin(GPIOA, GPIO_PIN_6, GPIO_PIN_RESET);
	HAL_GPIO_WritePin(GPIOA, GPIO_PIN_7, GPIO_PIN_RESET);
}


/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/**
  * LEDs period value of the timer in ms
  */
#define LED_PERIOD_TIME 500

/**
  * Join switch period value of the timer in ms
  */
#define JOIN_TIME 2000

/*---------------------------------------------------------------------------*/
/*                             LoRaWAN NVM configuration                     */
/*---------------------------------------------------------------------------*/
/**
  * @brief LoRaWAN NVM Flash address
  * @note last 2 sector of a 128kBytes device
  */
#define LORAWAN_NVM_BASE_ADDRESS                    ((void *)0x0803F000UL)

/* USER CODE BEGIN PD */
static const char *slotStrings[] = { "1", "2", "C", "C_MC", "P", "P_MC" };
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private function prototypes -----------------------------------------------*/
/**
  * @brief  LoRa End Node send request
  */
static void SendTxData(void);

/**
  * @brief  TX timer callback function
  * @param  context ptr of timer context
  */
static void OnTxTimerEvent(void *context);

/**
  * @brief  join event callback function
  * @param  joinParams status of join
  */
static void OnJoinRequest(LmHandlerJoinParams_t *joinParams);

/**
  * @brief callback when LoRaWAN application has sent a frame
  * @brief  tx event callback function
  * @param  params status of last Tx
  */
static void OnTxData(LmHandlerTxParams_t *params);

/**
  * @brief callback when LoRaWAN application has received a frame
  * @param appData data received in the last Rx
  * @param params status of last Rx
  */
static void OnRxData(LmHandlerAppData_t *appData, LmHandlerRxParams_t *params);

/**
  * @brief callback when LoRaWAN Beacon status is updated
  * @param params status of Last Beacon
  */
static void OnBeaconStatusChange(LmHandlerBeaconParams_t *params);

/**
  * @brief callback when system time has been updated
  */
static void OnSysTimeUpdate(void);

/**
  * @brief callback when LoRaWAN application Class is changed
  * @param deviceClass new class
  */
static void OnClassChange(DeviceClass_t deviceClass);

/**
  * @brief  LoRa store context in Non Volatile Memory
  */
static void StoreContext(void);

/**
  * @brief  stop current LoRa execution to switch into non default Activation mode
  */
static void StopJoin(void);

/**
  * @brief  Join switch timer callback function
  * @param  context ptr of Join switch context
  */
static void OnStopJoinTimerEvent(void *context);

/**
  * @brief  Notifies the upper layer that the NVM context has changed
  * @param  state Indicates if we are storing (true) or restoring (false) the NVM context
  */
static void OnNvmDataChange(LmHandlerNvmContextStates_t state);

/**
  * @brief  Store the NVM Data context to the Flash
  * @param  nvm ptr on nvm structure
  * @param  nvm_size number of data bytes which were stored
  */
static void OnStoreContextRequest(void *nvm, uint32_t nvm_size);

/**
  * @brief  Restore the NVM Data context from the Flash
  * @param  nvm ptr on nvm structure
  * @param  nvm_size number of data bytes which were restored
  */
static void OnRestoreContextRequest(void *nvm, uint32_t nvm_size);

/**
  * Will be called each time a Radio IRQ is handled by the MAC layer
  *
  */
static void OnMacProcessNotify(void);

/**
  * @brief Change the periodicity of the uplink frames
  * @param periodicity uplink frames period in ms
  * @note Compliance test protocol callbacks
  */
static void OnTxPeriodicityChanged(uint32_t periodicity);

/**
  * @brief Change the confirmation control of the uplink frames
  * @param isTxConfirmed Indicates if the uplink requires an acknowledgement
  * @note Compliance test protocol callbacks
  */
static void OnTxFrameCtrlChanged(LmHandlerMsgTypes_t isTxConfirmed);

/**
  * @brief Change the periodicity of the ping slot frames
  * @param pingSlotPeriodicity ping slot frames period in ms
  * @note Compliance test protocol callbacks
  */
static void OnPingSlotPeriodicityChanged(uint8_t pingSlotPeriodicity);

/**
  * @brief Will be called to reset the system
  * @note Compliance test protocol callbacks
  */
static void OnSystemReset(void);

/* USER CODE BEGIN PFP */

/**
  * @brief  LED Tx timer callback function
  * @param  context ptr of LED context
  */
static void OnTxTimerLedEvent(void *context);

/**
  * @brief  LED Rx timer callback function
  * @param  context ptr of LED context
  */
static void OnRxTimerLedEvent(void *context);

/**
  * @brief  LED Join timer callback function
  * @param  context ptr of LED context
  */
static void OnJoinTimerLedEvent(void *context);

/* USER CODE END PFP */

/* Private variables ---------------------------------------------------------*/
/**
  * @brief LoRaWAN default activation type
  */
static ActivationType_t ActivationType = LORAWAN_DEFAULT_ACTIVATION_TYPE;

/**
  * @brief LoRaWAN force rejoin even if the NVM context is restored
  */
static bool ForceRejoin = LORAWAN_FORCE_REJOIN_AT_BOOT;

/**
  * @brief LoRaWAN handler Callbacks
  */
static LmHandlerCallbacks_t LmHandlerCallbacks =
{
  .GetBatteryLevel =              GetBatteryLevel,
  .GetTemperature =               GetTemperatureLevel,
  .GetUniqueId =                  GetUniqueId,
  .GetDevAddr =                   GetDevAddr,
  .OnRestoreContextRequest =      OnRestoreContextRequest,
  .OnStoreContextRequest =        OnStoreContextRequest,
  .OnMacProcess =                 OnMacProcessNotify,
  .OnNvmDataChange =              OnNvmDataChange,
  .OnJoinRequest =                OnJoinRequest,
  .OnTxData =                     OnTxData,
  .OnRxData =                     OnRxData,
  .OnBeaconStatusChange =         OnBeaconStatusChange,
  .OnSysTimeUpdate =              OnSysTimeUpdate,
  .OnClassChange =                OnClassChange,
  .OnTxPeriodicityChanged =       OnTxPeriodicityChanged,
  .OnTxFrameCtrlChanged =         OnTxFrameCtrlChanged,
  .OnPingSlotPeriodicityChanged = OnPingSlotPeriodicityChanged,
  .OnSystemReset =                OnSystemReset,
};

/**
  * @brief LoRaWAN handler parameters
  */
static LmHandlerParams_t LmHandlerParams =
{
  .ActiveRegion =             ACTIVE_REGION,
  .DefaultClass =             LORAWAN_DEFAULT_CLASS,
  .AdrEnable =                LORAWAN_ADR_STATE,
  .IsTxConfirmed =            LORAWAN_DEFAULT_CONFIRMED_MSG_STATE,
  .TxDatarate =               LORAWAN_DEFAULT_DATA_RATE,
  .TxPower =                  LORAWAN_DEFAULT_TX_POWER,
  .PingSlotPeriodicity =      LORAWAN_DEFAULT_PING_SLOT_PERIODICITY,
  .RxBCTimeout =              LORAWAN_DEFAULT_CLASS_B_C_RESP_TIMEOUT
};

/**
  * @brief Type of Event to generate application Tx
  */
static TxEventType_t EventType = TX_ON_TIMER;//TX_ON_EVENT;////

/**
  * @brief Timer to handle the application Tx
  */
static UTIL_TIMER_Object_t TxTimer;

/**
  * @brief Tx Timer period
  */
static UTIL_TIMER_Time_t TxPeriodicity = APP_TX_DUTYCYCLE;

/**
  * @brief Join Timer period
  */
static UTIL_TIMER_Object_t StopJoinTimer;

/* USER CODE BEGIN PV */
/**
  * @brief User application buffer
  */
static uint8_t AppDataBuffer[LORAWAN_APP_DATA_BUFFER_MAX_SIZE];

/**
  * @brief User application data structure
  */
static LmHandlerAppData_t AppData = { 0, 0, AppDataBuffer };

/**
  * @brief Specifies the state of the application LED
  */
static uint8_t AppLedStateOn = RESET;

/**
  * @brief Timer to handle the application Tx Led to toggle
  */
static UTIL_TIMER_Object_t TxLedTimer;

/**
  * @brief Timer to handle the application Rx Led to toggle
  */
static UTIL_TIMER_Object_t RxLedTimer;

/**
  * @brief Timer to handle the application Join Led to toggle
  */
static UTIL_TIMER_Object_t JoinLedTimer;

/* USER CODE END PV */

/* Exported functions ---------------------------------------------------------*/

/* USER CODE BEGIN EF */

/**
 * PANIC Function to reset device configuration to default
 */
void panicMode(void){
	SecureElementNvmData_t FlashNVM;

	APP_LOG(TS_OFF, VLEVEL_M, "\r\n==============================================\r\n");
	APP_LOG(TS_OFF, VLEVEL_M, "\r\n 		[!]	I F*CKED UP MODE [!]              \r\n");
	APP_LOG(TS_OFF, VLEVEL_M, "\r\n==============================================\r\n");
	APP_LOG(TS_OFF, VLEVEL_M, "\r\n Reverting all device configuration to default \r\n");

	if (FLASH_IF_Read(&FlashNVM, LORAWAN_NVM_BASE_ADDRESS, sizeof(FlashNVM)) == FLASH_IF_OK) {
		FlashNVM.pwxWaterLevelThresholdHigh = (uint16_t)15.0;
		thresholdLevelHigh = 15.0;
		HAL_Delay(50);
		FlashNVM.pwxWaterLevelThresholdLow = (uint16_t)0.0;
		thresholdLevelLow = 0.0;
		HAL_Delay(50);
		FlashNVM.pwxTxInterval = (uint64_t)900000;
		TRANSMIT_INTERVAL_MS = 900000;
		HAL_Delay(50);
		FlashNVM.pwxCnfUplinkCount = (uint16_t)4;
		MAX_UPLINK_BEFORE_CONFIRMED = 4;
		confUplinkCounter = 0;
		HAL_Delay(50);
		FlashNVM.pwxSamplingCount = (uint16_t)5;
		MAX_WATER_LEVEL_SAMPLES = 5;
    	waterLevelSamples = (float*)malloc(sizeof(int)*MAX_WATER_LEVEL_SAMPLES);
    	SAMPLE_INTERVAL_MS = (TRANSMIT_INTERVAL_MS/MAX_WATER_LEVEL_SAMPLES);
	} else {
		APP_LOG(TS_OFF, VLEVEL_M, "FAILED READING FLASH \r\n");
	}

	/* Save to NVM */
	if (FLASH_IF_Erase(LORAWAN_NVM_BASE_ADDRESS, FLASH_PAGE_SIZE) == FLASH_IF_OK){
		if(FLASH_IF_Write(LORAWAN_NVM_BASE_ADDRESS, &FlashNVM, sizeof(FlashNVM)) == FLASH_IF_OK){
			APP_LOG(TS_OFF, VLEVEL_M, "###### Success Saving to Flash \r\n");
		}else{
			APP_LOG(TS_OFF, VLEVEL_M, "###### Error Saving to Flash \r\n");
		}
	}else{
		APP_LOG(TS_OFF, VLEVEL_M, "###### Error Erasing Flash \r\n");
	}

	HAL_Delay(500);
	HAL_NVIC_SystemReset();
	APP_LOG(TS_OFF, VLEVEL_M, "\r\n==============================================\r\n");
}

/**
 *  Initial water level distance value fetch
 *
 */
void fetchSensorDataOnce() {
	HAL_GPIO_WritePin(GPIOA, GPIO_PIN_6, GPIO_PIN_SET);
	HAL_GPIO_WritePin(GPIOA, GPIO_PIN_7, GPIO_PIN_SET);

	HAL_Delay(1500);
	uint8_t msg[100]; // Buffer for storing messages to be transmitted via UART
	uint8_t ModbusCommand[8] = {0x01,0x03,0x00,0x03,0x00,0x01,0x74,0x0A};
	uint16_t CommandSize = sizeof(ModbusCommand) / sizeof(ModbusCommand[0]);
	float waterLevels[readingCount];

	APP_LOG(TS_OFF, VLEVEL_M, "\r\nINITIAL DATA FETCH\r\n");

	for(int i = 0; i < 10;  i++)	{
		APP_LOG(TS_OFF, VLEVEL_M, " Data Fetch: %d\r\n", i+1);
		sendRaw(ModbusCommand, CommandSize, &ModbusResp);
		HAL_Delay(250);

		APP_LOG(TS_OFF, VLEVEL_M, " MODBUS RESPONSE (Hex): ");
		for (int x = 0; x < ModbusResp.rxIndex; x++){
			APP_LOG(TS_OFF, VLEVEL_M, "%02X ", ModbusResp.buffer[x]);
			HAL_Delay(50);
		}

		rawLevelVal = parseReply(ModbusResp.buffer);
		waterLevels[i] = rawLevelVal;
		HAL_Delay(50);
		//APP_LOG(TS_OFF, VLEVEL_M, " Water Level: %d\r\n", rawLevelVal);
		sprintf((char*)msg, "\r\nWater Level: %f \r\n", rawLevelVal);
		HAL_UART_Transmit(&huart2, msg, strlen((char*)msg), 1000);

		APP_LOG(TS_OFF, VLEVEL_M, " \r\n");
		HAL_Delay(1000);
	}

	sortArray(waterLevels, 10);

	if(samplingMethod == 0){
		waterLevel = getMedianValue(waterLevels, 10);
	} else {
		waterLevel = getAverageValue(waterLevels, 10);
	}

	waterLevelLatest = waterLevel;
	if(waterLevelMin == 0 && waterLevelMax == 0){
		waterLevelMin = waterLevelMax = waterLevel;
	}

	if(waterLevel < waterLevelMin){
		waterLevelMin = waterLevel;
	} else if(waterLevel > waterLevelMax){
		waterLevelMax = waterLevel;
	}

	sprintf((char*)msg, "Final Water Level: %f \r\n", waterLevel);
	HAL_UART_Transmit(&huart2, msg, strlen((char*)msg), 1000);

	waterLevelSamples[sampleIndex] = waterLevel;
	sampleIndex += 1;
}


/* Function to get water level differential value
 *
 */
void fetchSensorDataDifferential(void) {

	HAL_GPIO_WritePin(GPIOA, GPIO_PIN_6, GPIO_PIN_SET);
	HAL_GPIO_WritePin(GPIOA, GPIO_PIN_7, GPIO_PIN_SET);

	HAL_Delay(1500);
	uint8_t msg[100]; // Buffer for storing messages to be transmitted via UART
	uint8_t ModbusCommand[8] = {0x01,0x03,0x00,0x03,0x00,0x01,0x74,0x0A};
	uint16_t CommandSize = sizeof(ModbusCommand) / sizeof(ModbusCommand[0]);
	float waterLevelChange;
	float waterLevels[readingCount];
	float differentialLevels[readingCount];

	for (int i = 0; i < 10; i++) {
	    APP_LOG(TS_OFF, VLEVEL_M, " Data Fetch: %d\r\n", i + 1);
	    sendRaw(ModbusCommand, CommandSize, &ModbusResp);
	    HAL_Delay(250);

	    APP_LOG(TS_OFF, VLEVEL_M, " MODBUS RESPONSE (Hex): ");
	    for (int x = 0; x < ModbusResp.rxIndex; x++) {
	        APP_LOG(TS_OFF, VLEVEL_M, "%02X ", ModbusResp.buffer[x]);
	        HAL_Delay(50);
	    }

	    rawLevelVal = parseReply(ModbusResp.buffer);
	    waterLevels[i] = rawLevelVal;
	    differentialLevels[i] = rawLevelVal - waterLevelLatest; // Calculate the difference
	    HAL_Delay(50);

	    sprintf((char*)msg, "\r\nWater Level: %f, Differential: %f \r\n", rawLevelVal, differentialLevels[i]);
	    HAL_UART_Transmit(&huart2, msg, strlen((char*)msg), 1000);

	    APP_LOG(TS_OFF, VLEVEL_M, " \r\n");
	    HAL_Delay(1000);
	}

	// Sort the array
	sortArray(differentialLevels, 10);

	if (samplingMethod == 0) {
	    waterLevelChange = getMedianValue(differentialLevels, 10);
	} else {
	    waterLevelChange = getAverageValue(differentialLevels, 10);
	}

	// Update the latest water level
	waterLevelLatest = waterLevel + waterLevelChange;

	sprintf((char*)msg, "\r\nWater Level Change: %f \r\n", waterLevelChange);
	HAL_UART_Transmit(&huart2, msg, strlen((char*)msg), 1000);

	if(waterLevelMin == 0 && waterLevelMax == 0){
		waterLevelMin = waterLevelMax = waterLevelLatest;
	}

	if(waterLevel < waterLevelMin){
		waterLevelMin = waterLevelLatest;
	} else if(waterLevel > waterLevelMax){
		waterLevelMax = waterLevelLatest;
	}

	sprintf((char*)msg, "Final Water Level: %f \r\n", waterLevel);
	HAL_UART_Transmit(&huart2, msg, strlen((char*)msg), 1000);

    waterLevelSamples[sampleIndex] = waterLevel;
    sampleIndex += 1;

    TRANSMIT_INTERVAL_MS = 180000;
    APP_LOG(TS_OFF, VLEVEL_M, "Transmission Cycle: %d seconds \r\n", TRANSMIT_INTERVAL_MS / 1000);
    MAX_WATER_LEVEL_SAMPLES = 1;
    waterLevelSamples = (float *)malloc(sizeof(float) * MAX_WATER_LEVEL_SAMPLES);
    transmissionType = 0;
    skipScheduledTransmission = false;
    isLevelBreached = false;

	APP_LOG(TS_OFF, VLEVEL_M, "\r\n");
//	HAL_GPIO_WritePin(GPIOA, GPIO_PIN_6, GPIO_PIN_RESET);
//	HAL_GPIO_WritePin(GPIOA, GPIO_PIN_7, GPIO_PIN_RESET);
}

/* Function to get water level distance value
 *
 */
void fetchSensorData(void) {

	bool sendUnscheduledTransmission = false;
	SecureElementNvmData_t FlashNVM;

	HAL_GPIO_WritePin(GPIOA, GPIO_PIN_6, GPIO_PIN_SET);
	HAL_GPIO_WritePin(GPIOA, GPIO_PIN_7, GPIO_PIN_SET);

	HAL_Delay(1500);
	uint8_t msg[100]; // Buffer for storing messages to be transmitted via UART
	uint8_t ModbusCommand[8] = {0x01,0x03,0x00,0x03,0x00,0x01,0x74,0x0A};
	uint16_t CommandSize = sizeof(ModbusCommand) / sizeof(ModbusCommand[0]);
	float waterLevels[readingCount];

	for(int i = 0; i < 10;  i++)	{
		APP_LOG(TS_OFF, VLEVEL_M, " Data Fetch: %d\r\n", i+1);
		sendRaw(ModbusCommand, CommandSize, &ModbusResp);
		HAL_Delay(250);

		APP_LOG(TS_OFF, VLEVEL_M, " MODBUS RESPONSE (Hex): ");
		for (int x = 0; x < ModbusResp.rxIndex; x++){
			APP_LOG(TS_OFF, VLEVEL_M, "%02X ", ModbusResp.buffer[x]);
			HAL_Delay(50);
		}

		rawLevelVal = parseReply(ModbusResp.buffer);
		waterLevels[i] = rawLevelVal;
		HAL_Delay(50);
		//APP_LOG(TS_OFF, VLEVEL_M, " Water Level: %d\r\n", rawLevelVal);
		sprintf((char*)msg, "\r\nWater Level: %f \r\n", rawLevelVal);
		HAL_UART_Transmit(&huart2, msg, strlen((char*)msg), 1000);

		APP_LOG(TS_OFF, VLEVEL_M, " \r\n");
		HAL_Delay(1000);
	}

	//CheckModbus();

	/*!
	 * Reply Format
	 * [Address Code] [Function Code] [Effective Bytes] [Distance Value]    [CRC]
	 *     0x01	           0x03     	    0x02            0x04 0xD2      0x3A 0xD9
	 * */

	// Sort the array
	sortArray(waterLevels, 10);

	if(samplingMethod == 0){
		waterLevel = getMedianValue(waterLevels, 10);
	} else {
		waterLevel = getAverageValue(waterLevels, 10);
	}

	waterLevelLatest = waterLevel;
	if(waterLevelMin == 0 && waterLevelMax == 0){
		waterLevelMin = waterLevelMax = waterLevel;
	}

	if(waterLevel < waterLevelMin){
		waterLevelMin = waterLevel;
	} else if(waterLevel > waterLevelMax){
		waterLevelMax = waterLevel;
	}

	sprintf((char*)msg, "Final Water Level: %f \r\n", waterLevel);
	HAL_UART_Transmit(&huart2, msg, strlen((char*)msg), 1000);

    waterLevelSamples[sampleIndex] = waterLevel;
    sampleIndex += 1;

//    if (waterLevel > thresholdLevelHigh || waterLevel < thresholdLevelLow) {
//        APP_LOG(TS_OFF, VLEVEL_M, " Water Level is greater than threshold! \r\n");
//        TRANSMIT_INTERVAL_MS = 180000;
//        MAX_WATER_LEVEL_SAMPLES = 1;
//        transmissionType = 1;
//        sendUnscheduledTransmission = true;
//        skipScheduledTransmission = true;
//        continuousCounter = 0;
//    } else {
//        APP_LOG(TS_OFF, VLEVEL_M, " Water Level is within threshold. \r\n");
//        if (continuousMode && continuousCounter < 2) {
//            TRANSMIT_INTERVAL_MS = 180000;
//            MAX_WATER_LEVEL_SAMPLES = 1;
//            sendUnscheduledTransmission = true;
//            skipScheduledTransmission = true;
//            continuousCounter++;
//            transmissionType = 1;
//        } else {
//        	if(continuousCounter >= 2){
//            	if (FLASH_IF_Read(&FlashNVM, LORAWAN_NVM_BASE_ADDRESS, sizeof(FlashNVM)) == FLASH_IF_OK) {
//            		HAL_Delay(50);
//            		MAX_WATER_LEVEL_SAMPLES = FlashNVM.pwxSamplingCount;
//                	waterLevelSamples = (float*)malloc(sizeof(int)*MAX_WATER_LEVEL_SAMPLES);
//                	SAMPLE_INTERVAL_MS = (TRANSMIT_INTERVAL_MS/MAX_WATER_LEVEL_SAMPLES);
//                    continuousCounter = 0;
//            	} else {
//            		APP_LOG(TS_OFF, VLEVEL_M, "FAILED READING FLASH \r\n");
//            	}
//        	}
//
//            TRANSMIT_INTERVAL_MS = 900000;
//            sendUnscheduledTransmission = false;
//            skipScheduledTransmission = false;
//            transmissionType = 0;
//        }
//
//    }
//
//    APP_LOG(TS_OFF, VLEVEL_M, "Transmission Cycle: %d seconds \r\n", TRANSMIT_INTERVAL_MS / 1000);
//    waterLevelSamples = (float*)malloc(sizeof(int) * MAX_WATER_LEVEL_SAMPLES);
//    hasJoined = false; // for redundancy

    if (continuousMode) {
        if (waterLevel >= thresholdLevelHigh || waterLevel <= thresholdLevelLow) { // Un-comment this if Low Threshold is needed
            APP_LOG(TS_OFF, VLEVEL_M, "Water Level is out of threshold range! \r\n");
            TRANSMIT_INTERVAL_MS = 180000;
            APP_LOG(TS_OFF, VLEVEL_M, "Transmission Cycle: %d seconds \r\n", TRANSMIT_INTERVAL_MS / 1000);
            MAX_WATER_LEVEL_SAMPLES = 1;
            waterLevelSamples = (float *)malloc(sizeof(float) * MAX_WATER_LEVEL_SAMPLES);
            transmissionType = 1;
            sendUnscheduledTransmission = true;
            skipScheduledTransmission = true;
            isLevelBreached = true;
            sampleIndex = 0;
            //hasJoined = false; // Set as false due to unscheduled transmission
        } else {
            APP_LOG(TS_OFF, VLEVEL_M, "Water Level is within threshold. \r\n");
            if (continuousCounter < 2) {
                TRANSMIT_INTERVAL_MS = 180000;
                APP_LOG(TS_OFF, VLEVEL_M, "Transmission Cycle: %d seconds \r\n", TRANSMIT_INTERVAL_MS / 1000);
                MAX_WATER_LEVEL_SAMPLES = 1;
                waterLevelSamples = (float *)malloc(sizeof(float) * MAX_WATER_LEVEL_SAMPLES);
                transmissionType = 1;
                sendUnscheduledTransmission = true;
                skipScheduledTransmission = true;
                isLevelBreached = false;
                sampleIndex = 0;
                //hasJoined = false; // Still setting as false for redundancy
                continuousCounter++;
            } else {
                if (FLASH_IF_Read(&FlashNVM, LORAWAN_NVM_BASE_ADDRESS, sizeof(FlashNVM)) == FLASH_IF_OK) {
                    HAL_Delay(50);
                    MAX_WATER_LEVEL_SAMPLES = FlashNVM.pwxSamplingCount;
                    waterLevelSamples = (float *)malloc(sizeof(float) * MAX_WATER_LEVEL_SAMPLES);
                    SAMPLE_INTERVAL_MS = (TRANSMIT_INTERVAL_MS / MAX_WATER_LEVEL_SAMPLES);
                    continuousCounter = 0;
                } else {
                    APP_LOG(TS_OFF, VLEVEL_M, "FAILED READING FLASH \r\n");
                }
                TRANSMIT_INTERVAL_MS = 900000;
                APP_LOG(TS_OFF, VLEVEL_M, "Transmission Cycle: %d seconds \r\n", TRANSMIT_INTERVAL_MS / 1000);
                MAX_WATER_LEVEL_SAMPLES = 5;
                waterLevelSamples = (float *)malloc(sizeof(float) * MAX_WATER_LEVEL_SAMPLES);
                transmissionType = 0;
                sendUnscheduledTransmission = false;
                skipScheduledTransmission = false;
                isLevelBreached = false;
                //hasJoined = true; // Back to scheduled transmission
            }
        }
    } else { // continuousMode == false
        if (waterLevel >= thresholdLevelHigh || waterLevel <= thresholdLevelLow) { // Un-comment this if Low Threshold is needed
            APP_LOG(TS_OFF, VLEVEL_M, "Water Level is out of threshold range! \r\n");
            TRANSMIT_INTERVAL_MS = 180000;
            APP_LOG(TS_OFF, VLEVEL_M, "Transmission Cycle: %d seconds \r\n", TRANSMIT_INTERVAL_MS / 1000);
            MAX_WATER_LEVEL_SAMPLES = 1;
            waterLevelSamples = (float *)malloc(sizeof(float) * MAX_WATER_LEVEL_SAMPLES);
            transmissionType = 1;
            sendUnscheduledTransmission = true;
            skipScheduledTransmission = true;
            isLevelBreached = true;
            sampleIndex = 0;
            //hasJoined = false; // Set as false due to unscheduled transmission
        } else {
            APP_LOG(TS_OFF, VLEVEL_M, "Water Level is within threshold. \r\n");
            TRANSMIT_INTERVAL_MS = 900000;
            APP_LOG(TS_OFF, VLEVEL_M, "Transmission Cycle: %d seconds \r\n", TRANSMIT_INTERVAL_MS / 1000);
            MAX_WATER_LEVEL_SAMPLES = 5;
            waterLevelSamples = (float *)malloc(sizeof(float) * MAX_WATER_LEVEL_SAMPLES);
            transmissionType = 0;
            sendUnscheduledTransmission = false;
            skipScheduledTransmission = false;
            isLevelBreached = false;
            //hasJoined = true; // Back to scheduled transmission
        }
    }



//	if(waterLevel > thresholdLevel){
//		APP_LOG(TS_OFF, VLEVEL_M, " Water Level is greater than threshold! \r\n");
//		TRANSMIT_INTERVAL_MS = 180000;
//		APP_LOG(TS_OFF, VLEVEL_M, "Transmission Cycle: %d seconds \r\n",TRANSMIT_INTERVAL_MS/1000);
//		MAX_WATER_LEVEL_SAMPLES = 1;
//		waterLevelSamples = (float*)malloc(sizeof(int)*MAX_WATER_LEVEL_SAMPLES);
//		hasJoined = false; // for redundancy
//		transmissionType = 1;
//		sendUnscheduledTransmission = true;
//		skipScheduledTransmission = true;
//	} else{
//		APP_LOG(TS_OFF, VLEVEL_M, " Water Level is within threshold. \r\n");
//		TRANSMIT_INTERVAL_MS = 900000;
//		APP_LOG(TS_OFF, VLEVEL_M, "Transmission Cycle: %d seconds \r\n",TRANSMIT_INTERVAL_MS/1000);
//		MAX_WATER_LEVEL_SAMPLES = 5;
//		waterLevelSamples = (float*)malloc(sizeof(int)*MAX_WATER_LEVEL_SAMPLES);
//		transmissionType = 0;
//	    skipScheduledTransmission = false;
//	}

	if(sendUnscheduledTransmission == true){
		while(sendUnscheduledTransmission!= false){
			LmHandlerErrorStatus_t status = LORAMAC_HANDLER_ERROR;
			UTIL_TIMER_Time_t nextTxIn = 0;
			APP_LOG(TS_OFF, VLEVEL_M, "[!]  LM HANDLER IS BUSY! \r\n");
			HAL_Delay(100);
			  if (LmHandlerIsBusy() == false)
			  {
				APP_LOG(TS_OFF, VLEVEL_M, "============================================= \r\n");
				APP_LOG(TS_OFF, VLEVEL_M, "            UNSCHEDULED TRANSMISSION            \r\n");
				APP_LOG(TS_OFF, VLEVEL_M, "============================================= \r\n");
				//HAL_StatusTypeDef status;

				AppData.Port = LORAWAN_USER_APP_PORT;
				_doneScanning = false;

				uint8_t msg[100]; // Buffer for storing messages to be transmitted via UART
				for(int x = 0; x < AppData.BufferSize; x++){
						AppData.Buffer[x] = 0;
					}

				/* Prepare Data */
			    waterLevelLatest 	= waterLevelLatest 	* 100;
			    waterLevel 		 	= waterLevel 		* 100;
			    waterLevelMin	 	= waterLevelMin 	* 100;
			    waterLevelMax	 	= waterLevelMax 	* 100;

				uint8_t i = 0;
				AppData.Buffer[i++] = (uint8_t)transmissionType;
				AppData.Buffer[i++] = (uint8_t)((uint16_t)waterLevelLatest >> 8);
				AppData.Buffer[i++] = (uint8_t)((uint16_t)waterLevelLatest & 0xFF);
				AppData.Buffer[i++] = (uint8_t)((uint16_t)waterLevel >> 8);
				AppData.Buffer[i++] = (uint8_t)((uint16_t)waterLevel & 0xFF);
				AppData.Buffer[i++] = (uint8_t)((uint16_t)waterLevelMin >> 8);
				AppData.Buffer[i++] = (uint8_t)((uint16_t)waterLevelMin & 0xFF);
				AppData.Buffer[i++] = (uint8_t)((uint16_t)waterLevelMax >> 8);
				AppData.Buffer[i++] = (uint8_t)((uint16_t)waterLevelMax & 0xFF);
				AppData.BufferSize = i;

				sprintf((char*)msg, "Payload Buffer Size: %u\r\n\r\n", AppData.BufferSize);
				HAL_UART_Transmit(&huart2, msg, strlen((char*)msg), 1000);

				for(int i = 0; i < MAX_WATER_LEVEL_SAMPLES; i++){
					waterLevelSamples[i] = 0;
				}

				if ((JoinLedTimer.IsRunning) && (LmHandlerJoinStatus() == LORAMAC_HANDLER_SET))
				{
				  UTIL_TIMER_Stop(&JoinLedTimer);
				  HAL_GPIO_WritePin(LED3_GPIO_Port, LED3_Pin, GPIO_PIN_RESET); /* LED_RED */
				}

				LmHandlerSend(&AppData, LmHandlerParams.IsTxConfirmed, false);
				sendUnscheduledTransmission = false;
			  }
			  HAL_Delay(100);
		}

	}

    //sampleIndex = (sampleIndex + 1) % MAX_WATER_LEVEL_SAMPLES;

	APP_LOG(TS_OFF, VLEVEL_M, "\r\n");
//	HAL_GPIO_WritePin(GPIOA, GPIO_PIN_6, GPIO_PIN_RESET);
//	HAL_GPIO_WritePin(GPIOA, GPIO_PIN_7, GPIO_PIN_RESET);
}

void fetchLTCData(void){
	uint8_t b_i2c_data = 0;
	uint16_t i2c_data = 0;

	/* Read current value of CHARGER_CONFIG_BITS */

	/* Write back the updated value to CHARGER_CONFIG_BITS */
	uint8_t dataBuffer1[2] = {0x1E, 0x00};
	uint8_t readBuffer[2]; // Allocate memory for readBuffer
	HAL_StatusTypeDef status;
	uint8_t dataBuffer17[2] = {0x04, 0x00};
	uint8_t msg[100]; // Buffer for storing messages to be transmitted via UART

	sprintf((char*)msg, "LTC READING\r\n");
	HAL_UART_Transmit(&huart2, msg, strlen((char*)msg), 1000);

	sprintf((char*)msg, "=====================================================\r\n");
	HAL_UART_Transmit(&huart2, msg, strlen((char*)msg), 1000);

	if (isInit == true) {
		isInit = false;
		status = HAL_I2C_Mem_Write(&hi2c1, 0x68 << 1, 0x29, I2C_MEMADD_SIZE_8BIT, dataBuffer17, 2, 1000);
		if (status != HAL_OK) {
			sprintf((char*)msg, "Error writing new value: %d\r\n", status);
		} else {
			sprintf((char*)msg, "Disabled en_jeita successful\r\n");
		}
		HAL_UART_Transmit(&huart2, msg, strlen((char*)msg), 1000);
		status = HAL_I2C_Mem_Read(&hi2c1, 0x68 << 1, 0x29, I2C_MEMADD_SIZE_8BIT, (uint8_t*)&i2c_data, 1, HAL_MAX_DELAY);
		if (status != HAL_OK) {
			sprintf((char*)msg, "Error reading new value: %d\r\n", status);
		} else {
			sprintf((char*)msg, "JEITA Value: %d\r\n", i2c_data);
		}
		HAL_UART_Transmit(&huart2, msg, strlen((char*)msg), 1000);
	 //} REMOVED FOR TESTING PURPOSES

	 // Read the initial value from the register
	 status = HAL_I2C_Mem_Read(&hi2c1, 0x68 << 1, 0x1A, I2C_MEMADD_SIZE_8BIT, (uint8_t*)&i2c_data, 1, HAL_MAX_DELAY);
	 if (status != HAL_OK) {
		 sprintf((char*)msg, "Error reading initial value: %d\r\n", status);
	 } else {
		 sprintf((char*)msg, "CHARGER CONFIG Value: %d\r\n", i2c_data);
	 }
	 HAL_UART_Transmit(&huart2, msg, strlen((char*)msg), 1000);

	 HAL_Delay(100);

	 // Write the new value to the register
	 status = HAL_I2C_Mem_Write(&hi2c1, 0x68 << 1, 0x1A, I2C_MEMADD_SIZE_8BIT, dataBuffer1, 2, HAL_MAX_DELAY);
	 if (status != HAL_OK) {
		 sprintf((char*)msg, "Error writing new value: %d\r\n", status);
	 } else {
		 sprintf((char*)msg, "Write operation successful\r\n");
	 }
	 HAL_UART_Transmit(&huart2, msg, strlen((char*)msg), 1000);

	 HAL_Delay(100);

	 // Read the value again to verify the write operation
	 status = HAL_I2C_Mem_Read(&hi2c1, 0x68 << 1, 0x1A, I2C_MEMADD_SIZE_8BIT, readBuffer, 1, HAL_MAX_DELAY);
	 status = HAL_I2C_Mem_Read(&hi2c1, 0x68 << 1, 0x1A, I2C_MEMADD_SIZE_8BIT, (uint8_t*)&i2c_data, 1, HAL_MAX_DELAY);
	 if (status != HAL_OK) {
		 sprintf((char*)msg, "Error reading new value: %d\r\n", status);
	 } else {
		 sprintf((char*)msg, "CHARGER CONFIG New Value: %d\r\n", i2c_data);
	 }
	 HAL_UART_Transmit(&huart2, msg, strlen((char*)msg), 1000);

	 status = HAL_I2C_Mem_Read(&hi2c1, 0x68 << 1, 0x44, I2C_MEMADD_SIZE_8BIT, (uint8_t*)&i2c_data, 1, HAL_MAX_DELAY);    //ICHARGE_DAC
	 if (status != HAL_OK) {
		 sprintf((char*)msg, "Error reading new value: %d\r\n", status);
	 } else {
		 sprintf((char*)msg, "CHARGER DAC New Value: %d\r\n", i2c_data);
		 ICHARGE_DAC = (float)(i2c_data * 100);
	 }
	 HAL_UART_Transmit(&huart2, msg, strlen((char*)msg), 1000);

	 status = HAL_I2C_Mem_Read(&hi2c1, 0x68 << 1, 0x39, I2C_MEMADD_SIZE_8BIT, (uint8_t*)&i2c_data, 1, HAL_MAX_DELAY);
	 if (status != HAL_OK) {
		 sprintf((char*)msg, "Error reading new system status: %d\r\n", status);
	 } else {
		 sprintf((char*)msg, "SYSTEM STATUS: %d\r\n", i2c_data);
		 SYSTEM_STATUS = (uint16_t)(i2c_data * 100);
	 }
	 HAL_UART_Transmit(&huart2, msg, strlen((char*)msg), 1000);

	 status = HAL_I2C_Mem_Read(&hi2c1, 0x68 << 1, 0x34, I2C_MEMADD_SIZE_8BIT, (uint8_t*)&i2c_data, 1, HAL_MAX_DELAY);
	 if (status != HAL_OK) {
		 sprintf((char*)msg, "Error reading charger state: %d\r\n", status);
	 } else {
		 sprintf((char*)msg, "CHARGER STATE: %d\r\n", i2c_data);
	 }
	 HAL_UART_Transmit(&huart2, msg, strlen((char*)msg), 1000);

	 status = HAL_I2C_Mem_Read(&hi2c1, 0x68 << 1, 0x35, I2C_MEMADD_SIZE_8BIT, (uint8_t*)&i2c_data, 1, HAL_MAX_DELAY);
	 if (status != HAL_OK) {
		 sprintf((char*)msg, "Error reading charger status: %d\r\n", status);
	 } else {
		 sprintf((char*)msg, "CHARGER STATUS: %d\r\n", i2c_data);
	 }
	 HAL_UART_Transmit(&huart2, msg, strlen((char*)msg), 1000);

	 status = HAL_I2C_Mem_Read(&hi2c1, 0x68 << 1, 0x36, I2C_MEMADD_SIZE_8BIT, (uint8_t*)&i2c_data, 1, HAL_MAX_DELAY);
	 if (status != HAL_OK) {
		 sprintf((char*)msg, "Error reading limit alert: %d\r\n", status);
	 } else {
		 sprintf((char*)msg, "LIMIT ALERT: %d\r\n", i2c_data);
	 }
	 HAL_UART_Transmit(&huart2, msg, strlen((char*)msg), 1000);

	 status = HAL_I2C_Mem_Read(&hi2c1, 0x68 << 1, 0x37, I2C_MEMADD_SIZE_8BIT, (uint8_t*)&i2c_data, 1, HAL_MAX_DELAY);
	 if (status != HAL_OK) {
		 sprintf((char*)msg, "Error reading charger state alert: %d\r\n", status);
	 } else {
		 sprintf((char*)msg, "CHARGER STATE ALERT: %d\r\n", i2c_data);
	 }
	 HAL_UART_Transmit(&huart2, msg, strlen((char*)msg), 1000);

	 status = HAL_I2C_Mem_Read(&hi2c1, 0x68 << 1, 0x38, I2C_MEMADD_SIZE_8BIT, (uint8_t*)&i2c_data, 1, HAL_MAX_DELAY);
	 if (status != HAL_OK) {
		 sprintf((char*)msg, "Error reading charger status alert: %d\r\n", status);
	 } else {
		 sprintf((char*)msg, "CHARGER STATUS ALERT: %d\r\n", i2c_data);
	 }
	 HAL_UART_Transmit(&huart2, msg, strlen((char*)msg), 1000);
	}

	 sprintf((char*)msg, "=====================================================\r\n");
	 HAL_UART_Transmit(&huart2, msg, strlen((char*)msg), 1000);

	 /* Writing ICHARGE_TARGET */
	 uint8_t register_address = 0x1A;

	 // Write to device 0x68, set register address to 0x1A, and write 0x05
	 if (HAL_I2C_Mem_Write(&hi2c1, 0x68 << 1, register_address, I2C_MEMADD_SIZE_8BIT, 0x05, 1, HAL_MAX_DELAY) == HAL_OK) {
		 sprintf((char*)msg, "ICHARGE WRITTEN: %d\r\n", 0x05);
		 HAL_UART_Transmit(&huart2, msg, strlen((char*)msg), 1000);

		 // Read from device 0x68, read data from the specified register (0x1A)
		 if (HAL_I2C_Mem_Read(&hi2c1, 0x68 << 1, register_address, I2C_MEMADD_SIZE_8BIT, &b_i2c_data, 1, HAL_MAX_DELAY) == HAL_OK) {
			 sprintf((char*)msg, "ICHARGE READ: %d\r\n", b_i2c_data);
			 HAL_UART_Transmit(&huart2, msg, strlen((char*)msg), 1000);
		 } else {
			 sprintf((char*)msg, "Error in read operation\r\n");
			 HAL_UART_Transmit(&huart2, msg, strlen((char*)msg), 1000);
		 }
	 } else {
		 sprintf((char*)msg, "Error in write operation\r\n");
		 HAL_UART_Transmit(&huart2, msg, strlen((char*)msg), 1000);
	 }

	 sprintf((char*)msg, "=====================================================\r\n");
	 HAL_UART_Transmit(&huart2, msg, strlen((char*)msg), 1000);

	 /* Read actual charge current setting applied */
	 HAL_I2C_Mem_Read(&hi2c1, 0x68 << 1, 0x44, I2C_MEMADD_SIZE_8BIT, (uint8_t*)&b_i2c_data, 1, HAL_MAX_DELAY);
	 sprintf((char*)msg, "ICHARGE_DAC RAW Value: %d\r\n", b_i2c_data);
	 HAL_UART_Transmit(&huart2, msg, strlen((char*)msg), 1000);
	 //float calculated_value = ((b_i2c_data & 0x1F) + 1) / 3.0f;
	 float calculated_value = (b_i2c_data + 1) * (0.001/0.004);
	 sprintf((char*)msg, "ICHARGE_DAC Converted Value: %f\r\n", calculated_value);
	 HAL_UART_Transmit(&huart2, msg, strlen((char*)msg), 1000);
	 ICHARGE_DAC = (float)(calculated_value * 100);																		//ICHARGE DAC

	 /* Read Battery Voltage */
	 HAL_I2C_Mem_Read(&hi2c1, 0x68 << 1, 0x3A, I2C_MEMADD_SIZE_8BIT, (uint8_t*)&i2c_data, 2, HAL_MAX_DELAY);
	 sprintf((char*)msg, "BATTERY RAW Value: %d\r\n", i2c_data);
	 HAL_UART_Transmit(&huart2, msg, strlen((char*)msg), 1000);

	 /* Calculate and print Battery Voltage */
	 HAL_I2C_Mem_Read(&hi2c1, 0x68 << 1, 0x3A, I2C_MEMADD_SIZE_8BIT, (uint8_t*)&i2c_data, 2, HAL_MAX_DELAY);
	 float battery_voltage = i2c_data * 0.000192264 * 4;
	 sprintf((char*)msg, "Battery Voltage: %f V\r\n", battery_voltage);
	 HAL_UART_Transmit(&huart2, msg, strlen((char*)msg), 1000);
	 VBAT = battery_voltage * 100;

	 /* Read battery current */
	 i2c_data = 0;
	 HAL_I2C_Mem_Read(&hi2c1, 0x68 << 1, 0x3D, I2C_MEMADD_SIZE_8BIT, (uint8_t*)&i2c_data, 2, HAL_MAX_DELAY);
	 sprintf((char*)msg, "BATTERY Current Raw: %d\r\n", i2c_data);
	 HAL_UART_Transmit(&huart2, msg, strlen((char*)msg), 1000);
	 float calculated_battCur = i2c_data *( 0.00000146487 / 0.004);
	 sprintf((char*)msg, "BATTERY Current Converted: %f\r\n\r\n", calculated_battCur);
	 HAL_UART_Transmit(&huart2, msg, strlen((char*)msg), 1000);
	 IBAT = calculated_battCur * 100;																			//IBAT

	 HAL_I2C_Mem_Read(&hi2c1, 0x68 << 1, 0x3B, I2C_MEMADD_SIZE_8BIT, (uint8_t*)&i2c_data, 2, HAL_MAX_DELAY);
	 sprintf((char*)msg, "VOLTAGE IN RAW: %d\r\n", i2c_data);
	 HAL_UART_Transmit(&huart2, msg, strlen((char*)msg), 1000);
	 float calculated_vin = i2c_data * 0.001648;
	 sprintf((char*)msg, "VOLTAGE IN CALCULATED: %f\r\n", calculated_vin);
	 HAL_UART_Transmit(&huart2, msg, strlen((char*)msg), 1000);
	 VIN = calculated_vin * 100;

	 HAL_I2C_Mem_Read(&hi2c1, 0x68 << 1, 0x3C, I2C_MEMADD_SIZE_8BIT, (uint8_t*)&i2c_data, 2, HAL_MAX_DELAY);
	 sprintf((char*)msg, "SYSTEM VOLTAGE RAW: %d\r\n", i2c_data);
	 HAL_UART_Transmit(&huart2, msg, strlen((char*)msg), 1000);
	 float calculated_vsys = i2c_data * 0.001648;
	 sprintf((char*)msg, "SYSTEM VOLTAGE CALCULATED: %f\r\n", calculated_vsys);
	 HAL_UART_Transmit(&huart2, msg, strlen((char*)msg), 1000);
	 VSYS = calculated_vsys * 100;

	 HAL_I2C_Mem_Read(&hi2c1, 0x68 << 1, 0x3E, I2C_MEMADD_SIZE_8BIT, (uint8_t*)&i2c_data, 2, HAL_MAX_DELAY);
	 sprintf((char*)msg, "INPUT CURRENT RAW: %d\r\n", i2c_data);
	 HAL_UART_Transmit(&huart2, msg, strlen((char*)msg), 1000);
	 float calculated_iin = (i2c_data * ( 0.00000146487 / 0.003));
	 sprintf((char*)msg, "INPUT CURRENT CALCULATED: %f\r\n", calculated_iin);
	 HAL_UART_Transmit(&huart2, msg, strlen((char*)msg), 1000);
	 IIN = calculated_iin * 100;

	 sprintf((char*)msg, "=====================================================\r\n");
	 HAL_UART_Transmit(&huart2, msg, strlen((char*)msg), 1000);

	 /* Read Alerts */
	 HAL_I2C_Mem_Read(&hi2c1, 0x68 << 1, 0x36, I2C_MEMADD_SIZE_8BIT, (uint8_t*)&i2c_data, 2, HAL_MAX_DELAY);
	 sprintf((char*)msg, "LIMIT ALERTS: %d\r\n", i2c_data);
	 HAL_UART_Transmit(&huart2, msg, strlen((char*)msg), 1000);

	 HAL_I2C_Mem_Read(&hi2c1, 0x68 << 1, 0x37, I2C_MEMADD_SIZE_8BIT, (uint8_t*)&i2c_data, 2, HAL_MAX_DELAY);
	 sprintf((char*)msg, "CHARGER STATE ALERTS: %d\r\n", i2c_data);
	 HAL_UART_Transmit(&huart2, msg, strlen((char*)msg), 1000);

	 HAL_I2C_Mem_Read(&hi2c1, 0x68 << 1, 0x39, I2C_MEMADD_SIZE_8BIT, (uint8_t*)&i2c_data, 2, HAL_MAX_DELAY);
	 sprintf((char*)msg, "SYSTEM STATUS ALERTS: %d\r\n", i2c_data);
	 HAL_UART_Transmit(&huart2, msg, strlen((char*)msg), 1000);
	 SYSTEM_STATUS = (uint16_t) i2c_data;

	 sprintf((char*)msg, "=====================================================\r\n");
	 HAL_UART_Transmit(&huart2, msg, strlen((char*)msg), 1000);

	 HAL_Delay(2000);
}

/* USER CODE END EF */

void LoRaWAN_Init(void)
{
  /* USER CODE BEGIN LoRaWAN_Init_LV */
  uint32_t feature_version = 0UL;
  /* USER CODE END LoRaWAN_Init_LV */

  /* USER CODE BEGIN LoRaWAN_Init_1 */
  APP_LOG(TS_OFF, VLEVEL_M, "\r\n");
  APP_LOG(TS_OFF, VLEVEL_M, "  ____            _        _                             ___               \r\n");
  APP_LOG(TS_OFF, VLEVEL_M, " |  _ \\ __ _  ___| | _____| |___      _____  _ ____  __ |_ _|_ __   ___    \r\n");
  APP_LOG(TS_OFF, VLEVEL_M, " | |_) / _` |/ __| |/ / _ \\ __\\ \\ /\\ / / _ \\| '__\\ \\/ /  | || '_ \\ / __|   \r\n");
  APP_LOG(TS_OFF, VLEVEL_M, " |  __/ (_| | (__|   <  __/ |_ \\ V  V / (_) | |   >  <   | || | | | (__ _  \r\n");
  APP_LOG(TS_OFF, VLEVEL_M, " |_|   \\__,_|\\___|_|\\_\\___|\\__| \\_/\\_/ \\___/|_|  /_/\\_\\ |___|_| |_|\\___(_) \r\n");
  APP_LOG(TS_OFF, VLEVEL_M, "\r\n");
  APP_LOG(TS_OFF, VLEVEL_M, " Firmware: Water Level 20M 	Ver.: 1.0.1		Rev. Date: 8-2-24\r\n");
  APP_LOG(TS_OFF, VLEVEL_M, "\r\n");

  waterLevelSamples = (float*)malloc(sizeof(int)*MAX_WATER_LEVEL_SAMPLES);
  /* Get LoRaWAN APP version*/
  APP_LOG(TS_OFF, VLEVEL_M, "APPLICATION_VERSION: V%X.%X.%X\r\n",
          (uint8_t)(APP_VERSION_MAIN),
          (uint8_t)(APP_VERSION_SUB1),
          (uint8_t)(APP_VERSION_SUB2));

  /* Get MW LoRaWAN info */
  APP_LOG(TS_OFF, VLEVEL_M, "MW_LORAWAN_VERSION:  V%X.%X.%X\r\n",
          (uint8_t)(LORAWAN_VERSION_MAIN),
          (uint8_t)(LORAWAN_VERSION_SUB1),
          (uint8_t)(LORAWAN_VERSION_SUB2));

  /* Get MW SubGhz_Phy info */
  APP_LOG(TS_OFF, VLEVEL_M, "MW_RADIO_VERSION:    V%X.%X.%X\r\n",
          (uint8_t)(SUBGHZ_PHY_VERSION_MAIN),
          (uint8_t)(SUBGHZ_PHY_VERSION_SUB1),
          (uint8_t)(SUBGHZ_PHY_VERSION_SUB2));

  /* Get LoRaWAN Link Layer info */
  LmHandlerGetVersion(LORAMAC_HANDLER_L2_VERSION, &feature_version);
  APP_LOG(TS_OFF, VLEVEL_M, "L2_SPEC_VERSION:     V%X.%X.%X\r\n",
          (uint8_t)(feature_version >> 24),
          (uint8_t)(feature_version >> 16),
          (uint8_t)(feature_version >> 8));

  /* Get LoRaWAN Regional Parameters info */
  LmHandlerGetVersion(LORAMAC_HANDLER_REGION_VERSION, &feature_version);
  APP_LOG(TS_OFF, VLEVEL_M, "RP_SPEC_VERSION:     V%X-%X.%X.%X\r\n",
          (uint8_t)(feature_version >> 24),
          (uint8_t)(feature_version >> 16),
          (uint8_t)(feature_version >> 8),
          (uint8_t)(feature_version));

  UTIL_TIMER_Create(&TxLedTimer, LED_PERIOD_TIME, UTIL_TIMER_ONESHOT, OnTxTimerLedEvent, NULL);
  UTIL_TIMER_Create(&RxLedTimer, LED_PERIOD_TIME, UTIL_TIMER_ONESHOT, OnRxTimerLedEvent, NULL);
  UTIL_TIMER_Create(&JoinLedTimer, LED_PERIOD_TIME, UTIL_TIMER_PERIODIC, OnJoinTimerLedEvent, NULL);

  if (FLASH_IF_Init(NULL) != FLASH_IF_OK)
  {
    Error_Handler();
  }

  /* USER CODE END LoRaWAN_Init_1 */

  UTIL_TIMER_Create(&StopJoinTimer, JOIN_TIME, UTIL_TIMER_ONESHOT, OnStopJoinTimerEvent, NULL);

  UTIL_SEQ_RegTask((1 << CFG_SEQ_Task_LmHandlerProcess), UTIL_SEQ_RFU, LmHandlerProcess);

  UTIL_SEQ_RegTask((1 << CFG_SEQ_Task_LoRaSendOnTxTimerOrButtonEvent), UTIL_SEQ_RFU, SendTxData);
  UTIL_SEQ_RegTask((1 << CFG_SEQ_Task_LoRaStoreContextEvent), UTIL_SEQ_RFU, StoreContext);
  UTIL_SEQ_RegTask((1 << CFG_SEQ_Task_LoRaStopJoinEvent), UTIL_SEQ_RFU, StopJoin);

  /* Init Info table used by LmHandler*/
  LoraInfo_Init();

  /* Init the Lora Stack*/
  LmHandlerInit(&LmHandlerCallbacks, APP_VERSION);

  LmHandlerConfigure(&LmHandlerParams);

  /* USER CODE BEGIN LoRaWAN_Init_2 */
  UTIL_TIMER_Start(&JoinLedTimer);

  /* USER CODE END LoRaWAN_Init_2 */

  LmHandlerJoin(ActivationType, ForceRejoin);

  if (EventType == TX_ON_TIMER)
  {
    /* send every time timer elapses */
    UTIL_TIMER_Create(&TxTimer, TxPeriodicity, UTIL_TIMER_ONESHOT, OnTxTimerEvent, NULL);
    UTIL_TIMER_Start(&TxTimer);
  }
  else
  {
    /* USER CODE BEGIN LoRaWAN_Init_3 */

    /* USER CODE END LoRaWAN_Init_3 */
  }

  /* USER CODE BEGIN LoRaWAN_Init_Last */

  /* USER CODE END LoRaWAN_Init_Last */
}

/* USER CODE BEGIN PB_Callbacks */

void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
  switch (GPIO_Pin)
  {
    case  BUT1_Pin:
      /* Note: when "EventType == TX_ON_TIMER" this GPIO is not initialized */
      if (EventType == TX_ON_EVENT)
      {
        UTIL_SEQ_SetTask((1 << CFG_SEQ_Task_LoRaSendOnTxTimerOrButtonEvent), CFG_SEQ_Prio_0);
      }
      break;
    case  BUT2_Pin:
		  APP_LOG(TS_OFF, VLEVEL_M, "+============================================+\r\n");
		  APP_LOG(TS_OFF, VLEVEL_M, "|	    EXTI EVENT! EXTI EVENT! EXTI EVENT!   |\r\n");
		  APP_LOG(TS_OFF, VLEVEL_M, "+============================================+\r\n");
		  HAL_GPIO_WritePin(LED1_GPIO_Port, LED1_Pin, GPIO_PIN_SET); /* LED_BLUE */
		  HAL_GPIO_WritePin(LED1_GPIO_Port, LED2_Pin, GPIO_PIN_RESET); /* LED_BLUE */
      //UTIL_SEQ_SetTask((1 << CFG_SEQ_Task_LoRaStopJoinEvent), CFG_SEQ_Prio_0);
      break;
    case  BUT3_Pin:
      //UTIL_SEQ_SetTask((1 << CFG_SEQ_Task_LoRaStoreContextEvent), CFG_SEQ_Prio_0);
    	HAL_GPIO_WritePin(LED1_GPIO_Port, LED1_Pin, GPIO_PIN_RESET); /* LED_BLUE */
    	HAL_GPIO_WritePin(LED1_GPIO_Port, LED2_Pin, GPIO_PIN_SET);
      break;
    default:
      break;
  }
}

/* USER CODE END PB_Callbacks */

/* Private functions ---------------------------------------------------------*/
/* USER CODE BEGIN PrFD */

/* USER CODE END PrFD */

static void OnRxData(LmHandlerAppData_t *appData, LmHandlerRxParams_t *params)
{
  /* USER CODE BEGIN OnRxData_1 */
  uint8_t RxPort = 0;
  SecureElementNvmData_t FlashNVM;

  if (params != NULL)
  {
    HAL_GPIO_WritePin(LED1_GPIO_Port, LED1_Pin, GPIO_PIN_SET); /* LED_BLUE */

    UTIL_TIMER_Start(&RxLedTimer);

    if (params->IsMcpsIndication)
    {
      if (appData != NULL)
      {
        RxPort = appData->Port;
        if (appData->Buffer != NULL)
        {
          switch (appData->Port)
          {
            case LORAWAN_SWITCH_CLASS_PORT:
              /*this port switches the class*/
              if (appData->BufferSize == 1)
              {
                switch (appData->Buffer[0])
                {
                  case 0:
                  {
                    LmHandlerRequestClass(CLASS_A);
                    break;
                  }
                  case 1:
                  {
                    LmHandlerRequestClass(CLASS_B);
                    break;
                  }
                  case 2:
                  {
                    LmHandlerRequestClass(CLASS_C);
                    break;
                  }
                  default:
                    break;
                }
              }
              break;
//            case 25:
//                if (appData->Buffer != NULL && appData->BufferSize >= 1) {
//
//                	if(appData->Buffer[0] == 7){
//						int parsedMode = 0;
//						APP_LOG(TS_OFF, VLEVEL_M, "\r\n==============================================\r\n");
//						APP_LOG(TS_OFF, VLEVEL_M, "Received data from port 25 (hex): ");
//						for (int i = 0; i < appData->BufferSize; i++) {
//							APP_LOG(TS_OFF, VLEVEL_M, "%02X ", appData->Buffer[i]);
//						}
//
//                        for (int i = 1; i < appData->BufferSize; i++) {
//                        	parsedMode = (parsedMode << 8) | appData->Buffer[i];
//                        }
//						sensingMode = parsedMode;
//						APP_LOG(TS_OFF, VLEVEL_M, "Sensing Mode: %d", parsedMode);
//						APP_LOG(TS_OFF, VLEVEL_M, "\r\n==============================================\r\n");
//
//					}
//
//                	if(appData->Buffer[0] == 8){
//                        uint32_t parsedSeconds = 0;
//
//						APP_LOG(TS_OFF, VLEVEL_M, "\r\n==============================================\r\n");
//						APP_LOG(TS_OFF, VLEVEL_M, "Received data from port 25 (hex): ");
//						for (int i = 0; i < appData->BufferSize; i++) {
//							APP_LOG(TS_OFF, VLEVEL_M, "%02X ", appData->Buffer[i]);
//						}
//
//                        for (int i = 1; i < appData->BufferSize; i++) {
//                            parsedSeconds = (parsedSeconds << 8) | appData->Buffer[i];
//                        }
//                        APP_LOG(TS_OFF, VLEVEL_M, "Heartbeat Interval: %d seconds",parsedSeconds);
//                        TRANSMIT_INTERVAL_MS = parsedSeconds*1000;
//                        // Call OnTxPeriodicityChanged with the parsed seconds
//                        // OnTxPeriodicityChanged(parsedSeconds*1000);
//						APP_LOG(TS_OFF, VLEVEL_M, "\r\n==============================================\r\n");
//                	}
//
////                	if(appData->Buffer[0] == 7){
////                		float parsedThreshold = 0.0;
////						APP_LOG(TS_OFF, VLEVEL_M, "\r\n==============================================\r\n");
////						APP_LOG(TS_OFF, VLEVEL_M, "Received data from port 25 (hex): ");
////						for (int i = 0; i < appData->BufferSize; i++) {
////							APP_LOG(TS_OFF, VLEVEL_M, "%02X ", appData->Buffer[i]);
////						}
////                        for (int i = 1; i < appData->BufferSize; i++) {
////                        	parsedThreshold = (parsedThreshold << 8) | appData->Buffer[i];
////                        }
////                        thresholdLevel = parsedThreshold;
////                        APP_LOG(TS_OFF, VLEVEL_M, "Threshold Level: %d", parsedThreshold);
////						APP_LOG(TS_OFF, VLEVEL_M, "\r\n==============================================\r\n");
////                	}
//
//					if(appData->Buffer[0] == 9){
//						int parsedCount = 0;
//						APP_LOG(TS_OFF, VLEVEL_M, "\r\n==============================================\r\n");
//						APP_LOG(TS_OFF, VLEVEL_M, "Received data from port 25 (hex): ");
//						for (int i = 0; i < appData->BufferSize; i++) {
//							APP_LOG(TS_OFF, VLEVEL_M, "%02X ", appData->Buffer[i]);
//						}
//                        for (int i = 1; i < appData->BufferSize; i++) {
//                        	parsedCount = (parsedCount << 8) | appData->Buffer[i];
//                        }
//						MAX_WATER_LEVEL_SAMPLES = parsedCount;
//				    	waterLevelSamples = (float*)malloc(sizeof(int)*MAX_WATER_LEVEL_SAMPLES);
//						APP_LOG(TS_OFF, VLEVEL_M, "Sampling Count: %d", parsedCount);
//						APP_LOG(TS_OFF, VLEVEL_M, "\r\n==============================================\r\n");
//
//					}
//
//                }
//            break;

            case LORA_CONFIG_PARAMS_PORT:
            	if(appData->Buffer[0] == CONFIG_DEVEUI_ID){
            		uint8_t *_devEui = (uint8_t *) malloc (sizeof(uint8_t) * SE_EUI_SIZE);

            		if(_devEui != NULL){
            			memset(_devEui, 0, sizeof(uint8_t) * SE_EUI_SIZE);
            			for(uint8_t i = 0; i < SE_EUI_SIZE; i++){
            				_devEui[i] = appData->Buffer[i + 1];
            			}
            			/* Retrieve Data from NVM */
            			APP_LOG(TS_OFF, VLEVEL_M, "###### Lora-Configuration Mode: ON \r\n");
						if (FLASH_IF_Read(&FlashNVM, LORAWAN_NVM_BASE_ADDRESS, sizeof(FlashNVM)) == FLASH_IF_OK) {
							memcpy1( ( uint8_t * )FlashNVM.SeNvmDevJoinKey.DevEui, _devEui, SE_EUI_SIZE);
							free(_devEui);
						} else {
							APP_LOG(TS_OFF, VLEVEL_M, "FAILED READING FLASH \r\n");
						}

						/* Save to NVM */
						if (FLASH_IF_Erase(LORAWAN_NVM_BASE_ADDRESS, FLASH_PAGE_SIZE) == FLASH_IF_OK){
							if(FLASH_IF_Write(LORAWAN_NVM_BASE_ADDRESS, &FlashNVM, sizeof(FlashNVM)) == FLASH_IF_OK){
								APP_LOG(TS_OFF, VLEVEL_M, "###### Success Saving to Flash \r\n");
							}else{
								APP_LOG(TS_OFF, VLEVEL_M, "###### Error Saving to Flash \r\n");
							}
						}else{
							APP_LOG(TS_OFF, VLEVEL_M, "###### Error Erasing Flash \r\n");
						}
            		}else{
            			APP_LOG(TS_OFF, VLEVEL_M, "Memory Allocation Issue \r\n");
            		}
            	}

            	if(appData->Buffer[0] == CONFIG_APPEUI_ID){
					uint8_t *_appEui = (uint8_t *) malloc (sizeof(uint8_t) * SE_EUI_SIZE);

					if(_appEui != NULL){
						memset(_appEui, 0, sizeof(uint8_t) * SE_EUI_SIZE);
						for(uint8_t i = 0; i < SE_EUI_SIZE; i++){
							_appEui[i] = appData->Buffer[i + 1];
						}
						/* Retrieve Data from NVM */
						APP_LOG(TS_OFF, VLEVEL_M, "###### Lora-Configuration Mode: ON \r\n");
						if (FLASH_IF_Read(&FlashNVM, LORAWAN_NVM_BASE_ADDRESS, sizeof(FlashNVM)) == FLASH_IF_OK) {
							memcpy1( ( uint8_t * )FlashNVM.SeNvmDevJoinKey.JoinEui, _appEui, SE_EUI_SIZE);
							free(_appEui);
						} else {
							APP_LOG(TS_OFF, VLEVEL_M, "FAILED READING FLASH \r\n");
						}

						/* Save to NVM */
						if (FLASH_IF_Erase(LORAWAN_NVM_BASE_ADDRESS, FLASH_PAGE_SIZE) == FLASH_IF_OK){
							if(FLASH_IF_Write(LORAWAN_NVM_BASE_ADDRESS, &FlashNVM, sizeof(FlashNVM)) == FLASH_IF_OK){
								APP_LOG(TS_OFF, VLEVEL_M, "###### Success Saving to Flash \r\n");
							}else{
								APP_LOG(TS_OFF, VLEVEL_M, "###### Error Saving to Flash \r\n");
							}
						}else{
							APP_LOG(TS_OFF, VLEVEL_M, "###### Error Erasing Flash \r\n");
						}
					}else{
						APP_LOG(TS_OFF, VLEVEL_M, "Memory Allocation Issue \r\n");
					}
				}

            	if(appData->Buffer[0] == CONFIG_APPKEY_ID){
					uint8_t *_appKey = (uint8_t *) malloc (sizeof(uint8_t) * SE_KEY_SIZE);

					if(_appKey != NULL){
						memset(_appKey, 0, sizeof(uint8_t) * SE_KEY_SIZE);
						for(uint8_t i = 0; i < SE_KEY_SIZE; i++){
							_appKey[i] = appData->Buffer[i + 1];
						}
						/* Retrive Data from NVM */
						APP_LOG(TS_OFF, VLEVEL_M, "###### Lora-Configuration Mode: ON \r\n");
						if (FLASH_IF_Read(&FlashNVM, LORAWAN_NVM_BASE_ADDRESS, sizeof(FlashNVM)) == FLASH_IF_OK) {

							memcpy1( ( uint8_t * )FlashNVM.KeyList[0].KeyValue,     _appKey, SE_KEY_SIZE);
							memcpy1( ( uint8_t * )FlashNVM.KeyList[1].KeyValue,     _appKey, SE_KEY_SIZE);
							memcpy1( ( uint8_t * )FlashNVM.KeyList[2].KeyValue,     _appKey, SE_KEY_SIZE);
							memcpy1( ( uint8_t * )FlashNVM.KeyList[3].KeyValue,     _appKey, SE_KEY_SIZE);

							free(_appKey);
						} else {
							APP_LOG(TS_OFF, VLEVEL_M, "FAILED READING FLASH \r\n");
						}

						/* Save to NVM */
						if (FLASH_IF_Erase(LORAWAN_NVM_BASE_ADDRESS, FLASH_PAGE_SIZE) == FLASH_IF_OK){
							if(FLASH_IF_Write(LORAWAN_NVM_BASE_ADDRESS, &FlashNVM, sizeof(FlashNVM)) == FLASH_IF_OK){
								APP_LOG(TS_OFF, VLEVEL_M, "###### Success Saving to Flash \r\n");
							}else{
								APP_LOG(TS_OFF, VLEVEL_M, "###### Error Saving to Flash \r\n");
							}
						}else{
							APP_LOG(TS_OFF, VLEVEL_M, "###### Error Erasing Flash \r\n");
						}
					}else{
						APP_LOG(TS_OFF, VLEVEL_M, "Memory Allocation Issue \r\n");
					}
				}

            	if(appData->Buffer[0] == CONFIG_INTERVAL_ID){
            		uint32_t parsedSeconds = 0;

					APP_LOG(TS_OFF, VLEVEL_M, "\r\n==============================================\r\n");
					APP_LOG(TS_OFF, VLEVEL_M, "Received data from port 55 (hex): ");
					for (int i = 0; i < appData->BufferSize; i++) {
						APP_LOG(TS_OFF, VLEVEL_M, "%02X ", appData->Buffer[i]);
					}

                    for (int i = 1; i < appData->BufferSize; i++) {
                        parsedSeconds = (parsedSeconds << 8) | appData->Buffer[i];
                    }

            		if (FLASH_IF_Read(&FlashNVM, LORAWAN_NVM_BASE_ADDRESS, sizeof(FlashNVM)) == FLASH_IF_OK) {
            			FlashNVM.pwxTxInterval = (uint64_t)parsedSeconds*1000;
            			TRANSMIT_INTERVAL_MS = parsedSeconds*1000;
					} else {
						APP_LOG(TS_OFF, VLEVEL_M, "FAILED READING FLASH \r\n");
					}

					/* Save to NVM */
					if (FLASH_IF_Erase(LORAWAN_NVM_BASE_ADDRESS, FLASH_PAGE_SIZE) == FLASH_IF_OK){
						if(FLASH_IF_Write(LORAWAN_NVM_BASE_ADDRESS, &FlashNVM, sizeof(FlashNVM)) == FLASH_IF_OK){
							APP_LOG(TS_OFF, VLEVEL_M, "###### Success Saving to Flash \r\n");
						}else{
							APP_LOG(TS_OFF, VLEVEL_M, "###### Error Saving to Flash \r\n");
						}
					}else{
						APP_LOG(TS_OFF, VLEVEL_M, "###### Error Erasing Flash \r\n");
					}

            	}

            	if(appData->Buffer[0] == CONFIG_CONF_UPLINK_ID){
            		uint16_t uplinkCounter = 0;

					APP_LOG(TS_OFF, VLEVEL_M, "\r\n==============================================\r\n");
					APP_LOG(TS_OFF, VLEVEL_M, "Received data from port 55 (hex): ");
					for (int i = 0; i < appData->BufferSize; i++) {
						APP_LOG(TS_OFF, VLEVEL_M, "%02X ", appData->Buffer[i]);
					}
                    for (int i = 1; i < appData->BufferSize; i++) {
                    	uplinkCounter = (uplinkCounter << 8) | appData->Buffer[i];
                    }

            		APP_LOG( TS_OFF, VLEVEL_M, "###### Uplinks before sending Confirmed Uplink: %u \r\n", uplinkCounter);

            		if (FLASH_IF_Read(&FlashNVM, LORAWAN_NVM_BASE_ADDRESS, sizeof(FlashNVM)) == FLASH_IF_OK) {
            			FlashNVM.pwxCnfUplinkCount = (uint16_t)uplinkCounter;
            			MAX_UPLINK_BEFORE_CONFIRMED = uplinkCounter;
            			confUplinkCounter = 0;
					} else {
						APP_LOG(TS_OFF, VLEVEL_M, "FAILED READING FLASH \r\n");
					}

					/* Save to NVM */
					if (FLASH_IF_Erase(LORAWAN_NVM_BASE_ADDRESS, FLASH_PAGE_SIZE) == FLASH_IF_OK){
						if(FLASH_IF_Write(LORAWAN_NVM_BASE_ADDRESS, &FlashNVM, sizeof(FlashNVM)) == FLASH_IF_OK){
							APP_LOG(TS_OFF, VLEVEL_M, "###### Success Saving to Flash \r\n");
						}else{
							APP_LOG(TS_OFF, VLEVEL_M, "###### Error Saving to Flash \r\n");
						}
					}else{
						APP_LOG(TS_OFF, VLEVEL_M, "###### Error Erasing Flash \r\n");
					}

            	}

            	if(appData->Buffer[0] == SYSTEM_DIAGNOSTIC_ID){
					APP_LOG(TS_OFF, VLEVEL_M, "\r\n==============================================\r\n");
					APP_LOG(TS_OFF, VLEVEL_M, "Received data from port 55 (hex): ");
					for (int i = 0; i < appData->BufferSize; i++) {
						APP_LOG(TS_OFF, VLEVEL_M, "%02X ", appData->Buffer[i]);
					}

            		APP_LOG( TS_OFF, VLEVEL_M, "###### Sending System Diagnostic on the next uplink \r\n");
            		sendSystemDiagnostic = true;
            	}

            	if(appData->Buffer[0] == CONFIG_SAVE_REBOOT){
            		if(appData->Buffer[1] == 0x01){
            			HAL_NVIC_SystemReset();
            		}else{

            		}
            	}

            	break;

				case DEVICE_PANIC_PORT:
					if (appData->Buffer != NULL && appData->BufferSize >= 1) {

						if(appData->Buffer[0] == CONFIG_RESTORE_DEV_CONFIG){ //ADR
							APP_LOG(TS_OFF, VLEVEL_M, "\r\n==============================================\r\n");
							APP_LOG(TS_OFF, VLEVEL_M, "\r\n 		[!]	I F*CKED UP MODE [!]              \r\n");
							APP_LOG(TS_OFF, VLEVEL_M, "\r\n==============================================\r\n");
							APP_LOG(TS_OFF, VLEVEL_M, "\r\n Reverting all device configuration to default \r\n");

		            		if (FLASH_IF_Read(&FlashNVM, LORAWAN_NVM_BASE_ADDRESS, sizeof(FlashNVM)) == FLASH_IF_OK) {

								FlashNVM.pwxWaterLevelThresholdHigh = (uint16_t)15.0;
								thresholdLevelHigh = 15.0;
								HAL_Delay(100);

								FlashNVM.pwxWaterLevelThresholdLow = (uint16_t)0.0;
								thresholdLevelLow = 0.0;
								HAL_Delay(100);

								FlashNVM.pwxTxInterval = (uint64_t)900000;
								TRANSMIT_INTERVAL_MS = 900000;
								HAL_Delay(100);

								FlashNVM.pwxCnfUplinkCount = (uint16_t)4;
								MAX_UPLINK_BEFORE_CONFIRMED = 4;
								confUplinkCounter = 0;
								HAL_Delay(100);

								FlashNVM.pwxSamplingMethod = (uint8_t)0;
								samplingMethod = 0;
								HAL_Delay(100);

								FlashNVM.pwxMeasurementMethod = (uint8_t)0;
								measurementMethod = 0;
								HAL_Delay(100);

		            			FlashNVM.pwxSamplingCount = (uint16_t)5;
		            			MAX_WATER_LEVEL_SAMPLES = 5;
						    	waterLevelSamples = (float*)malloc(sizeof(int)*MAX_WATER_LEVEL_SAMPLES);
						    	SAMPLE_INTERVAL_MS = (TRANSMIT_INTERVAL_MS/MAX_WATER_LEVEL_SAMPLES);
						    	HAL_Delay(100);

						    	APP_LOG(TS_OFF, VLEVEL_M, " REVERTED ALL CONFIGURATION TO DEFAULT! \r\n");
							} else {
								APP_LOG(TS_OFF, VLEVEL_M, "FAILED READING FLASH \r\n");
							}

							/* Save to NVM */
							if (FLASH_IF_Erase(LORAWAN_NVM_BASE_ADDRESS, FLASH_PAGE_SIZE) == FLASH_IF_OK){
								if(FLASH_IF_Write(LORAWAN_NVM_BASE_ADDRESS, &FlashNVM, sizeof(FlashNVM)) == FLASH_IF_OK){
									APP_LOG(TS_OFF, VLEVEL_M, "###### Success Saving to Flash \r\n");
								}else{
									APP_LOG(TS_OFF, VLEVEL_M, "###### Error Saving to Flash \r\n");
								}
							}else{
								APP_LOG(TS_OFF, VLEVEL_M, "###### Error Erasing Flash \r\n");
							}

							HAL_Delay(500);
							HAL_NVIC_SystemReset();
							APP_LOG(TS_OFF, VLEVEL_M, "\r\n==============================================\r\n");
						}

						if(appData->Buffer[0] == CONFIG_CONTINUOUS_UPLINK){ //ADR

							if(appData->Buffer[1] == 0){
								APP_LOG(TS_OFF, VLEVEL_M, "\r\n==============================================\r\n");
								APP_LOG(TS_OFF, VLEVEL_M, "\r\n 	[!]	CONTINUOUS UPLINK MODE OFF [!]              \r\n");
								APP_LOG(TS_OFF, VLEVEL_M, "\r\n==============================================\r\n");
								continuousMode = false;
								HAL_Delay(500);
							}

							if(appData->Buffer[1] == 1){
								APP_LOG(TS_OFF, VLEVEL_M, "\r\n==============================================\r\n");
								APP_LOG(TS_OFF, VLEVEL_M, "\r\n 	[!]	CONTINUOUS UPLINK MODE ON [!]              \r\n");
								APP_LOG(TS_OFF, VLEVEL_M, "\r\n==============================================\r\n");
								continuousMode = true;
								HAL_Delay(500);
							}

							APP_LOG(TS_OFF, VLEVEL_M, "\r\n==============================================\r\n");
						}
					}
				break;

				case DEVICE_CONFIG_PORT:
					if(appData->Buffer != NULL && appData->BufferSize >1){
		            	if(appData->Buffer[0] == CONFIG_SAMPLING_COUNT_ID){
		            		int parsedCount = 0;

							APP_LOG(TS_OFF, VLEVEL_M, "\r\n==============================================\r\n");
							APP_LOG(TS_OFF, VLEVEL_M, "Received data from port 55 (hex): ");
							for (int i = 0; i < appData->BufferSize; i++) {
								APP_LOG(TS_OFF, VLEVEL_M, "%02X ", appData->Buffer[i]);
							}
		                    for (int i = 1; i < appData->BufferSize; i++) {
		                    	parsedCount = (parsedCount << 8) | appData->Buffer[i];
		                    }

		            		APP_LOG( TS_OFF, VLEVEL_M, "###### Sampling Count: %u \r\n", parsedCount);

		            		if (FLASH_IF_Read(&FlashNVM, LORAWAN_NVM_BASE_ADDRESS, sizeof(FlashNVM)) == FLASH_IF_OK) {
		            			FlashNVM.pwxSamplingCount = (uint16_t)parsedCount;
		            			MAX_WATER_LEVEL_SAMPLES = parsedCount;
						    	waterLevelSamples = (float*)malloc(sizeof(int)*MAX_WATER_LEVEL_SAMPLES);
						    	SAMPLE_INTERVAL_MS = (TRANSMIT_INTERVAL_MS/MAX_WATER_LEVEL_SAMPLES);
							} else {
								APP_LOG(TS_OFF, VLEVEL_M, "FAILED READING FLASH \r\n");
							}

							/* Save to NVM */
							if (FLASH_IF_Erase(LORAWAN_NVM_BASE_ADDRESS, FLASH_PAGE_SIZE) == FLASH_IF_OK){
								if(FLASH_IF_Write(LORAWAN_NVM_BASE_ADDRESS, &FlashNVM, sizeof(FlashNVM)) == FLASH_IF_OK){
									APP_LOG(TS_OFF, VLEVEL_M, "###### Success Saving to Flash \r\n");
								}else{
									APP_LOG(TS_OFF, VLEVEL_M, "###### Error Saving to Flash \r\n");
								}
							}else{
								APP_LOG(TS_OFF, VLEVEL_M, "###### Error Erasing Flash \r\n");
							}
		            	}

		            	if(appData->Buffer[0] == CONFIG_LEVEL_THRESHOLD_HIGH_ID){
		            		int parsedLevel = 0;

							APP_LOG(TS_OFF, VLEVEL_M, "\r\n==============================================\r\n");
							APP_LOG(TS_OFF, VLEVEL_M, "Received data from port 55 (hex): ");
							for (int i = 0; i < appData->BufferSize; i++) {
								APP_LOG(TS_OFF, VLEVEL_M, "%02X ", appData->Buffer[i]);
							}
		                    for (int i = 1; i < appData->BufferSize; i++) {
		                    	parsedLevel = (parsedLevel << 8) | appData->Buffer[i];
		                    }

		            		APP_LOG( TS_OFF, VLEVEL_M, "###### Water Level HIGH Threshold: %u \r\n", parsedLevel);

		            		if (FLASH_IF_Read(&FlashNVM, LORAWAN_NVM_BASE_ADDRESS, sizeof(FlashNVM)) == FLASH_IF_OK) {
		            			FlashNVM.pwxWaterLevelThresholdHigh = (uint16_t)parsedLevel;
		            			thresholdLevelHigh = parsedLevel;
							} else {
								APP_LOG(TS_OFF, VLEVEL_M, "FAILED READING FLASH \r\n");
							}

							/* Save to NVM */
							if (FLASH_IF_Erase(LORAWAN_NVM_BASE_ADDRESS, FLASH_PAGE_SIZE) == FLASH_IF_OK){
								if(FLASH_IF_Write(LORAWAN_NVM_BASE_ADDRESS, &FlashNVM, sizeof(FlashNVM)) == FLASH_IF_OK){
									APP_LOG(TS_OFF, VLEVEL_M, "###### Success Saving to Flash \r\n");
								}else{
									APP_LOG(TS_OFF, VLEVEL_M, "###### Error Saving to Flash \r\n");
								}
							}else{
								APP_LOG(TS_OFF, VLEVEL_M, "###### Error Erasing Flash \r\n");
							}
		            	}

		            	if(appData->Buffer[0] == CONFIG_LEVEL_THRESHOLD_LOW_ID){
		            		int parsedLevel = 0;

							APP_LOG(TS_OFF, VLEVEL_M, "\r\n==============================================\r\n");
							APP_LOG(TS_OFF, VLEVEL_M, "Received data from port 55 (hex): ");
							for (int i = 0; i < appData->BufferSize; i++) {
								APP_LOG(TS_OFF, VLEVEL_M, "%02X ", appData->Buffer[i]);
							}
		                    for (int i = 1; i < appData->BufferSize; i++) {
		                    	parsedLevel = (parsedLevel << 8) | appData->Buffer[i];
		                    }

		            		APP_LOG( TS_OFF, VLEVEL_M, "###### Water Level LOW Threshold: %u \r\n", parsedLevel);

		            		if (FLASH_IF_Read(&FlashNVM, LORAWAN_NVM_BASE_ADDRESS, sizeof(FlashNVM)) == FLASH_IF_OK) {
		            			FlashNVM.pwxWaterLevelThresholdLow = (uint16_t)parsedLevel;
		            			thresholdLevelLow = parsedLevel;
							} else {
								APP_LOG(TS_OFF, VLEVEL_M, "FAILED READING FLASH \r\n");
							}

							/* Save to NVM */
							if (FLASH_IF_Erase(LORAWAN_NVM_BASE_ADDRESS, FLASH_PAGE_SIZE) == FLASH_IF_OK){
								if(FLASH_IF_Write(LORAWAN_NVM_BASE_ADDRESS, &FlashNVM, sizeof(FlashNVM)) == FLASH_IF_OK){
									APP_LOG(TS_OFF, VLEVEL_M, "###### Success Saving to Flash \r\n");
								}else{
									APP_LOG(TS_OFF, VLEVEL_M, "###### Error Saving to Flash \r\n");
								}
							}else{
								APP_LOG(TS_OFF, VLEVEL_M, "###### Error Erasing Flash \r\n");
							}
		            	}

		            	if(appData->Buffer[0] == SAMPLING_METHOD_ID){
		            		int parsedSamplingMethod = 0;

							APP_LOG(TS_OFF, VLEVEL_M, "\r\n==============================================\r\n");
							APP_LOG(TS_OFF, VLEVEL_M, "Received data from port 55 (hex): ");
							for (int i = 0; i < appData->BufferSize; i++) {
								APP_LOG(TS_OFF, VLEVEL_M, "%02X ", appData->Buffer[i]);
							}
		                    for (int i = 1; i < appData->BufferSize; i++) {
		                    	parsedSamplingMethod = (parsedSamplingMethod << 8) | appData->Buffer[i];
		                    }

		            		APP_LOG( TS_OFF, VLEVEL_M, "###### Sampling Method: %u \r\n", parsedSamplingMethod);

		            		if (FLASH_IF_Read(&FlashNVM, LORAWAN_NVM_BASE_ADDRESS, sizeof(FlashNVM)) == FLASH_IF_OK) {
		            			FlashNVM.pwxSamplingMethod = (uint8_t)parsedSamplingMethod;
		            			samplingMethod = parsedSamplingMethod;
							} else {
								APP_LOG(TS_OFF, VLEVEL_M, "FAILED READING FLASH \r\n");
							}

							/* Save to NVM */
							if (FLASH_IF_Erase(LORAWAN_NVM_BASE_ADDRESS, FLASH_PAGE_SIZE) == FLASH_IF_OK){
								if(FLASH_IF_Write(LORAWAN_NVM_BASE_ADDRESS, &FlashNVM, sizeof(FlashNVM)) == FLASH_IF_OK){
									APP_LOG(TS_OFF, VLEVEL_M, "###### Success Saving to Flash \r\n");
								}else{
									APP_LOG(TS_OFF, VLEVEL_M, "###### Error Saving to Flash \r\n");
								}
							}else{
								APP_LOG(TS_OFF, VLEVEL_M, "###### Error Erasing Flash \r\n");
							}
		            	}

		            	if(appData->Buffer[0] == MEASUREMENT_METHOD_ID){
		            		int parsedMeasurementMethod = 0;

							APP_LOG(TS_OFF, VLEVEL_M, "\r\n==============================================\r\n");
							APP_LOG(TS_OFF, VLEVEL_M, "Received data from port 55 (hex): ");
							for (int i = 0; i < appData->BufferSize; i++) {
								APP_LOG(TS_OFF, VLEVEL_M, "%02X ", appData->Buffer[i]);
							}
		                    for (int i = 1; i < appData->BufferSize; i++) {
		                    	parsedMeasurementMethod = (parsedMeasurementMethod << 8) | appData->Buffer[i];
		                    }

		            		APP_LOG( TS_OFF, VLEVEL_M, "###### Sampling Method: %u \r\n", parsedMeasurementMethod);

		            		if (FLASH_IF_Read(&FlashNVM, LORAWAN_NVM_BASE_ADDRESS, sizeof(FlashNVM)) == FLASH_IF_OK) {
		            			FlashNVM.pwxMeasurementMethod = (uint8_t)parsedMeasurementMethod;
		            			measurementMethod = parsedMeasurementMethod;
							} else {
								APP_LOG(TS_OFF, VLEVEL_M, "FAILED READING FLASH \r\n");
							}

							/* Save to NVM */
							if (FLASH_IF_Erase(LORAWAN_NVM_BASE_ADDRESS, FLASH_PAGE_SIZE) == FLASH_IF_OK){
								if(FLASH_IF_Write(LORAWAN_NVM_BASE_ADDRESS, &FlashNVM, sizeof(FlashNVM)) == FLASH_IF_OK){
									APP_LOG(TS_OFF, VLEVEL_M, "###### Success Saving to Flash \r\n");
								}else{
									APP_LOG(TS_OFF, VLEVEL_M, "###### Error Saving to Flash \r\n");
								}
							}else{
								APP_LOG(TS_OFF, VLEVEL_M, "###### Error Erasing Flash \r\n");
							}
		            	}

					}
					break;

//            case 55:
//            	if (appData->Buffer != NULL && appData->BufferSize >= 1) {
//
//					if(appData->Buffer[0] == 5){ //ADR
//						APP_LOG(TS_OFF, VLEVEL_M, "\r\n==============================================\r\n");
//						APP_LOG(TS_OFF, VLEVEL_M, "Received data from port 25 (hex): ");
//						for (int i = 0; i < appData->BufferSize; i++) {
//							APP_LOG(TS_OFF, VLEVEL_M, "%02X ", appData->Buffer[i]);
//						}
//
//						if(appData->Buffer[1] != 0){
//							APP_LOG(TS_OFF, VLEVEL_M, "Adaptive Data Rate - ENABLED! \r\n");
//					     	LmHandlerSetAdrEnable(true);
//						} else{
//							APP_LOG(TS_OFF, VLEVEL_M, "Adaptive Data Rate - DISABLED! \r\n");
//							LmHandlerSetAdrEnable(false);
//						}
//						APP_LOG(TS_OFF, VLEVEL_M, "\r\n==============================================\r\n");
//					}
//
//					if(appData->Buffer[0] == 6){ //SF
//						int parsedCount = 0;
//						APP_LOG(TS_OFF, VLEVEL_M, "\r\n==============================================\r\n");
//						APP_LOG(TS_OFF, VLEVEL_M, "Received data from port 25 (hex): ");
//						for (int i = 0; i < appData->BufferSize; i++) {
//							APP_LOG(TS_OFF, VLEVEL_M, "%02X ", appData->Buffer[i]);
//						}
//                        for (int i = 1; i < appData->BufferSize; i++) {
//                        	parsedCount = (parsedCount << 8) | appData->Buffer[i];
//                        }
//						MAX_WATER_LEVEL_SAMPLES = parsedCount;
//				    	waterLevelSamples = (float*)malloc(sizeof(int)*MAX_WATER_LEVEL_SAMPLES);
//						APP_LOG(TS_OFF, VLEVEL_M, "Sampling Count: %d", parsedCount);
//						APP_LOG(TS_OFF, VLEVEL_M, "\r\n==============================================\r\n");
//
//					}
//            	}
//            break;

            default:
              break;
          }
        }
      }
    }
    if (params->RxSlot < RX_SLOT_NONE)
    {
      APP_LOG(TS_OFF, VLEVEL_H, "###### D/L FRAME:%04d | PORT:%d | DR:%d | SLOT:%s | RSSI:%d | SNR:%d\r\n",
              params->DownlinkCounter, RxPort, params->Datarate, slotStrings[params->RxSlot],
              params->Rssi, params->Snr);
    }
  }
  /* USER CODE END OnRxData_1 */
}

static void SendTxData(void)
{
  /* USER CODE BEGIN SendTxData_1 */
	uint8_t currentDR = 0;
	currentTime = HAL_GetTick();

	// Check if it's time to sample the water level
	if ((currentTime - lastSampleTime) >= SAMPLE_INTERVAL_MS) {
		APP_LOG(TS_OFF, VLEVEL_M, "============================================= \r\n");
		APP_LOG(TS_OFF, VLEVEL_M, "            DATA SAMPLING: %d           \r\n", sampleIndex+1);
		APP_LOG(TS_OFF, VLEVEL_M, "============================================= \r\n");
		if(measurementMethod == 0){
			fetchSensorData();
		} else {
			fetchSensorData();
		}

		lastSampleTime = currentTime;
	}

	// Check if it's time to send the transmission
	//if ((currentTime - lastTransmitTime) >= TRANSMIT_INTERVAL_MS || hasJoined == false) {   // Buggy
	if (!skipScheduledTransmission){
		if (sampleIndex >= MAX_WATER_LEVEL_SAMPLES || hasJoined == false) {
			LmHandlerErrorStatus_t status = LORAMAC_HANDLER_ERROR;
			UTIL_TIMER_Time_t nextTxIn = 0;
		    uint8_t msg[100]; 					    // Buffer for storing messages to be transmitted via UART

			  if (LmHandlerIsBusy() == false)
			  {
				APP_LOG(TS_OFF, VLEVEL_M, "============================================= \r\n");
				APP_LOG(TS_OFF, VLEVEL_M, "            SCHEDULED TRANSMISSION            \r\n");
				APP_LOG(TS_OFF, VLEVEL_M, "============================================= \r\n");
			    //HAL_StatusTypeDef status;
				//LmHandlerGetTxDatarate(&currentDR);
				if(LmHandlerGetTxDatarate(&currentDR) == LORAMAC_HANDLER_SUCCESS){
					APP_LOG(TS_OFF, VLEVEL_M, "Current Data Rate: %d           \r\n", currentDR);
				}

			    AppData.Port = LORAWAN_USER_APP_PORT;
			    _doneScanning = false;

			    if(hasJoined == false){
			    	fetchSensorDataOnce();
			    }
			    _doneScanning = true;

			    HAL_Delay(200);
			    fetchLTCData();							//READ LTC DATA
			    HAL_Delay(50);

			    for(int x = 0; x < AppData.BufferSize; x++){
			    		AppData.Buffer[x] = 0;
			    	}

			    /* Prepare Data */

			    waterLevelLatest 	= waterLevelLatest * 100;
			    waterLevel 		 	= waterLevel * 100;
			    waterLevelMin	 	= waterLevelMin * 100;
			    waterLevelMax	 	= waterLevelMax * 100;

				if(sendSystemDiagnostic == true){
					transmissionType = 2;
					int isZeroCalib  = 0;
					int measureMeth  = measurementMethod == 0 ? 0 : 1;
					int samplingMeth = samplingMethod == 0 ? 0 : 1;
					int threshMeth	 = continuousMode == true ? 1 : 0;
					int modbusError  = isModbusDefective == true ? 1 : 0;
					int levelBreach  = isLevelBreached == true ? 1 : 0;

					systemDiagnostic = (isZeroCalib << 5) |
									   (measureMeth << 4) |
									   (samplingMeth << 3) |
									   (threshMeth  << 2) |
									   (modbusError << 1) |
									   (levelBreach << 0);
				}

				uint8_t i = 0;
				AppData.Buffer[i++] = (uint8_t)transmissionType;
				AppData.Buffer[i++] = (uint8_t)((uint16_t)waterLevelLatest >> 8);
				AppData.Buffer[i++] = (uint8_t)((uint16_t)waterLevelLatest & 0xFF);
				AppData.Buffer[i++] = (uint8_t)((uint16_t)waterLevel >> 8);
				AppData.Buffer[i++] = (uint8_t)((uint16_t)waterLevel & 0xFF);
				AppData.Buffer[i++] = (uint8_t)((uint16_t)waterLevelMin >> 8);
				AppData.Buffer[i++] = (uint8_t)((uint16_t)waterLevelMin & 0xFF);
				AppData.Buffer[i++] = (uint8_t)((uint16_t)waterLevelMax >> 8);
				AppData.Buffer[i++] = (uint8_t)((uint16_t)waterLevelMax & 0xFF);
				AppData.Buffer[i++] = (uint8_t)((uint16_t)VBAT >> 8);
				AppData.Buffer[i++] = (uint8_t)((uint16_t)VBAT & 0xFF);
				AppData.Buffer[i++] = (uint8_t)((uint16_t)VIN >> 8);
				AppData.Buffer[i++] = (uint8_t)((uint16_t)VIN & 0xFF);
				AppData.Buffer[i++] = (uint8_t)((uint16_t)VSYS >> 8);
				AppData.Buffer[i++] = (uint8_t)((uint16_t)VSYS & 0xFF);
				AppData.Buffer[i++] = (uint8_t)((uint16_t)IBAT >> 8);
				AppData.Buffer[i++] = (uint8_t)((uint16_t)IBAT & 0xFF);
				AppData.Buffer[i++] = (uint8_t)((uint16_t)IIN >> 8);
				AppData.Buffer[i++] = (uint8_t)((uint16_t)IIN & 0xFF);
				AppData.Buffer[i++] = (uint8_t)((uint16_t)ICHARGE_DAC >> 8);
				AppData.Buffer[i++] = (uint8_t)((uint16_t)ICHARGE_DAC & 0xFF);
				AppData.Buffer[i++] = (uint8_t)((uint16_t)SYSTEM_STATUS >> 8);
				AppData.Buffer[i++] = (uint8_t)((uint16_t)SYSTEM_STATUS & 0xFF);
				if(sendSystemDiagnostic == true){
					AppData.Buffer[i++] = (uint8_t)systemDiagnostic;
					sendSystemDiagnostic = false;
				}

				AppData.BufferSize = i;

				sprintf((char*)msg, "Payload Buffer Size: %u\r\n\r\n", AppData.BufferSize);
				HAL_UART_Transmit(&huart2, msg, strlen((char*)msg), 1000);

			    if ((JoinLedTimer.IsRunning) && (LmHandlerJoinStatus() == LORAMAC_HANDLER_SET))
			    {
			      UTIL_TIMER_Stop(&JoinLedTimer);
			      HAL_GPIO_WritePin(LED3_GPIO_Port, LED3_Pin, GPIO_PIN_RESET); /* LED_RED */
			    }

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
				//status = LmHandlerSend(&AppData, LmHandlerParams.IsTxConfirmed, false);

				if(confUplinkCounter >= MAX_UPLINK_BEFORE_CONFIRMED){
					APP_LOG(TS_ON, VLEVEL_L, "### SENDING CONFIRMED UPLINK \r\n");
					APP_LOG(TS_ON, VLEVEL_L, "% Uplink Counter %u \r\n", confUplinkCounter);
					confUplinkCounter += 1;
					//confUplinkCounter = 0;
					status = LmHandlerSend(&AppData, LmHandlerParamsConfirmed.IsTxConfirmed, false);
				}else{
					status = LmHandlerSend(&AppData, LmHandlerParams.IsTxConfirmed, false);
					APP_LOG(TS_ON, VLEVEL_L, "%u Remaining Uplink/s before sending Confirmed Uplink \r\n", (MAX_UPLINK_BEFORE_CONFIRMED-confUplinkCounter));
					confUplinkCounter += 1;
				}

				if((confUplinkCounter - MAX_UPLINK_BEFORE_CONFIRMED) > 10){
					APP_LOG(TS_ON, VLEVEL_L, "### Confirmed Uplinks Failed! \r\n");
					APP_LOG(TS_ON, VLEVEL_L, "### Resetting Device! \r\n");
					panicMode();
					//HAL_NVIC_SystemReset();
				}
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


			    if (LORAMAC_HANDLER_SUCCESS == status)
			    {
			      APP_LOG(TS_ON, VLEVEL_L, "SEND REQUEST\r\n");
			    }
			    else if (LORAMAC_HANDLER_DUTYCYCLE_RESTRICTED == status)
			    {
			      nextTxIn = LmHandlerGetDutyCycleWaitTime();
			      //nextTxIn = 300000; //5minutes
			      if (nextTxIn > 0)
			      {
			        APP_LOG(TS_ON, VLEVEL_L, "Next Tx in  : ~%d second(s)\r\n", (nextTxIn / 1000));
			        UTIL_TIMER_Stop(&TxTimer);
			        //UTIL_TIMER_SetPeriod(&TxTimer, MAX(nextTxIn, TxPeriodicity));
			        UTIL_TIMER_SetPeriod(&TxTimer, MAX(nextTxIn, TxPeriodicity));
			        UTIL_TIMER_Start(&TxTimer);
			      }
			    }
			  }
			lastTransmitTime = currentTime;

			/*
			 * Reset Water Level Values
			 */
			sampleIndex = 0;
			for(int i = 0; i < MAX_WATER_LEVEL_SAMPLES; i++){
				waterLevelSamples[i] = 0;
			}
			waterLevelMin    = 0;
			waterLevelMax    = 0;
			waterLevelLatest = 0;
		}
	}



  if (EventType == TX_ON_TIMER)
  {
	APP_LOG(TS_ON, VLEVEL_L, "Next Scan in  : ~%d second(s)\r\n", ((SAMPLE_INTERVAL_MS - (currentTime - lastSampleTime)) / 1000));
//    UTIL_TIMER_Stop(&TxTimer);
//    //UTIL_TIMER_SetPeriod(&TxTimer, MAX(nextTxIn, TxPeriodicity));
//    UTIL_TIMER_SetPeriod(&TxTimer, MAX(nextTxIn, TxPeriodicity));
//    UTIL_TIMER_Start(&TxTimer);
  }

  /* USER CODE END SendTxData_1 */
}

static void OnTxTimerEvent(void *context)
{
  /* USER CODE BEGIN OnTxTimerEvent_1 */

  /* USER CODE END OnTxTimerEvent_1 */
  UTIL_SEQ_SetTask((1 << CFG_SEQ_Task_LoRaSendOnTxTimerOrButtonEvent), CFG_SEQ_Prio_0);

  /*Wait for next tx slot*/
  UTIL_TIMER_Start(&TxTimer);
  /* USER CODE BEGIN OnTxTimerEvent_2 */

  /* USER CODE END OnTxTimerEvent_2 */
}

/* USER CODE BEGIN PrFD_LedEvents */
static void OnTxTimerLedEvent(void *context)
{
  HAL_GPIO_WritePin(LED2_GPIO_Port, LED2_Pin, GPIO_PIN_RESET); /* LED_GREEN */
}

static void OnRxTimerLedEvent(void *context)
{
  HAL_GPIO_WritePin(LED1_GPIO_Port, LED1_Pin, GPIO_PIN_RESET); /* LED_BLUE */
}

static void OnJoinTimerLedEvent(void *context)
{
  HAL_GPIO_TogglePin(LED3_GPIO_Port, LED3_Pin); /* LED_RED */
}

/* USER CODE END PrFD_LedEvents */

static void OnTxData(LmHandlerTxParams_t *params)
{
  /* USER CODE BEGIN OnTxData_1 */
  if ((params != NULL))
  {
    /* Process Tx event only if its a mcps response to prevent some internal events (mlme) */
    if (params->IsMcpsConfirm != 0)
    {
      HAL_GPIO_WritePin(LED2_GPIO_Port, LED2_Pin, GPIO_PIN_SET); /* LED_GREEN */
      UTIL_TIMER_Start(&TxLedTimer);

      APP_LOG(TS_OFF, VLEVEL_M, "\r\n###### ========== MCPS-Confirm =============\r\n");
      APP_LOG(TS_OFF, VLEVEL_H, "###### U/L FRAME:%04d | PORT:%d | DR:%d | PWR:%d", params->UplinkCounter,
              params->AppData.Port, params->Datarate, params->TxPower);

      hasJoined = true;

      APP_LOG(TS_OFF, VLEVEL_H, " | MSG TYPE:");
      if (params->MsgType == LORAMAC_HANDLER_CONFIRMED_MSG)
      {
        APP_LOG(TS_OFF, VLEVEL_H, "CONFIRMED [%s]\r\n", (params->AckReceived != 0) ? "ACK" : "NACK");
      }
      else
      {
        APP_LOG(TS_OFF, VLEVEL_H, "UNCONFIRMED\r\n");
      }

//      LmHandlerSetAdrEnable(true);

    }
  }
  /* USER CODE END OnTxData_1 */
}

static void OnJoinRequest(LmHandlerJoinParams_t *joinParams)
{
  /* USER CODE BEGIN OnJoinRequest_1 */
  if (joinParams != NULL)
  {
    if (joinParams->Status == LORAMAC_HANDLER_SUCCESS)
    {
      UTIL_TIMER_Stop(&JoinLedTimer);
      HAL_GPIO_WritePin(LED3_GPIO_Port, LED3_Pin, GPIO_PIN_RESET); /* LED_RED */

      APP_LOG(TS_OFF, VLEVEL_M, "\r\n###### = JOINED = ");
      APP_LOG(TS_OFF, VLEVEL_M, "\r\nWriting Devnonce to Flash:%d \r\n", getDevnonce);
      write_devnonce_to_flash(getDevnonce);
      if (joinParams->Mode == ACTIVATION_TYPE_ABP)
      {
        APP_LOG(TS_OFF, VLEVEL_M, "ABP ======================\r\n");
      }
      else
      {
        APP_LOG(TS_OFF, VLEVEL_M, "OTAA =====================\r\n");
      }
    }
    else
    {
      APP_LOG(TS_OFF, VLEVEL_M, "\r\n###### = JOIN FAILED\r\n");
      APP_LOG(TS_OFF, VLEVEL_M, "\r\nDevnonce:%d \r\n", getDevnonce);
    }

    APP_LOG(TS_OFF, VLEVEL_H, "###### U/L FRAME:JOIN | DR:%d | PWR:%d\r\n", joinParams->Datarate, joinParams->TxPower);
  }
  /* USER CODE END OnJoinRequest_1 */
}

static void OnBeaconStatusChange(LmHandlerBeaconParams_t *params)
{
  /* USER CODE BEGIN OnBeaconStatusChange_1 */
  if (params != NULL)
  {
    switch (params->State)
    {
      default:
      case LORAMAC_HANDLER_BEACON_LOST:
      {
        APP_LOG(TS_OFF, VLEVEL_M, "\r\n###### BEACON LOST\r\n");
        break;
      }
      case LORAMAC_HANDLER_BEACON_RX:
      {
        APP_LOG(TS_OFF, VLEVEL_M,
                "\r\n###### BEACON RECEIVED | DR:%d | RSSI:%d | SNR:%d | FQ:%d | TIME:%d | DESC:%d | "
                "INFO:02X%02X%02X %02X%02X%02X\r\n",
                params->Info.Datarate, params->Info.Rssi, params->Info.Snr, params->Info.Frequency,
                params->Info.Time.Seconds, params->Info.GwSpecific.InfoDesc,
                params->Info.GwSpecific.Info[0], params->Info.GwSpecific.Info[1],
                params->Info.GwSpecific.Info[2], params->Info.GwSpecific.Info[3],
                params->Info.GwSpecific.Info[4], params->Info.GwSpecific.Info[5]);
        break;
      }
      case LORAMAC_HANDLER_BEACON_NRX:
      {
        APP_LOG(TS_OFF, VLEVEL_M, "\r\n###### BEACON NOT RECEIVED\r\n");
        break;
      }
    }
  }
  /* USER CODE END OnBeaconStatusChange_1 */
}

static void OnSysTimeUpdate(void)
{
  /* USER CODE BEGIN OnSysTimeUpdate_1 */

  /* USER CODE END OnSysTimeUpdate_1 */
}

static void OnClassChange(DeviceClass_t deviceClass)
{
  /* USER CODE BEGIN OnClassChange_1 */
  APP_LOG(TS_OFF, VLEVEL_M, "Switch to Class %c done\r\n", "ABC"[deviceClass]);
  /* USER CODE END OnClassChange_1 */
}

static void OnMacProcessNotify(void)
{
  /* USER CODE BEGIN OnMacProcessNotify_1 */

  /* USER CODE END OnMacProcessNotify_1 */
  UTIL_SEQ_SetTask((1 << CFG_SEQ_Task_LmHandlerProcess), CFG_SEQ_Prio_0);

  /* USER CODE BEGIN OnMacProcessNotify_2 */

  /* USER CODE END OnMacProcessNotify_2 */
}

static void OnTxPeriodicityChanged(uint32_t periodicity)
{
  /* USER CODE BEGIN OnTxPeriodicityChanged_1 */

  /* USER CODE END OnTxPeriodicityChanged_1 */
  TxPeriodicity = periodicity;

  if (TxPeriodicity == 0)
  {
    /* Revert to application default periodicity */
    TxPeriodicity = APP_TX_DUTYCYCLE;
  }

  /* Update timer periodicity */
  UTIL_TIMER_Stop(&TxTimer);
  UTIL_TIMER_SetPeriod(&TxTimer, TxPeriodicity);
  UTIL_TIMER_Start(&TxTimer);
  /* USER CODE BEGIN OnTxPeriodicityChanged_2 */

  /* USER CODE END OnTxPeriodicityChanged_2 */
}

static void OnTxFrameCtrlChanged(LmHandlerMsgTypes_t isTxConfirmed)
{
  /* USER CODE BEGIN OnTxFrameCtrlChanged_1 */

  /* USER CODE END OnTxFrameCtrlChanged_1 */
  LmHandlerParams.IsTxConfirmed = isTxConfirmed;
  /* USER CODE BEGIN OnTxFrameCtrlChanged_2 */

  /* USER CODE END OnTxFrameCtrlChanged_2 */
}

static void OnPingSlotPeriodicityChanged(uint8_t pingSlotPeriodicity)
{
  /* USER CODE BEGIN OnPingSlotPeriodicityChanged_1 */

  /* USER CODE END OnPingSlotPeriodicityChanged_1 */
  LmHandlerParams.PingSlotPeriodicity = pingSlotPeriodicity;
  /* USER CODE BEGIN OnPingSlotPeriodicityChanged_2 */

  /* USER CODE END OnPingSlotPeriodicityChanged_2 */
}

static void OnSystemReset(void)
{
  /* USER CODE BEGIN OnSystemReset_1 */

  /* USER CODE END OnSystemReset_1 */
  if ((LORAMAC_HANDLER_SUCCESS == LmHandlerHalt()) && (LmHandlerJoinStatus() == LORAMAC_HANDLER_SET))
  {
    NVIC_SystemReset();
  }
  /* USER CODE BEGIN OnSystemReset_Last */

  /* USER CODE END OnSystemReset_Last */
}

static void StopJoin(void)
{
  /* USER CODE BEGIN StopJoin_1 */
  HAL_GPIO_WritePin(LED1_GPIO_Port, LED1_Pin, GPIO_PIN_SET); /* LED_BLUE */
  HAL_GPIO_WritePin(LED2_GPIO_Port, LED2_Pin, GPIO_PIN_SET); /* LED_GREEN */
  HAL_GPIO_WritePin(LED3_GPIO_Port, LED3_Pin, GPIO_PIN_SET); /* LED_RED */
  /* USER CODE END StopJoin_1 */

  UTIL_TIMER_Stop(&TxTimer);

  if (LORAMAC_HANDLER_SUCCESS != LmHandlerStop())
  {
    APP_LOG(TS_OFF, VLEVEL_M, "LmHandler Stop on going ...\r\n");
  }
  else
  {
    APP_LOG(TS_OFF, VLEVEL_M, "LmHandler Stopped\r\n");
    if (LORAWAN_DEFAULT_ACTIVATION_TYPE == ACTIVATION_TYPE_ABP)
    {
      ActivationType = ACTIVATION_TYPE_OTAA;
      APP_LOG(TS_OFF, VLEVEL_M, "LmHandler switch to OTAA mode\r\n");
    }
    else
    {
      ActivationType = ACTIVATION_TYPE_ABP;
      APP_LOG(TS_OFF, VLEVEL_M, "LmHandler switch to ABP mode\r\n");
    }
    LmHandlerConfigure(&LmHandlerParams);
    LmHandlerJoin(ActivationType, true);
    UTIL_TIMER_Start(&TxTimer);
  }
  UTIL_TIMER_Start(&StopJoinTimer);
  /* USER CODE BEGIN StopJoin_Last */

  /* USER CODE END StopJoin_Last */
}

static void OnStopJoinTimerEvent(void *context)
{
  /* USER CODE BEGIN OnStopJoinTimerEvent_1 */

  /* USER CODE END OnStopJoinTimerEvent_1 */
  if (ActivationType == LORAWAN_DEFAULT_ACTIVATION_TYPE)
  {
    UTIL_SEQ_SetTask((1 << CFG_SEQ_Task_LoRaStopJoinEvent), CFG_SEQ_Prio_0);
  }
  /* USER CODE BEGIN OnStopJoinTimerEvent_Last */
  HAL_GPIO_WritePin(LED1_GPIO_Port, LED1_Pin, GPIO_PIN_RESET); /* LED_BLUE */
  HAL_GPIO_WritePin(LED2_GPIO_Port, LED2_Pin, GPIO_PIN_RESET); /* LED_GREEN */
  HAL_GPIO_WritePin(LED3_GPIO_Port, LED3_Pin, GPIO_PIN_RESET); /* LED_RED */
  /* USER CODE END OnStopJoinTimerEvent_Last */
}

static void StoreContext(void)
{
  LmHandlerErrorStatus_t status = LORAMAC_HANDLER_ERROR;

  /* USER CODE BEGIN StoreContext_1 */

  /* USER CODE END StoreContext_1 */
  status = LmHandlerNvmDataStore();

  if (status == LORAMAC_HANDLER_NVM_DATA_UP_TO_DATE)
  {
    APP_LOG(TS_OFF, VLEVEL_M, "NVM DATA UP TO DATE\r\n");
  }
  else if (status == LORAMAC_HANDLER_ERROR)
  {
    APP_LOG(TS_OFF, VLEVEL_M, "NVM DATA STORE FAILED\r\n");
  }
  /* USER CODE BEGIN StoreContext_Last */

  /* USER CODE END StoreContext_Last */
}

static void OnNvmDataChange(LmHandlerNvmContextStates_t state)
{
  /* USER CODE BEGIN OnNvmDataChange_1 */

  /* USER CODE END OnNvmDataChange_1 */
  if (state == LORAMAC_HANDLER_NVM_STORE)
  {
    APP_LOG(TS_OFF, VLEVEL_M, "NVM DATA STORED\r\n");
  }
  else
  {
    APP_LOG(TS_OFF, VLEVEL_M, "NVM DATA RESTORED\r\n");
  }
  /* USER CODE BEGIN OnNvmDataChange_Last */

  /* USER CODE END OnNvmDataChange_Last */
}

static void OnStoreContextRequest(void *nvm, uint32_t nvm_size)
{
  /* USER CODE BEGIN OnStoreContextRequest_1 */

  /* USER CODE END OnStoreContextRequest_1 */
  /* store nvm in flash */
  if (FLASH_IF_Erase(LORAWAN_NVM_BASE_ADDRESS, FLASH_PAGE_SIZE) == FLASH_IF_OK)
  {
    FLASH_IF_Write(LORAWAN_NVM_BASE_ADDRESS, (const void *)nvm, nvm_size);
  }
  /* USER CODE BEGIN OnStoreContextRequest_Last */

  /* USER CODE END OnStoreContextRequest_Last */
}

static void OnRestoreContextRequest(void *nvm, uint32_t nvm_size)
{
  /* USER CODE BEGIN OnRestoreContextRequest_1 */

  /* USER CODE END OnRestoreContextRequest_1 */
  FLASH_IF_Read(nvm, LORAWAN_NVM_BASE_ADDRESS, nvm_size);
  /* USER CODE BEGIN OnRestoreContextRequest_Last */

  /* USER CODE END OnRestoreContextRequest_Last */
}

