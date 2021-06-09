/* Hello World Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include <stdio.h>
#include <string>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_spi_flash.h"
#include "esp_task_wdt.h"
#include "soc/rtc_wdt.h"
#include "soc/timer_group_struct.h"
#include "soc/timer_group_reg.h"
#include "freertos/portmacro.h"
#include <soc/rtc.h>
#include "esp_event.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "driver/timer.h"
#include "esp32/rom/uart.h"
#include "wifi_manager.h"

#include <string.h>
#include <esp_wifi.h>
#include <esp_netif.h>
#include "esp_system.h"
#include "esp_log.h"

#include <inttypes.h>

#include "RestServer.h"
#include "IntexSWG.h"
#include "TM1650.h"
#include "utils.h"
#include "app_priv.h"


// DIO=18, CLK=19, Digits=2, ActivateDisplay=true, Intensity=3, DisplayMode=4x8
TM1650 module(dataDispPin, clockDispPin, 2, true, 3, TM1650_DISPMODE_4x8);

/* @brief tag used for ESP serial console messages */
static const char TAG[] = "main";

TaskHandle_t xHandle1;
TaskHandle_t xHandle2;
TaskHandle_t TaskA;

volatile bool displayON = true;
volatile uint8_t displayIntensity = 3;      // (range 0-7)
volatile uint8_t statusLedsIntensity = 3;   // (range 0-7)
volatile bool sendingKeyCode = false;
volatile bool keyCodeSetByAPI = false;
volatile uint8_t buttonStatus = 0x2E;
volatile uint8_t prevButtonStatus = 0x00;
volatile uint8_t dataReceived[2];
volatile uint8_t statusDigit1;
volatile uint8_t statusDigit2;
volatile uint8_t statusDigit3;
volatile uint8_t displayingDigit1;
volatile uint8_t displayingDigit2;
volatile uint8_t powerStatus;
volatile bool displayBlinking;
volatile bool removeWifiConfig;

volatile uint16_t virtualPressButtonTime = 250;
volatile uint32_t checkpoint=0;
volatile bool waitForWifiConfig = true;
volatile uint8_t dataReceivedBuffer[256];
int totalbytes = 0;


char getDisplayDigitFromCode(uint8_t code) {
    switch (code) {
        case DISP_BLANK: return 0;
        case DISP_0: return '0';
        case DISP_1: return '1';
        case DISP_2: return '2';
        case DISP_3: return '3';
        case DISP_4: return '4';
        case DISP_5: return '5';
        case DISP_6: return '6';
        case DISP_7: return '7';
        case DISP_8: return '8';
        case DISP_9: return '9';
        case DISP_DP: return '.';
        default: return 0;
    }
}

void feedTheDog(){
    // feed dog 0
    TIMERG0.wdt_wprotect=TIMG_WDT_WKEY_VALUE; // write enable
    TIMERG0.wdt_feed=1;                       // feed dog
    TIMERG0.wdt_wprotect=0;                   // write protect
    // feed dog 1
    TIMERG1.wdt_wprotect=TIMG_WDT_WKEY_VALUE; // write enable
    TIMERG1.wdt_feed=1;                       // feed dog
    TIMERG1.wdt_wprotect=0;                   // write protect
}

inline uint32_t IRAM_ATTR clocks()
{
    uint32_t ccount;
    asm volatile ( "rsr %0, ccount" : "=a" (ccount) );
    return ccount;
}

inline void delayClocks(uint32_t clks)
{
    uint32_t c = clocks();
    while( (clocks() - c ) < clks ){
        asm(" nop");
    }
}

/**
 * @brief Task in charge of ESP32<->SWG serial BUS
 */
