// Initial startup functions for GNSS, SD, display, radio, etc

void identifyBoard()
{
  productVariant = RTK_EXPRESS;
}


// Based on hardware features, determine if this is RTK Surveyor or RTK Express hardware
// Must be called after beginI2C (Wire.begin) so that we can do I2C tests
// Must be called after beginGNSS so the GNSS type is known
void beginBoard()
{

  pin_powerSenseAndControl = 13;
  pin_setupButton = 14;
  pin_microSD_CS = 25;
  pin_dac26 = 26;
  pin_powerFastOff = 27;
  pin_adc39 = 39;

  pinMode(pin_powerSenseAndControl, INPUT_PULLUP);
  pinMode(pin_powerFastOff, INPUT);

  if (esp_reset_reason() == ESP_RST_POWERON)
  {
      powerOnCheck(); // Only do check if we POR start
  }

  pinMode(pin_setupButton, INPUT_PULLUP);


  char versionString[21];
  
  systemPrintf("SparkFun RTK %s %s\r\n", platformPrefix, versionString);

  // Get unit MAC address
  esp_read_mac(wifiMACAddress, ESP_MAC_WIFI_STA);
  memcpy(btMACAddress, wifiMACAddress, sizeof(wifiMACAddress));
  btMACAddress[5] +=
      2; // Convert MAC address to Bluetooth MAC (add 2):
          // https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/system/system.html#mac-address
  memcpy(ethernetMACAddress, wifiMACAddress, sizeof(wifiMACAddress));
  ethernetMACAddress[5] += 3; // Convert MAC address to Ethernet MAC (add 3)

  // For all boards, check reset reason. If reset was due to wdt or panic, append last log
  
  if ((esp_reset_reason() == ESP_RST_POWERON) || (esp_reset_reason() == ESP_RST_SW))
  {
      reuseLastLog = false; // Start new log

      if (settings.enableResetDisplay == true)
      {
          settings.resetCount = 0;
          
      }
      settings.resetCount = 0;
  }
  else
  {
      reuseLastLog = true; // Attempt to reuse previous log

      if (settings.enableResetDisplay == true)
      {
          settings.resetCount++;
          systemPrintf("resetCount: %d\r\n", settings.resetCount);
          
      }

      systemPrint("Reset reason: ");
      switch (esp_reset_reason())
      {
      case ESP_RST_UNKNOWN:
          systemPrintln("ESP_RST_UNKNOWN");
          break;
      case ESP_RST_POWERON:
          systemPrintln("ESP_RST_POWERON");
          break;
      case ESP_RST_SW:
          systemPrintln("ESP_RST_SW");
          break;
      case ESP_RST_PANIC:
          systemPrintln("ESP_RST_PANIC");
          break;
      case ESP_RST_INT_WDT:
          systemPrintln("ESP_RST_INT_WDT");
          break;
      case ESP_RST_TASK_WDT:
          systemPrintln("ESP_RST_TASK_WDT");
          break;
      case ESP_RST_WDT:
          systemPrintln("ESP_RST_WDT");
          break;
      case ESP_RST_DEEPSLEEP:
          systemPrintln("ESP_RST_DEEPSLEEP");
          break;
      case ESP_RST_BROWNOUT:
          systemPrintln("ESP_RST_BROWNOUT");
          break;
      case ESP_RST_SDIO:
          systemPrintln("ESP_RST_SDIO");
          break;
      default:
          systemPrintln("Unknown");
      }
    }
}


// Attempt to de-init the SD card - SPI only
// https://github.com/greiman/SdFat/issues/351
void resetSPI()
{
    if (USE_SPI_MICROSD)
    {
        pinMode(pin_microSD_CS, OUTPUT);
        digitalWrite(pin_microSD_CS, HIGH); // De-select SD card

        // Flush SPI interface
        SPI.begin();
        SPI.beginTransaction(SPISettings(400000, MSBFIRST, SPI_MODE0));
        for (int x = 0; x < 10; x++)
            SPI.transfer(0XFF);
        SPI.endTransaction();
        SPI.end();

        digitalWrite(pin_microSD_CS, LOW); // Select SD card

        // Flush SD interface
        SPI.begin();
        SPI.beginTransaction(SPISettings(400000, MSBFIRST, SPI_MODE0));
        for (int x = 0; x < 10; x++)
            SPI.transfer(0XFF);
        SPI.endTransaction();
        SPI.end();

        digitalWrite(pin_microSD_CS, HIGH); // Deselet SD card
    }
}

