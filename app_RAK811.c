// Fixa increment på join ifall det finns fel

#include "rui.h"
#include "board.h"

/* User Code */

/*!
 * User sleep timer related variables.
 */
TimerEvent_t user_sleep_timer;     // Timer instance.
uint32_t join_interval = 10;       // If the join fails, the retry interval, in seconds.
uint32_t send_interval = 86400000; // Data sending interval time, in milliseconds.
bool sleep_enable = false;         // Sleep enable flag.

/*!
 * loop function enable flag.
 */
bool loop_enable = true;

/*!
 * LoRaWAN data
 */
uint8_t AppData[32] = {0};

/*!
 * GPIO variables
 */
RUI_GPIO_ST gpio_2_input;
RUI_GPIO_ST gpio_3_input;

extern uint8_t process_cli;

void lora_join_callback(uint32_t status)
{
    process_cli = RUI_UART1; // Internal variable, select uart1 to print information.
    loop_enable = true;

    if (status)
        rui_timer_setvalue(&user_sleep_timer, send_interval);
    else
        sleep_enable = true; // If join fails, go to sleep.
}

void lora_send_callback(uint32_t status)
{
    process_cli = RUI_UART1; // Internal variable, select uart1 to print information.
    loop_enable = true;
    sleep_enable = true; // After the data is sent, go to sleep.
}

void user_downlink_process(uint8_t *pdata, uint8_t len)
{

    // Write data to flash
    RUI_RETURN_STATUS ret_code;

    ret_code = rui_flash_write(RUI_FLASH_USER, pdata, len);
    if (ret_code != RUI_STATUS_OK)
        RUI_LOG_PRINTF("Flash write error! %d\r\n", ret_code);

    if (pdata[0] > 0 && pdata[0] < 256)
        send_interval = pdata[0] * 3600000;
    else
        send_interval = 60000; // if 00 is in downlink, set send interval to every minute for debugging

    RUI_LOG_PRINTF("Wrote send interval %d hours to flash\r\n", send_interval / 3600000);
    rui_timer_setvalue(&user_sleep_timer, send_interval);
}

void user_sleep_timer_callback(void)
{
    loop_enable = true;
}

void gpio_2_interrupt_callback(void)
{
    AppData[0] = 0;
    loop_enable = true;
}

void gpio_3_interrupt_callback(void)
{
    AppData[0] = 1;
    loop_enable = true;
}

void setup(void)
{
    // Read send_interval from flash
    uint8_t flash_buf[0] = {0};
    uint16_t flash_len = 1;

    RUI_RETURN_STATUS ret_code;
    ret_code = rui_flash_read(RUI_FLASH_USER, flash_buf, flash_len);
    if (ret_code != RUI_STATUS_OK)
        RUI_LOG_PRINTF("Flash read error! %d\r\n", ret_code);
    else
    {
        send_interval = flash_buf[0] * 3600000;
        RUI_LOG_PRINTF("Send interval every %d hours\r\n", send_interval / 3600000);
        rui_timer_setvalue(&user_sleep_timer, send_interval);
    }

    // Init timer
    rui_timer_init(&user_sleep_timer, user_sleep_timer_callback);
    rui_timer_setvalue(&user_sleep_timer, join_interval * 1000);

    /* The available pins of the RAK811 module are:
     * pin 2, pin 3, pin 4, pin 5, pin 8, pin 9, pin 14,
     * pin 15, pin 16, pin 18, pin 19, pin 20, pin 22, pin 23.
     */
    gpio_2_input.pin_num = 2; // Pin 2 of the RAK811 module, PB12 pin of the MCU.
    gpio_2_input.dir = RUI_GPIO_PIN_DIR_INPUT;
    gpio_2_input.pull = RUI_GPIO_PIN_NOPULL; // External pullup instead of internal due to software bug with the interrupt

    gpio_3_input.pin_num = 3; // Pin 3 of the RAK811 module, PB13 pin of the MCU.
    gpio_3_input.dir = RUI_GPIO_PIN_DIR_INPUT;
    gpio_3_input.pull = RUI_GPIO_PIN_NOPULL; // External pullup instead of internal due to software bug with the interrupt

    ret_code = rui_gpio_interrupt(true, gpio_2_input, RUI_GPIO_EDGE_FALL, RUI_GPIO_IRQ_NORMAL_PRIORITY, gpio_2_interrupt_callback);
    if (ret_code != RUI_STATUS_OK)
        RUI_LOG_PRINTF("GPIO interrupt init error! %d\r\n", ret_code);

    ret_code = rui_gpio_interrupt(true, gpio_3_input, RUI_GPIO_EDGE_FALL, RUI_GPIO_IRQ_NORMAL_PRIORITY, gpio_3_interrupt_callback);
    if (ret_code != RUI_STATUS_OK)
        RUI_LOG_PRINTF("GPIO interrupt init error! %d\r\n", ret_code);
}