void IRAM_ATTR Core1( void* p) {

    vTaskDelay(1000);
    
    portDISABLE_INTERRUPTS();
    register uint8_t sdaValue, sclValue, prevSda = 0, prevScl = 0, totalClocks = 0, totalBitsSent = 0;
    register bool receivingData = false,  bytePosition = false, dataOutput = false, sendingACK = false;
    uint8_t receivedByte = 0;  
    
    sdaValue = prevSda = GPIO_IN_Get(dataPin);
    prevScl = GPIO_IN_Get(clockPin);

    while(1) {
        if (!dataOutput && !sendingACK) {
            sdaValue = GPIO_IN_Get(dataPin);
        }
        sclValue = GPIO_IN_Get(clockPin);       

        // STOP Condition
        if (prevSda == LOW && sdaValue == HIGH && sclValue == HIGH) {
            receivingData = false;
            sendingKeyCode = false;
            totalClocks = 0;
            receivedByte = 0;
        }

        // Detect CLK RISING (0 -> 1)
        else if (prevScl == LOW && sclValue == HIGH) {
            // RECEIVING DATA
            if (receivingData) {                    
                if (totalClocks < 8) {
                    // Read SDA bit
                    receivedByte <<= 1;   // MSB first on TM1650, so shift left                            
                    if (sdaValue == HIGH) {
                        receivedByte |= 1;	// MSB first on TM1650, so set lowest bit
                    }                     
                    totalClocks++;
                }
                // Save/Process received byte
                else {                      
                    dataReceived[(int)bytePosition] = receivedByte;                                                        

                    // Check if we have received both bytes
                    if (bytePosition) {
                        switch (dataReceived[0])
                        {
                        case 0x68:
                            statusDigit1 = dataReceived[1];
                            break;
                        case 0x6A:
                            statusDigit2 = dataReceived[1];
                            break;
                        case 0x6C:
                            statusDigit3 = dataReceived[1];
                            break;                            
                        default:
                            break;
                        }
                    } 
                    else {
                        // Read Keyboard code received
                        if (dataReceived[0] == 0x4F) {
                            sendingKeyCode = true;
                            receivingData = false;
                            bytePosition = !bytePosition;
                        }
                    }               
                    receivedByte = 0;
                    totalClocks = 0;
                    bytePosition = !bytePosition;
                }
            }            
        }

        // Detect CLK FALLING (1 -> 0)
        else if (prevScl == HIGH && sclValue == LOW) {
            if (totalClocks == 8 && !sendingKeyCode) {
                // BEGIN SEND ACK
                digitalWrite(dataPin, LOW);
                pinMode(dataPin, GPIO_MODE_OUTPUT);
                sendingACK = true;
            }
            // END SEND ACK
            else if (sendingACK && totalClocks == 0) {
                if (sendingKeyCode == false) {
                    // Release SDA, Stop Send ACK 
                    delayMicroseconds(3);
                    pinMode(dataPin, GPIO_MODE_INPUT);
                    digitalWrite(dataPin, HIGH);
                    sendingACK = false;
                } else {
                    dataOutput = true;
                    receivingData = false;
                    sendingACK = false;
                }
            }

            // SEND KEY CODE (Master request to read keys with command 0x4F)
            if (sendingKeyCode) {                  
                if (totalBitsSent < 8) {                    
                    digitalWrite(dataPin, (buttonStatus << totalBitsSent) & 0x80 ? HIGH : LOW);
                    totalBitsSent++;
                }
                if (totalBitsSent >= 8) {
                    // Receive ACK
                    delayMicroseconds(12);
                    pinMode(dataPin, GPIO_MODE_INPUT);                        
                    digitalWrite(dataPin, HIGH);

                    dataOutput = false;
                    sendingKeyCode = false;
                    receivingData = true;
                    totalBitsSent = 0;
                }
            }
        }

        // START Condition
        else if (prevSda == HIGH && sdaValue == LOW && sclValue == HIGH) {
            receivingData = true;            
        }

        // Save previous SDA and SCL values
        if (dataOutput == false) {
            prevSda = sdaValue;            
        }        
        prevScl = sclValue;        
        
        feedTheDog();   
    }
    //vTaskDelete(NULL);
}

/**
 * @brief this is an exemple of a callback that you can setup in your own app to get notified of wifi manager event.
 */