// We want the UART2 interrupts to be pinned to core 0 to avoid competing with I2C interrupts
// We do not start the UART2 for GNSS->BT reception here because the interrupts would be pinned to core 1
// We instead start a task that runs on core 0, that then begins serial
// See issue: https://github.com/espressif/arduino-esp32/issues/3386
void beginUART2()
{
    ringBuffer = (uint8_t *)malloc(settings.gnssHandlerBufferSize);

    if (pinUART2TaskHandle == nullptr)
        xTaskCreatePinnedToCore(
            pinUART2Task,
            "UARTStart", // Just for humans
            2000,        // Stack Size
            nullptr,     // Task input parameter
            0,           // Priority, with 3 (configMAX_PRIORITIES - 1) being the highest, and 0 being the lowest
            &pinUART2TaskHandle,              // Task handle
            settings.gnssUartInterruptsCore); // Core where task should run, 0=core, 1=Arduino

    while (uart2pinned == false) // Wait for task to run once
        delay(1);
}

// Assign UART2 interrupts to the core that started the task. See:
// https://github.com/espressif/arduino-esp32/issues/3386
void pinUART2Task(void *pvParameters)
{
    // Note: ESP32 2.0.6 does some strange auto-bauding thing here which takes 20s to complete if there is no data for
    // it to auto-baud.
    //       That's fine for most RTK products, but causes the Ref Stn to stall for 20s. However, it doesn't stall with
    //       ESP32 2.0.2... Uncomment these lines to prevent the stall if/when we upgrade to ESP32 ~2.0.6.
    // #if defined(REF_STN_GNSS_DEBUG)
    //   if (ENABLE_DEVELOPER && productVariant == REFERENCE_STATION)
    // #else   // REF_STN_GNSS_DEBUG
    //   if (USE_I2C_GNSS)
    // #endif  // REF_STN_GNSS_DEBUG
    {
        serialGNSS.setRxBufferSize(
            settings.uartReceiveBufferSize); // TODO: work out if we can reduce or skip this when using SPI GNSS
        serialGNSS.setTimeout(settings.serialTimeoutGNSS); // Requires serial traffic on the UART pins for detection
        serialGNSS.begin(settings.dataPortBaud); // UART2 on pins 16/17 for SPP. The ZED-F9P will be configured to
                                                 // output NMEA over its UART1 at the same rate.

        // Reduce threshold value above which RX FIFO full interrupt is generated
        // Allows more time between when the UART interrupt occurs and when the FIFO buffer overruns
        // serialGNSS.setRxFIFOFull(50); //Available in >v2.0.5
        uart_set_rx_full_threshold(2, settings.serialGNSSRxFullThreshold); // uart_num, threshold
    }

    uart2pinned = true;

    vTaskDelete(nullptr); // Delete task once it has run once
}