void loop(void)
{
    RUI_RETURN_STATUS ret_code;
    RUI_LORA_STATUS_T user_lora_status; // record status

    if (loop_enable != true)
        return;

    loop_enable = false;

    if (sleep_enable)
    {
        /* Device go to sleep */
        rui_timer_start(&user_sleep_timer);
        rui_device_sleep(1); // go to sleep
        sleep_enable = false;
    }
    else
    {
        rui_lora_get_status(false, &user_lora_status);
        if (user_lora_status.IsJoined == false)
        {
            /* Join the network */
            RUI_LOG_PRINTF("Start join...\r\n");
            ret_code = rui_lora_join();
            if (ret_code != RUI_STATUS_OK)
            {
                RUI_LOG_PRINTF("Error code is 0x%X\r\n", ret_code);

                join_interval = join_interval * 2;
                if (join_interval > 21600)
                {
                    join_interval = 10;
                }
                RUI_LOG_PRINTF("Rejoining in %d seconds.\r\n", join_interval);

                rui_timer_setvalue(&user_sleep_timer, join_interval * 1000);
                rui_timer_start(&user_sleep_timer); // If join fails, start the timer and wait for the next join.
            }
            else
            {
                if (user_lora_status.join_mode == RUI_OTAA)
                    LoRaWANJoined_callback(1);
            }
        }
        else
        {
            /* Send LoRa data */
            uint8_t AppPort = 2;
            uint8_t AppLen = 1;

            RUI_LOG_PRINTF("Send data\r\n");
            ret_code = rui_lora_send(AppPort, AppData, AppLen);
            if (ret_code != RUI_STATUS_OK)
            {
                rui_timer_start(&user_sleep_timer); // If send fails, start the timer and wait for the next send.
            }
            else
            {
                RUI_LOG_PRINTF("Sending again in %d hours.\r\n", send_interval / 3600000);
            }
        }
    }
}

/* End User Code */

/*******************************************************************************************
 * LoRaMac callback functions
 * * void LoRaReceive_callback(RUI_RECEIVE_T* Receive_datapackage);//LoRaWAN callback if receive data
 * * void LoRaP2PReceive_callback(RUI_LORAP2P_RECEIVE_T *Receive_P2Pdatapackage);//LoRaP2P callback if receive data
 * * void LoRaWANJoined_callback(uint32_t status);//LoRaWAN callback after join server request
 * * void LoRaWANSendsucceed_callback(RUI_MCPS_T status);//LoRaWAN call back after send data complete
 * *****************************************************************************************/
void LoRaReceive_callback(RUI_RECEIVE_T *Receive_datapackage)
{
    char hex_str[3] = {0};
    RUI_LOG_PRINTF("at+recv=%d,%d,%d,%d", Receive_datapackage->Port, Receive_datapackage->Rssi, Receive_datapackage->Snr, Receive_datapackage->BufferSize);

    if ((Receive_datapackage->Buffer != NULL) && Receive_datapackage->BufferSize)
    {
        RUI_LOG_PRINTF(":");
        for (int i = 0; i < Receive_datapackage->BufferSize; i++)
        {
            sprintf(hex_str, "%02x", Receive_datapackage->Buffer[i]);
            RUI_LOG_PRINTF("%s", hex_str);
        }
    }
    RUI_LOG_PRINTF("\r\n");

    /* User Code */
    if (Receive_datapackage->Port == 1)
    {
        user_downlink_process(Receive_datapackage->Buffer, Receive_datapackage->BufferSize);
    }
    /* End User Code */
}

void LoRaP2PReceive_callback(RUI_LORAP2P_RECEIVE_T *Receive_P2Pdatapackage)
{
    char hex_str[3] = {0};
    RUI_LOG_PRINTF("at+recv=%d,%d,%d:", Receive_P2Pdatapackage->Rssi, Receive_P2Pdatapackage->Snr, Receive_P2Pdatapackage->BufferSize);
    for (int i = 0; i < Receive_P2Pdatapackage->BufferSize; i++)
    {
        sprintf(hex_str, "%02X", Receive_P2Pdatapackage->Buffer[i]);
        RUI_LOG_PRINTF("%s", hex_str);
    }
    RUI_LOG_PRINTF("\r\n");
}

void LoRaWANJoined_callback(uint32_t status)
{
    /* User Code */
    lora_join_callback(status);
    /* End User Code */

    if (status) // Join Success
    {
        RUI_LOG_PRINTF("OK Join Success\r\n");
    }
    else
    {
        RUI_LOG_PRINTF("ERROR: %d\r\n", RUI_AT_LORA_INFO_STATUS_JOIN_FAIL);
    }
}