void cb_connection_ok(void *pvParameter){
	ip_event_got_ip_t* param = (ip_event_got_ip_t*)pvParameter;

	/* transform IP to human readable string */
	char str_ip[16];
	esp_ip4addr_ntoa(&param->ip_info.ip, str_ip, IP4ADDR_STRLEN_MAX);

	ESP_LOGI(TAG, "I have a connection and my IP is %s!", str_ip);

    start_rest_server(8080);

    //cloud_start();
    
}


/**
 * @brief Task that shows a countdown en restarts ESP32 when wifi configuration has been saved successfully
 */
void reset_esp(void *pvParameter){
    waitForWifiConfig = false;
    statusDigit2 = DISP_0;
    for (int i = 3; i >= 0; i--) {
        printf("Restarting in %d seconds...\n", i);
        switch (i)
        {
        case 3:
            statusDigit1 = DISP_3;
            break;
        case 2:
            statusDigit1 = DISP_2;
            break;
        case 1:
            statusDigit1 = DISP_1;
            break;
        case 0:
            statusDigit1 = DISP_0;
            break;        
        default:
            break;
        }        
        vTaskDelay(1000 / portTICK_PERIOD_MS);
        feedTheDog();
    }
    printf("Restarting now.\n");
    fflush(stdout);
    esp_restart();
}


/**
 * @brief Task in charge of ESP32<->Display serial BUS
 */
void RTOS_1(void *p) {

    vTaskDelay(1000);
    //uint32_t prevCheckpoint = 0;

    while(1) {
        if (!keyCodeSetByAPI) buttonStatus = module.getButtonPressedCode();
        module.setupDisplay(displayON, displayIntensity);
        module.setSegments(statusDigit1, (DIGIT1 & 0b111) >> 1);
        
        vTaskDelay(10);
        
        if (!keyCodeSetByAPI) buttonStatus = module.getButtonPressedCode();
        module.setupDisplay(displayON, displayIntensity);
        module.setSegments(statusDigit2, (DIGIT2 & 0b111) >> 1);
        
        vTaskDelay(10);
        
        if (!keyCodeSetByAPI) buttonStatus = module.getButtonPressedCode();
        module.setupDisplay(displayON, statusLedsIntensity);
        module.setSegments(statusDigit3, (DIGIT3 & 0b111) >> 1);

        taskYIELD();
/*
        if (prevCheckpoint != checkpoint) {
            prevCheckpoint = checkpoint;
            printf("Checkpoint: %d\n", checkpoint);
        }
*/

 //       printf("DIG1: 0x%02X - DIG2:0x%02X - DIG3: 0x%02X\n", statusDigit1, statusDigit2, statusDigit3);
/*        
        if (totalbytes >= 100) {
            totalbytes = 0;
            for (int i=0 ; i<100 ; i++) {
                printf("0x%02X\n", dataReceivedBuffer[i]);
            }
        }
*/
        //printf("key: 0x%02X\n", buttonStatus);
        vTaskDelay(10);
    }
}

/**
 * @brief Task that controls virtual key press (API), power status info and display information to be retrieved by API
 */
