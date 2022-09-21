/* Program: ESP32 FUOTA
 * Author: Pedro Bertoleti
 */
#include <WiFi.h>
#include <Update.h>
#include <esp_task_wdt.h>

/* Definition - versão de firmware */
#define FIRMWARE_VERSION         "V1.0"

/* Definitions - wifi */
#define WIFI_SSID               " "  /* Define here your wifi SSID */
#define WIFI_PASS               " "  /* Define here your wifi Password */

/* Definitions - FUOTA */
#define FUOTA_SERVER_PORT        80
#define FUOTA_SERVER_TIMEOUT     10000 //ms

/* Definition - FUOTA push-button */
#define FUOTA_PUSH_BUTTON_GPIO           22
#define FUOTA_PUSH_BUTTON_DEBOUNCE_TIME  50 //ms

/* Global objects and variables */
WiFiClient espClient;
String url_FUOTA = "";
String host_http = " ";          /* Insert here remote server address without http:// (example: www.example.com) */
String firmware_filename = " ";  /* Insert here firmware filename considering its path in server (example: /firmware.bin) */
unsigned long timestamp_since_firmware_start = 0;
bool FUOTA_must_start = false;

/* Protótipos */
void init_wifi(void);
void connect_to_wifi_network(void);
void verify_wifi_connection(void);
String search_for_string_in_http_header(String header_http, String string_to_search);
void init_FUOTA(String host_http, String firware_filename);
unsigned long calculate_time_difference(unsigned long t_ref);

/* Function: init wi-fi connection
   Parameters: nothing
   Return: nothing
*/
void init_wifi(void)
{    
    Serial.println("Trying to connect to the following wi-fi network: ");
    Serial.println(WIFI_SSID);    
    connect_to_wifi_network();
}

/* Function: connect to a wi-fi network
   Parameters: nothing
   Return: nothing
*/
void connect_to_wifi_network(void)
{
    /* Check if ESP32 is already connected to a wi-fi network */
    if (WiFi.status() == WL_CONNECTED)
        return;
    
    WiFi.begin(WIFI_SSID, WIFI_PASS); 
    
    while (WiFi.status() != WL_CONNECTED)
    {
        Serial.print(".");
        delay(100);    
    }

    Serial.println("Successfully connected to the following wi-fi network: ");
    Serial.println(WIFI_SSID); 
}

/* Function: check if ESP32 is connected to a wi-fi network.
             In negative case, ESP32 will try to connect to wi-fi network
   Parameters: nothing
   Return: nothing
*/
void verify_wifi_connection(void)
{
    connect_to_wifi_network();
}

/* Function: search for a string in HTTP header.If it's found, return
             the substring
   Parameters: - Full HTTP header
               - String to search
   Return: substring
*/
String search_for_string_in_http_header(String header_http, String string_to_search) 
{
    return header_http.substring(strlen(string_to_search.c_str()));
}