// Connect to ZED module and identify particulars
void beginGNSS()
{
    // Skip if going into configure-via-ethernet mode
    if (configureViaEthernet)
    {
        log_d("configureViaEthernet: skipping beginGNSS");
        return;
    }

    // If we're using SPI, then increase the logging buffer
    if (USE_SPI_GNSS)
    {
        SPI.begin(); // Begin SPI here -   has not yet been called

        // setFileBufferSize must be called _before_ .begin
        // Use gnssHandlerBufferSize for now. TODO: work out if the SPI GNSS needs its own buffer size setting
        // Also used by Tasks.ino
        theGNSS.setFileBufferSize(settings.gnssHandlerBufferSize);
        theGNSS.setRTCMBufferSize(settings.gnssHandlerBufferSize);
    }

    if (USE_I2C_GNSS)
    {
        if (theGNSS.begin() == false)
        {
            log_d("GNSS Failed to begin. Trying again.");

            // Try again with power on delay
            delay(1000); // Wait for ZED-F9P to power up before it can respond to ACK
            if (theGNSS.begin() == false)
            {
                log_d("GNSS offline");
                
                return;
            }
        }
    }
    else
    {
        if (theGNSS.begin(SPI, pin_GNSS_CS) == false)
        {
            log_d("GNSS Failed to begin. Trying again.");

            // Try again with power on delay
            delay(1000); // Wait for ZED-F9P to power up before it can respond to ACK
            if (theGNSS.begin(SPI, pin_GNSS_CS) == false)
            {
                log_d("GNSS offline");
                return;
            }
        }

        if (theGNSS.getFileBufferSize() != settings.gnssHandlerBufferSize) // Need to call getFileBufferSize after begin
        {
            log_d("GNSS offline - no RAM for file buffer");
            return;
        }
        if (theGNSS.getRTCMBufferSize() != settings.gnssHandlerBufferSize) // Need to call getRTCMBufferSize after begin
        {
            log_d("GNSS offline - no RAM for RTCM buffer");
            return;
        }
    }

    // Increase transactions to reduce transfer time
    if (USE_I2C_GNSS)
        theGNSS.i2cTransactionSize = 128;

    // Auto-send Valset messages before the buffer is completely full
    theGNSS.autoSendCfgValsetAtSpaceRemaining(16);

    // Check the firmware version of the ZED-F9P. Based on Example21_ModuleInfo.
    if (theGNSS.getModuleInfo(1100) == true) // Try to get the module info
    {
        // Reconstruct the firmware version
        snprintf(zedFirmwareVersion, sizeof(zedFirmwareVersion), "%s %d.%02d", theGNSS.getFirmwareType(),
                 theGNSS.getFirmwareVersionHigh(), theGNSS.getFirmwareVersionLow());

        // Construct the firmware version as uint8_t. Note: will fail above 2.55!
        zedFirmwareVersionInt = (theGNSS.getFirmwareVersionHigh() * 100) + theGNSS.getFirmwareVersionLow();

        // Check this is known firmware
        //"1.20" - Mostly for F9R HPS 1.20, but also F9P HPG v1.20
        //"1.21" - F9R HPS v1.21
        //"1.30" - ZED-F9P (HPG) released Dec, 2021. Also ZED-F9R (HPS) released Sept, 2022
        //"1.32" - ZED-F9P released May, 2022

        const uint8_t knownFirmwareVersions[] = {100, 112, 113, 120, 121, 130, 132};
        bool knownFirmware = false;
        for (uint8_t i = 0; i < (sizeof(knownFirmwareVersions) / sizeof(uint8_t)); i++)
        {
            if (zedFirmwareVersionInt == knownFirmwareVersions[i])
                knownFirmware = true;
        }

        if (!knownFirmware)
        {
            systemPrintf("Unknown firmware version: %s\r\n", zedFirmwareVersion);
            zedFirmwareVersionInt = 99; // 0.99 invalid firmware version
        }

        // Determine if we have a ZED-F9P (Express/Facet) or an ZED-F9R (Express Plus/Facet Plus)
        if (strstr(theGNSS.getModuleName(), "ZED-F9P") != nullptr)
            zedModuleType = PLATFORM_F9P;
        else if (strstr(theGNSS.getModuleName(), "ZED-F9R") != nullptr)
            zedModuleType = PLATFORM_F9R;
        else
        {
            systemPrintf("Unknown ZED module: %s\r\n", theGNSS.getModuleName());
            zedModuleType = PLATFORM_F9P;
        }

        printZEDInfo(); // Print module type and firmware version
    }

    UBX_SEC_UNIQID_data_t chipID;
    if (theGNSS.getUniqueChipId(&chipID))
    {
        snprintf(zedUniqueId, sizeof(zedUniqueId), "%s", theGNSS.getUniqueChipIdStr(&chipID));
    }

    online.gnss = true;
}