void RTOS_2(void *p) {

    vTaskDelay(1000);
    
    unsigned long time1_keycode_api = 0;
    unsigned long time2_keycode_api = 0;
    unsigned long time1_power = 0;
    unsigned long time2_power = 0;
    unsigned long time1_displ = 0;
    unsigned long time2_displ = 0;
    bool statusPowerLed = false;
    bool prevStatusPowerLed = false;
    bool statusDisplayLed = false;
    bool prevStatusDisplayLed = false;
    uint8_t powerBlinks = 0;
    uint8_t displayBlinks = 0;
    time1_power = time2_power = time1_displ = time2_displ = millis();
    powerBlinks = 0;

    while(1) { 
        if (removeWifiConfig) {
            removeWifiConfig = false;
            wifi_manager_clear_wifi_configuration();
        }
        
        // Keep API keycode for 250ms (Virtual Press Button) **************************************
        if (keyCodeSetByAPI) {
            // Start counting time
            if (time1_keycode_api == 0 && time2_keycode_api == 0) {
                time1_keycode_api = time2_keycode_api = millis();
            }
            if (time2_keycode_api - time1_keycode_api > virtualPressButtonTime) {
                keyCodeSetByAPI = false;
                time1_keycode_api = time2_keycode_api = 0;
            } 
            else {
                time2_keycode_api = millis();
            }
        }
        // Keep API keycode for 250ms (Virtual Press Button) **************************************

        // Check and update Power status ***********************************************************
        if (time2_power - time1_power < 1500) {        
            statusPowerLed = (statusDigit3 & (0x01 << LED_POWER)) >> LED_POWER == 1 ? true : false;
            if (prevStatusPowerLed != statusPowerLed) {
                powerBlinks++;
                prevStatusPowerLed = statusPowerLed;
            }
            time2_power = millis();
        }
        else {
            powerStatus = (powerBlinks > 1) ? 2 : (powerBlinks == 1) ? 1 : 0;
            powerBlinks = 0;
            time1_power = time2_power = millis();
        }
        // Check and update Power status ***********************************************************

        // Check and update Display status ***********************************************************
        displayingDigit1 = (statusDigit2 > 0x00) ? statusDigit1 : displayingDigit1;
        displayingDigit2 = (statusDigit2 > 0x00) ? statusDigit2 : displayingDigit2;
        if (time2_displ - time1_displ < 600) {
            statusDisplayLed = (statusDigit2 > 0x00) ? true : false;
            if (prevStatusDisplayLed != statusDisplayLed) {
                displayBlinks++;
                prevStatusDisplayLed = statusDisplayLed;
            }
            time2_displ = millis();
        }
        else {
            displayBlinking = (displayBlinks > 0) ? true : false;
            displayBlinks = 0;
            time1_displ = time2_displ = millis();
        }
        // Check and update Display status ***********************************************************
        
        taskYIELD();
    }
}


/**
 * @brief Show blinking "AP" in the display while waiting for wifi configuration
 */
void ConfigureWifi(void *p) {
    vTaskDelay(1000);
    int i = 0;
    while(waitForWifiConfig) {
        //statusDigit2 = (i % 2 == 0) ? 0x02 : 00;
        statusDigit1 = (i % 2 == 0) ? DISP_P : DISP_BLANK;
        statusDigit2 = (i % 2 == 0) ? DISP_A : DISP_BLANK;
        //statusDigit3 = 1 << i;
        i = (i < 7) ? i + 1 : 0;
        delayMicroseconds(300000);
        feedTheDog();
        taskYIELD();
    } 
    vTaskDelete( NULL );
}


void startCore0a(void) {
    xTaskCreatePinnedToCore(
        RTOS_1,                 // Function that implements the task.
        "RTOS-1",               // Text name for the task.
        STACK_SIZE,             // Stack size in bytes, not words.
        ( void * ) 1,           // Parameter passed into the task.
        tskIDLE_PRIORITY + 5,   // Priority
        &xHandle1,              // Variable to hold the task's data structure.
        0);
}

void startCore0b(void) {
    xTaskCreatePinnedToCore(
        RTOS_2,                 // Function that implements the task.
        "RTOS-2",               // Text name for the task.
        STACK_SIZE,             // Stack size in bytes, not words.
        ( void * ) 1,           // Parameter passed into the task.
        tskIDLE_PRIORITY + 3,   // Priority
        &xHandle2,              // Variable to hold the task's data structure.
        0);
}

// Function that creates the superloop Core1 to be pinned at Core 1
void startCore1( void )
{
    xTaskCreatePinnedToCore(
        Core1,                  // Function that implements the task.
        "Core1",                // Text name for the task.
        10000,             // Stack size in bytes, not words.
        ( void * ) 1,           // Parameter passed into the task.
        tskIDLE_PRIORITY + 5,   // Priority
        &TaskA,                 // Task
        1);                     // Core 1
}