/* Function: make the FUOTA process
   Parameters: - Server address
               - Firmware filename
   Return: nothing
*/
void init_FUOTA(String host_http, String firware_filename) 
{
    unsigned long timeout_fw_FUOTA = 0;
    String http_line = "";
    String contentType_http = "";
    bool is_firmware_valid = false;
    size_t FUOTA_firmware_size = 0;
    bool FUOTA_can_start;
    size_t bytes_written_to_flash = 0;
    char verbose_debug_line[150] = {0};
    unsigned long timestamp_FUOTA_init = 0;

    timestamp_FUOTA_init = millis();
    Serial.println("Connecting to remote server...");
    
    if (espClient.connect(host_http.c_str(), FUOTA_SERVER_PORT)) 
    {
        Serial.println("Successfully connected to remote server. Downloading firmware...");
     
        /* Obtain information of firmware to be downloaded (via HTTP GET request)
           and download it */   
        espClient.print(String("GET ") + firware_filename + " HTTP/1.1\r\n" +
                       "Host: " + host_http + "\r\n" +
                       "Cache-Control: no-cache\r\n" +
                       "Connection: close\r\n\r\n");

        /* Wait for FUOTA remote server response. Timeout is defined in FUOTA_SERVER_TIMEOUT */
        timeout_fw_FUOTA = millis();
        while (espClient.available() == 0) 
        {
            if ( calculate_time_difference(timeout_fw_FUOTA) > FUOTA_SERVER_TIMEOUT ) 
            {
                Serial.println("[ERROR] Timeout: no response received from FUOTA remote server. FUOTA is cancelled");
                espClient.stop();
                Serial.println("Rebooting ESP32 due to FUOTA error");
                ESP.restart();
            }

        }

        /* FUOTA remote server response has been received.
           FUOTA process start
        */
        while (espClient.available()) 
        {
            http_line = espClient.readStringUntil('\n');
            http_line.trim();

            if (http_line.length() == 0) 
            {
                break;
            }

            if (http_line.startsWith("HTTP/1.1")) 
            {
                if (http_line.indexOf("200") < 0) 
                {
                    Serial.println("[ERROR] FUOTA remote server response contains an error. FUOTA is cancelled.");
                    Serial.print("HTTP request response: ");
                    Serial.println(http_line);
                    Serial.println("Rebooting ESP32 due to FUOTA error");
                    ESP.restart();
                    return;
                }
            }

            /* Parse firmware size from HTTP header received from remote server */
            if (http_line.startsWith("Content-Length: ")) 
            {
                FUOTA_firmware_size = (size_t)atoi((search_for_string_in_http_header(http_line, "Content-Length: ")).c_str());
                sprintf(verbose_debug_line, "Firmware size: %d bytes", FUOTA_firmware_size);
                Serial.println(verbose_debug_line);
            }

            /* Parse data type from HTTP header received from remote server */
            if (http_line.startsWith("Content-Type: ")) 
            {
                contentType_http = search_for_string_in_http_header(http_line, "Content-Type: ");
                               
                if (contentType_http == "application/octet-stream") 
                {
                    is_firmware_valid = true;
                    Serial.println("Data type corresponds to firmware data: application/octet-stream");
                }
                else
                {
                    Serial.println("Data type doesn't correspond to firmware data: it's different from application/octet-stream");
                    Serial.println("Rebooting ESP32 due to FUOTA error");
                    ESP.restart();
                }
            }
        }
    }
    else 
    {
        Serial.println("[ERROR] FUOTA server connection has failed. FUOTA is cancelled.");
        Serial.println("Rebooting ESP32 due to FUOTA error");
        ESP.restart();
        return;
    }

    /* If both data type and firmware size are valid, start FUOTA process */
    if (FUOTA_firmware_size && is_firmware_valid) 
    {
        /* Check if there's enough Flash size for flashing firmware obtained in FUOTA */
        FUOTA_can_start = Update.begin(FUOTA_firmware_size);   
        if (FUOTA_can_start == true) 
        {
            /* FUOTA has been started!!
  
               FUOTA is made using the APIs provided by ESP32 manufacturer (Espressif) SDK.
               A HTTP data stream is passed to the ESP32 writeStream, and it automatically uses this stream
               to download firmware from remote server and flashes the data downloaded into flash memory.
            */
            Serial.println("FUOTA is happening. Please, wait some seconds until it finishes...");
            bytes_written_to_flash = Update.writeStream(espClient);

            /* Check if the entire firmware binary has been downloaded and flashed */
            if (bytes_written_to_flash == FUOTA_firmware_size) 
            {
                sprintf(verbose_debug_line, "Success: %d bytes written in Flash", bytes_written_to_flash);
                Serial.println(verbose_debug_line);
            }
            else 
            {
                sprintf(verbose_debug_line, "[ERROR] Only %d / %d bytes have been written to flash. FUOTA is cancelled", bytes_written_to_flash, FUOTA_firmware_size);
                Serial.println(verbose_debug_line);
                Serial.println((char *)Update.errorString());
                Serial.println("Rebooting ESP32 due to FUOTA error");
                ESP.restart();
                return;
            }

            if (Update.end()) 
            {                
                if (Update.isFinished()) 
                {
                    Serial.println("FUOTA process has succesfully finished");
                    Serial.println("FUOTA total time: ");
                    Serial.print(calculate_time_difference(timestamp_FUOTA_init)/1000);
                    Serial.println("s");
                    Serial.println("* ESP32 is going to reboot with updated firmware");                    
                    ESP.restart();
                }
            }
            else 
            {
                sprintf(verbose_debug_line, "[ERROR] FUOTA Error #: %d", Update.getError());
                Serial.println((char *)Update.errorString());
                Serial.println(verbose_debug_line);
                Serial.println("Rebooting ESP32 due to FUOTA error");
                ESP.restart();
            }
        }
        else 
        {   
            Serial.println("[ERROR] There isn't enough flash size to download and flash new firmware. FUOTA is cancelled.");
            Serial.println((char *)Update.errorString());
            espClient.flush();
            Serial.println("Rebooting ESP32 due to FUOTA error");
            ESP.restart();
        }
    }
    else 
    {
        Serial.println("[ERROR] No useful data received from HTTP request. FUOTA is cancelled");
        Serial.println((char *)Update.errorString());
        espClient.flush();
        Serial.println("Rebooting ESP32 due to FUOTA error");
        ESP.restart();
    }
}

/* Function: calculate time difference between now and the informed timestamp
 * Parameters: timestamp
 * Return: time difference
 *  
 */
unsigned long calculate_time_difference(unsigned long t_ref)
{
    return (millis() - t_ref);
}

void setup() 
{
    /* Init serial (for verbose debugging) and writes current firmware version in it */
    Serial.begin(115200);
    Serial.print("Firmware version: ");
    Serial.println(FIRMWARE_VERSION);

    /* Init FUOTA push-button GPIO */
    pinMode(FUOTA_PUSH_BUTTON_GPIO, INPUT_PULLUP);
    
    /* Init (empty) FUOTA URL */
    url_FUOTA = "";

    /* Init and connect to wi-fi */
    init_wifi();

    /* Stores the timestamp that firmware started to run */
    timestamp_since_firmware_start = millis();
}

void loop() 
{
    /* Ensure wi-fi connection is active */
    verify_wifi_connection();

    /* Check if FUOTA push-button has been pressed (considering debouncing).
       If yes, FUOTA starts. */
    FUOTA_must_start = false;

    if (digitalRead(FUOTA_PUSH_BUTTON_GPIO) == LOW)
    {
        delay(FUOTA_PUSH_BUTTON_DEBOUNCE_TIME);
        if (digitalRead(FUOTA_PUSH_BUTTON_GPIO) == LOW)
        {
            FUOTA_must_start = true;
        }
    }

    if (FUOTA_must_start == true)
    {
        Serial.println("FUOTA process - start");
        Serial.print("Host: ");
        Serial.println(host_http);
        Serial.print("Firmware filename: ");
        Serial.println(firmware_filename);
        init_FUOTA(host_http, firmware_filename);  
    }

    /* Escreve periodicamente uma mensagem no serial monitor */ 
    Serial.print("Total time since firmware started: ");
    Serial.print(calculate_time_difference(timestamp_since_firmware_start)/1000);
    Serial.println("s");
    delay(1000);
}