// Configuration can take >1s so configure during splash
void configureGNSS()
{
    // Skip if going into configure-via-ethernet mode
    if (configureViaEthernet)
    {
        log_d("configureViaEthernet: skipping configureGNSS");
        return;
    }

    if (online.gnss == false)
        return;

    // Check if the ubxMessageRates or ubxMessageRatesBase need to be defaulted
    //checkMessageRates();

    theGNSS.setAutoPVTcallbackPtr(&storePVTdata); // Enable automatic NAV PVT messages with callback to storePVTdata
    theGNSS.setAutoHPPOSLLHcallbackPtr(
        &storeHPdata); // Enable automatic NAV HPPOSLLH messages with callback to storeHPdata
    theGNSS.setRTCM1005InputcallbackPtr(
        &storeRTCM1005data); // Configure a callback for RTCM 1005 - parsed from pushRawData
    theGNSS.setRTCM1006InputcallbackPtr(
        &storeRTCM1006data); // Configure a callback for RTCM 1006 - parsed from pushRawData

    if (HAS_GNSS_TP_INT)
        theGNSS.setAutoTIMTPcallbackPtr(
            &storeTIMTPdata); // Enable automatic TIM TP messages with callback to storeTIMTPdata

    if (HAS_ANTENNA_SHORT_OPEN)
    {
        theGNSS.newCfgValset();

        theGNSS.addCfgValset(UBLOX_CFG_HW_ANT_CFG_SHORTDET, 1); // Enable antenna short detection
        theGNSS.addCfgValset(UBLOX_CFG_HW_ANT_CFG_OPENDET, 1);  // Enable antenna open detection

        if (theGNSS.sendCfgValset())
        {
            theGNSS.setAutoMONHWcallbackPtr(
                &storeMONHWdata); // Enable automatic MON HW messages with callback to storeMONHWdata
        }
        else
        {
            systemPrintln("Failed to configure GNSS antenna detection");
        }
    }

    // Configuring the ZED can take more than 2000ms. We save configuration to
    // ZED so there is no need to update settings unless user has modified
    // the settings file or internal settings.
    if (settings.updateZEDSettings == false)
    {
        log_d("Skipping ZED configuration");
        return;
    }

    bool response = configureUbloxModule();
    if (response == false)
    {
        // Try once more
        systemPrintln("Failed to configure GNSS module. Trying again.");
        delay(1000);
        response = configureUbloxModule();

        if (response == false)
        {
            systemPrintln("Failed to configure GNSS module.");
            online.gnss = false;
            return;
        }
    }

    systemPrintln("GNSS configuration complete");
}

// Begin interrupts
void beginInterrupts()
{
    // Skip if going into configure-via-ethernet mode
    if (configureViaEthernet)
    {
        log_d("configureViaEthernet: skipping beginInterrupts");
        return;
    }

    if (HAS_GNSS_TP_INT) // If the GNSS Time Pulse is connected, use it as an interrupt to set the clock accurately
    {
        pinMode(pin_GNSS_TimePulse, INPUT);
        attachInterrupt(pin_GNSS_TimePulse, tpISR, RISING);
    }

}



// Configure the on board MAX17048 fuel gauge
void beginFuelGauge()
{
    if (HAS_NO_BATTERY)
        return; // Reference station does not have a battery

    // Set up the MAX17048 LiPo fuel gauge
    if (lipo.begin() == false)
    {
        systemPrintln("Fuel gauge not detected.");
        return;
    }

    online.battery = true;

    // Always use hibernate mode
    if (lipo.getHIBRTActThr() < 0xFF)
        lipo.setHIBRTActThr((uint8_t)0xFF);
    if (lipo.getHIBRTHibThr() < 0xFF)
        lipo.setHIBRTHibThr((uint8_t)0xFF);

    systemPrintln("Fuel gauge configuration complete");

    checkBatteryLevels(); // Force check so you see battery level immediately at power on

    // Check to see if we are dangerously low
    if (battLevel < 5 && battChangeRate < 0.5) // 5% and not charging
    {
        systemPrintln("Battery too low. Please charge. Shutting down...");



        delay(2000);

        powerDown(false); // Don't display 'Shutting Down'
    }
}