// Function that creates the superloop Core1 to be pinned at Core 1
void configureWifiTask( void )
{
    xTaskCreate(
        ConfigureWifi,                  // Function that implements the task.
        "ConfigureWifi",                // Text name for the task.
        STACK_SIZE,             // Stack size in bytes, not words.
        ( void * ) 1,           // Parameter passed into the task.
        tskIDLE_PRIORITY + 2,   // Priority
        &TaskA);                 // Task
}


extern "C" void app_main(void)
{
   //printf("Hello world!\n");

    /* Print chip information */
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    printf("This is %s chip with %d CPU cores, WiFi%s%s, ",
            CONFIG_IDF_TARGET,
            chip_info.cores,
            (chip_info.features & CHIP_FEATURE_BT) ? "/BT" : "",
            (chip_info.features & CHIP_FEATURE_BLE) ? "/BLE" : "");

    printf("silicon revision %d, ", chip_info.revision);

    printf("%dMB %s flash\n", spi_flash_get_chip_size() / (1024 * 1024),
            (chip_info.features & CHIP_FEATURE_EMB_FLASH) ? "embedded" : "external");

    printf("Free heap: %d\n", esp_get_free_heap_size());

/*
    uint64_t time1=0;
    uint64_t time2=0;
    time1 = millis();
    vTaskDelay(1000);
    time2 = millis();
    printf("Delta time %" PRId64 "\n", (time2-time1));
*/

/*
    uint64_t time1=0;
    uint64_t time2=0;
    time1 = millis();
    //delayClocks(1000000000);
    //delayClocks(239980801);
    delayClocks(CLOCKS_4_us);
    time2 = millis();
    printf("Delta time %" PRId64 "\n", (time2-time1));
*/

/*
    for (int i = 10; i >= 0; i--) {
        printf("Restarting in %d seconds...\n", i);
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
    printf("Restarting now.\n");
    fflush(stdout);
    esp_restart();
*/

    uint32_t clocks1=0;
    uint32_t clocks2=0;
    uint32_t clocks3=0;
    pinMode(dataPin, GPIO_MODE_INPUT);
    clocks1 = clocks();
    pinMode(dataPin, GPIO_MODE_OUTPUT);
    clocks2 = clocks();
    pinMode(dataPin, GPIO_MODE_INPUT);
    clocks3 = clocks();
    printf("IN->OUT: %d clocks - OUT->IN: %d clocks\n", (clocks2-clocks1), (clocks3-clocks2));


    pinMode(dataPin, GPIO_MODE_INPUT);
    GPIO_Set(dataPin);

    pinMode(clockPin, GPIO_MODE_INPUT);
    GPIO_Set(clockPin);

    pinMode(dataDispPin, GPIO_MODE_OUTPUT);
    pinMode(clockDispPin, GPIO_MODE_OUTPUT); 

    //module.setupDisplay(true, 5);   // on=true, intensity-2 (range 0-7)

    /* start the wifi manager */
	wifi_manager_start();

    //wifi_manager_clear_wifi_configuration();

	/* register a callback as an example to how you can integrate your code with the wifi manager */
	wifi_manager_set_callback(WM_EVENT_STA_GOT_IP, &cb_connection_ok);    

	/* your code should go here. Here we simply create a task on core 2 that monitors free heap memory */
	//xTaskCreatePinnedToCore(&monitoring_task, "monitoring_task", 2048, NULL, 1, NULL, 0);
    /*
    while (!wifi_manager_fetch_wifi_sta_config())
    {
        delayMicroseconds(10000000);
    }
    */

    startCore0a();
    startCore0b();
   
   if (wifi_manager_fetch_wifi_sta_config())
    {
        startCore1();        
    }
    else {
        wifi_manager_set_callback(WM_EVENT_WIFI_CONFIG_SAVED, &reset_esp);
        configureWifiTask();
    }
}