void LoRaWANSendsucceed_callback(RUI_MCPS_T mcps_type, RUI_RETURN_STATUS status)
{
    /* User Code */
    lora_send_callback(status);
    /* End User Code */

    if (status == RUI_STATUS_OK)
    {
        switch (mcps_type)
        {
        case RUI_MCPS_UNCONFIRMED:
        {
            RUI_LOG_PRINTF("OK \r\n");
            break;
        }
        case RUI_MCPS_CONFIRMED:
        {
            RUI_LOG_PRINTF("OK \r\n");
            break;
        }
        case RUI_MCPS_MULTICAST:
        {
            RUI_LOG_PRINTF("OK \r\n");
            break;
        }
        case RUI_MCPS_PROPRIETARY:
        {
            RUI_LOG_PRINTF("OK \r\n");
            break;
        }
        default:
            break;
        }
    }
    else if (status != RUI_AT_LORA_INFO_STATUS_ADDRESS_FAIL)
        RUI_LOG_PRINTF("ERROR: %d\r\n", status);
}

void LoRaP2PSendsucceed_callback(void)
{
    RUI_LOG_PRINTF("OK \r\n");
}

/*******************************************************************************************
 * The RUI is used to receive data from uart.
 *
 * *****************************************************************************************/
void rui_uart_recv(RUI_UART_DEF uart_def, uint8_t *pdata, uint16_t len)
{

    switch (uart_def)
    {
    case RUI_UART1: // process code if RUI_UART1 work at RUI_UART_UNVARNISHED

        break;
    case RUI_UART3: // process code if RUI_UART3 received data ,the len is always 1
        /*****************************************************************************
         * user code
         ******************************************************************************/
    //    RUI_LOG_PRINTF("%c",*pdata);
    default:
        break;
    }
}

/*******************************************************************************************
 * sleep and wakeup callback
 *
 * *****************************************************************************************/
void bsp_sleep(void)
{
    /*****************************************************************************
     * user process code before enter sleep
     ******************************************************************************/
}
void bsp_wakeup(void)
{
    /*****************************************************************************
     * user process code after exit sleep
     ******************************************************************************/
}

/*******************************************************************************************
 * the app_main function
 * *****************************************************************************************/
void main(void)
{
    RUI_LORA_STATUS_T app_lora_status;

    rui_init();

    /*******************************************************************************************
     * Register LoRaMac callback function
     *
     * *****************************************************************************************/
    rui_lora_register_recv_callback(LoRaReceive_callback);
    rui_lorap2p_register_recv_callback(LoRaP2PReceive_callback);
    rui_lorajoin_register_callback(LoRaWANJoined_callback);
    rui_lorasend_complete_register_callback(LoRaWANSendsucceed_callback);
    rui_lorap2p_complete_register_callback(LoRaP2PSendsucceed_callback);

    /*******************************************************************************************
     * Register Sleep and Wakeup callback function
     *
     * *****************************************************************************************/
    rui_sensor_register_callback(bsp_wakeup, bsp_sleep);

    /*******************************************************************************************
     * Obtain status information and print.
     *
     * *****************************************************************************************/
    rui_lora_get_status(false, &app_lora_status);
    switch (app_lora_status.work_mode)
    {
    case RUI_LORAWAN:
        RUI_LOG_PRINTF("LoRa work mode:LoRaWAN,");
        if (app_lora_status.join_mode == RUI_OTAA)
        {
            RUI_LOG_PRINTF(" join_mode:OTAA,");
            if (app_lora_status.MulticastEnable)
            {
                RUI_LOG_PRINTF(" MulticastEnable:true.\r\n");
            }
            else
            {
                RUI_LOG_PRINTF(" MulticastEnable: false,");
                switch (app_lora_status.class_status)
                {
                case RUI_CLASS_A:
                    RUI_LOG_PRINTF(" Class: A\r\n");
                    break;
                case RUI_CLASS_B:
                    RUI_LOG_PRINTF(" Class: B\r\n");
                    break;
                case RUI_CLASS_C:
                    RUI_LOG_PRINTF(" Class: C\r\n");
                    break;
                default:
                    break;
                }
            }
        }
        else if (app_lora_status.join_mode == RUI_ABP)
        {
            RUI_LOG_PRINTF(" join_mode:ABP,");
            if (app_lora_status.MulticastEnable)
            {
                RUI_LOG_PRINTF(" MulticastEnable:true.\r\n");
            }
            else
            {
                RUI_LOG_PRINTF(" MulticastEnable: false,");
                switch (app_lora_status.class_status)
                {
                case RUI_CLASS_A:
                    RUI_LOG_PRINTF(" Class: A\r\n");
                    break;
                case RUI_CLASS_B:
                    RUI_LOG_PRINTF(" Class: B\r\n");
                    break;
                case RUI_CLASS_C:
                    RUI_LOG_PRINTF(" Class: C\r\n");
                    break;
                default:
                    break;
                }
            }
        }
        break;
    default:
        break;
    }
    RUI_LOG_PRINTF("Initialization OK \r\n");

    /* User Code */
    setup();
    /* End User Code */

    while (1)
    {
        rui_running();

        /* User Code */
        loop();
        /* End User Code */
    }
}