// Depending on platform and previous power down state, set system state
void beginSystemState()
{
    if (systemState > STATE_NOT_SET)
    {
        systemPrintln("Unknown state - factory reset");
        factoryReset(false); //We do not have the SD semaphore
    }

    if (settings.lastState == STATE_NOT_SET) // Default
        {
            systemState = STATE_ROVER_NOT_STARTED;
            settings.lastState = systemState;
        }

        systemState = settings.lastState; // Return to either Rover or Base Not Started. The last state previous to power down.
        
}

// Setup the timepulse output on the PPS pin for external triggering
// Setup TM2 time stamp input as need
bool beginExternalTriggers()
{
    // Skip if going into configure-via-ethernet mode
    if (configureViaEthernet)
    {
        log_d("configureViaEthernet: skipping beginExternalTriggers");
        return (false);
    }

    if (online.gnss == false)
        return (false);

    // If our settings haven't changed, trust ZED's settings
    if (settings.updateZEDSettings == false)
    {
        log_d("Skipping ZED Trigger configuration");
        return (true);
    }

    if (settings.dataPortChannel != MUX_PPS_EVENTTRIGGER)
        return (true); // No need to configure PPS if port is not selected

    bool response = true;

    response &= theGNSS.newCfgValset();
    response &= theGNSS.addCfgValset(UBLOX_CFG_TP_PULSE_DEF, 0);        // Time pulse definition is a period (in us)
    response &= theGNSS.addCfgValset(UBLOX_CFG_TP_PULSE_LENGTH_DEF, 1); // Define timepulse by length (not ratio)
    response &=
        theGNSS.addCfgValset(UBLOX_CFG_TP_USE_LOCKED_TP1,
                             1); // Use CFG-TP-PERIOD_LOCK_TP1 and CFG-TP-LEN_LOCK_TP1 as soon as GNSS time is valid
    response &= theGNSS.addCfgValset(UBLOX_CFG_TP_TP1_ENA, settings.enableExternalPulse); // Enable/disable timepulse
    response &=
        theGNSS.addCfgValset(UBLOX_CFG_TP_POL_TP1, settings.externalPulsePolarity); // 0 = falling, 1 = rising edge

    // While the module is _locking_ to GNSS time, turn off pulse
    response &= theGNSS.addCfgValset(UBLOX_CFG_TP_PERIOD_TP1, 1000000); // Set the period between pulses in us
    response &= theGNSS.addCfgValset(UBLOX_CFG_TP_LEN_TP1, 0);          // Set the pulse length in us

    // When the module is _locked_ to GNSS time, make it generate 1Hz (Default is 100ms high, 900ms low)
    response &= theGNSS.addCfgValset(UBLOX_CFG_TP_PERIOD_LOCK_TP1,
                                     settings.externalPulseTimeBetweenPulse_us); // Set the period between pulses is us
    response &=
        theGNSS.addCfgValset(UBLOX_CFG_TP_LEN_LOCK_TP1, settings.externalPulseLength_us); // Set the pulse length in us
    response &= theGNSS.sendCfgValset();

    if (response == false)
        systemPrintln("beginExternalTriggers config failed");

    if (settings.enableExternalHardwareEventLogging == true)
    {
        theGNSS.setAutoTIMTM2callbackPtr(
            &eventTriggerReceived); // Enable automatic TIM TM2 messages with callback to eventTriggerReceived
    }
    else
        theGNSS.setAutoTIMTM2callbackPtr(nullptr);

    return (response);
}

void beginIdleTasks()
{
    if (settings.enablePrintIdleTime == true)
    {
        char taskName[32];

        for (int index = 0; index < MAX_CPU_CORES; index++)
        {
            snprintf(taskName, sizeof(taskName), "IdleTask%d", index);
            if (idleTaskHandle[index] == nullptr)
                xTaskCreatePinnedToCore(
                    idleTask,
                    taskName, // Just for humans
                    2000,     // Stack Size
                    nullptr,  // Task input parameter
                    0,        // Priority, with 3 (configMAX_PRIORITIES - 1) being the highest, and 0 being the lowest
                    &idleTaskHandle[index], // Task handle
                    index);                 // Core where task should run, 0=core, 1=Arduino
        }
    }
}

void beginI2C()
{
    if (pinI2CTaskHandle == nullptr)
        xTaskCreatePinnedToCore(
            pinI2CTask,
            "I2CStart",        // Just for humans
            2000,              // Stack Size
            nullptr,           // Task input parameter
            0,                 // Priority, with 3 (configMAX_PRIORITIES - 1) being the highest, and 0 being the lowest
            &pinI2CTaskHandle, // Task handle
            settings.i2cInterruptsCore); // Core where task should run, 0=core, 1=Arduino

    while (i2cPinned == false) // Wait for task to run once
        delay(1);
}

// Assign I2C interrupts to the core that started the task. See: https://github.com/espressif/arduino-esp32/issues/3386
void pinI2CTask(void *pvParameters)
{
    Wire.begin(); // Start I2C on core the core that was chosen when the task was started
    // Wire.setClock(400000);

    // begin/end wire transmission to see if bus is responding correctly
    // All good: 0ms, response 2
    // SDA/SCL shorted: 1000ms timeout, response 5
    // SCL/VCC shorted: 14ms, response 5
    // SCL/GND shorted: 1000ms, response 5
    // SDA/VCC shorted: 1000ms, reponse 5
    // SDA/GND shorted: 14ms, response 5
    Wire.beginTransmission(0x15); // Dummy address
    int now = millis();
    int endValue = Wire.endTransmission();
    int i2cDelay = millis()-now;
    if (endValue == 2)
        online.i2c = true;
    else
    {
        systemPrint("Error: I2C Bus Not Responding. endValue = ");
        systemPrint(endValue);systemPrint(".....");
        systemPrint(i2cDelay);systemPrintln("ms");
    }

    i2cPinned = true;

    vTaskDelete(nullptr); // Delete task once it has run once
}

//~SWR added espnowaddpeer for the ALL meter remote.
//     peer1 is the MAC address defined as a global
void radioStart()
{
    espnowStart();

    if (esp_now_is_peer_exist(peer1) == true)
                log_d("Peer already exists");
    else
    {
        // Add new peer to system
        espnowAddPeer(peer1, false);

        // Record this MAC to peer list
        memcpy(settings.espnowPeers[settings.espnowPeerCount], peer1, 6);
        settings.espnowPeerCount++;
        settings.espnowPeerCount %= ESPNOW_MAX_PEERS;
        recordSystemSettings();
    }

    espnowSetState(ESPNOW_PAIRED);
}



// =-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-
// Time Pulse ISR
// Triggered by the rising edge of the time pulse signal, indicates the top-of-second.
// Set the ESP32 RTC to UTC

void tpISR()
{
    unsigned long millisNow = millis();
    if (!inMainMenu) // Skip this if the menu is open
    {
        if (online.rtc) // Only sync if the RTC has been set via PVT first
        {
            if (timTpUpdated) // Only sync if timTpUpdated is true
            {
                if (millisNow - lastRTCSync >
                    syncRTCInterval) // Only sync if it is more than syncRTCInterval since the last sync
                {
                    if (millisNow < (timTpArrivalMillis + 999)) // Only sync if the GNSS time is not stale
                    {
                        if (fullyResolved) // Only sync if GNSS time is fully resolved
                        {
                            if (tAcc < 5000) // Only sync if the tAcc is better than 5000ns
                            {
                                // To perform the time zone adjustment correctly, it's easiest if we convert the GNSS
                                // time and date into Unix epoch first and then apply the timeZone offset
                                uint32_t epochSecs = timTpEpoch;
                                uint32_t epochMicros = timTpMicros;
                                epochSecs += settings.timeZoneSeconds;
                                epochSecs += settings.timeZoneMinutes * 60;
                                epochSecs += settings.timeZoneHours * 60 * 60;

                                // Set the internal system time
                                rtc.setTime(epochSecs, epochMicros);

                                lastRTCSync = millis();
                                rtcSyncd = true;

                                gnssSyncTv.tv_sec = epochSecs; // Store the timeval of the sync
                                gnssSyncTv.tv_usec = epochMicros;

                                if (syncRTCInterval < 59000) // From now on, sync every minute
                                    syncRTCInterval = 59000;
                            }
                        }
                    }
                }
            }
        }
    }
}
