/*
 * =============================================================================
 * InkyPi E-Paper Display Client
 * =============================================================================
 * 
 * Fetches images from InkyPi server and displays them on a Waveshare 7.3" 
 * Spectra 6 color e-paper display. Supports deep sleep with wake-on-button
 * for energy efficiency.
 * 
 * Hardware:
 *   - FireBeetle 2 ESP32-E (DFRobot)
 *   - Waveshare 7.3" Spectra 6 e-paper (800x480, 6-color)
 * 
 * Display Wiring:
 *   ESP32 Pin     Display Pin
 *   ---------     -----------
 *   3V3           VCC
 *   GND           GND
 *   GPIO23 (MOSI) DIN
 *   GPIO18 (SCK)  CLK
 *   GPIO2  (D9)   CS
 *   GPIO22        DC
 *   GPIO21        RST
 *   GPIO13 (D7)   BUSY
 * 
 * Button Wiring:
 *   Button 1 (Next Plugin):    GPIO14 (D6) with internal pull-up, other leg to GND
 *   Button 2 (Refresh):        GPIO26 (D3) with internal pull-up, other leg to GND
 * 
 * Required Libraries:
 *   - GxEPD2 by Jean-Marc Zingg (install via Library Manager)
 *   - Adafruit GFX Library (installed as GxEPD2 dependency)
 * 
 * =============================================================================
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <GxEPD2_7C.h>
#include <driver/rtc_io.h>  // For RTC GPIO pull-up/pull-down control

// =============================================================================
// CONFIGURATION - Modify these values for your setup
// =============================================================================

const char* WIFI_SSID = "ssid";
const char* WIFI_PASSWORD = "password";

// InkyPi server endpoints
const char* INKYPI_URL = "http://192.168.1.144/api/current_image?format=spectra6";
const char* NEXT_PLUGIN_URL = "http://192.168.1.144/next_plugin";
const char* REFRESH_URL = "http://192.168.1.144/refresh_current";

// How often to automatically refresh the display (in minutes)
const int UPDATE_INTERVAL_MINUTES = 60;

// =============================================================================
// BUTTON CONFIGURATION
// =============================================================================
// Both pins must be RTC GPIOs to support wake from deep sleep.
// GPIO14 (RTC_GPIO16) and GPIO26 (RTC_GPIO7) are valid RTC GPIOs.

#define BUTTON_NEXT_PIN    GPIO_NUM_14  // D6 - Switches to next plugin
#define BUTTON_REFRESH_PIN GPIO_NUM_26  // D3 - Refreshes current plugin

// =============================================================================
// DISPLAY PIN CONFIGURATION
// =============================================================================

#define PIN_SPI_MOSI  23   // Master Out Slave In (data to display)
#define PIN_SPI_CLK   18   // SPI clock
#define PIN_CS         2   // Chip Select
#define PIN_DC        22   // Data/Command select
#define PIN_RST       21   // Hardware reset
#define PIN_BUSY      13   // Busy signal from display

// =============================================================================
// DISPLAY SETUP
// =============================================================================

#define DISPLAY_WIDTH   800
#define DISPLAY_HEIGHT  480

// Initialize display with paging (HEIGHT/4 reduces RAM usage)
GxEPD2_7C<GxEPD2_730c_ACeP_730, GxEPD2_730c_ACeP_730::HEIGHT / 120> display(
    GxEPD2_730c_ACeP_730(PIN_CS, PIN_DC, PIN_RST, PIN_BUSY)
);

// =============================================================================
// GLOBAL VARIABLES
// =============================================================================

// Image buffer size: 4 bits per pixel, 2 pixels per byte
const uint32_t BUFFER_SIZE = (DISPLAY_WIDTH * DISPLAY_HEIGHT) / 2;  // 192,000 bytes

// Pointer to image buffer (allocated dynamically)
uint8_t* imageBuffer = nullptr;

// Boot counter persists across deep sleep cycles (stored in RTC memory)
RTC_DATA_ATTR int bootCount = 0;

// =============================================================================
// WIFI CONNECTION
// =============================================================================

/**
 * Connects to WiFi network.
 * Attempts connection for up to 15 seconds (30 attempts × 500ms).
 * 
 * @return true if connected successfully, false otherwise
 */
bool connectWiFi() {
    Serial.printf("Connecting to WiFi: %s\n", WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 30) {
        delay(500);
        Serial.print(".");
        attempts++;
    }
    
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("\nConnected! IP: %s\n", WiFi.localIP().toString().c_str());
        return true;
    }
    
    Serial.println("\nWiFi connection failed!");
    return false;
}

// =============================================================================
// SERVER API CALLS
// =============================================================================

/**
 * Sends POST request to switch to next plugin on InkyPi server.
 * 
 * @return true if request succeeded (HTTP 200 or 204), false otherwise
 */
bool sendNextPlugin() {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("WiFi not connected, can't send request");
        return false;
    }
    
    Serial.println("Sending POST to /next_plugin...");
    
    HTTPClient http;
    http.begin(NEXT_PLUGIN_URL);
    http.setTimeout(60000);
    http.addHeader("Content-Type", "application/json");
    
    int httpCode = http.POST("");
    http.end();
    
    if (httpCode > 0) {
        Serial.printf("Response: %d\n", httpCode);
        return (httpCode == 200 || httpCode == 204);
    }
    
    Serial.printf("Request failed: %s\n", http.errorToString(httpCode).c_str());
    return false;
}

/**
 * Sends POST request to refresh current plugin on InkyPi server.
 * 
 * @return true if request succeeded (HTTP 200 or 204), false otherwise
 */
bool sendRefresh() {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("WiFi not connected, can't send request");
        return false;
    }
    
    Serial.println("Sending POST to /refresh_current...");
    
    HTTPClient http;
    http.begin(REFRESH_URL);
    http.setTimeout(60000);
    http.addHeader("Content-Type", "application/json");
    
    int httpCode = http.POST("");
    http.end();
    
    if (httpCode > 0) {
        Serial.printf("Response: %d\n", httpCode);
        return (httpCode == 200 || httpCode == 204);
    }
    
    Serial.printf("Request failed: %s\n", http.errorToString(httpCode).c_str());
    return false;
}

// =============================================================================
// WAKE REASON DETECTION
// =============================================================================

/**
 * Determines why the ESP32 woke up from deep sleep.
 * 
 * @return 0 = timer or first boot
 *         1 = button 1 pressed (next plugin)
 *         2 = button 2 pressed (refresh)
 */
int getWakeReason() {
    esp_sleep_wakeup_cause_t reason = esp_sleep_get_wakeup_cause();
    
    switch (reason) {
        case ESP_SLEEP_WAKEUP_EXT0:
            Serial.println("Wake reason: Button 1 (Next Plugin)");
            return 1;
            
        case ESP_SLEEP_WAKEUP_EXT1:
            Serial.println("Wake reason: Button 2 (Refresh)");
            return 2;
            
        case ESP_SLEEP_WAKEUP_TIMER:
            Serial.println("Wake reason: Timer");
            return 0;
            
        default:
            Serial.printf("Wake reason: %d (first boot or reset)\n", reason);
            return 0;
    }
}

// =============================================================================
// IMAGE FETCHING
// =============================================================================

/**
 * Fetches image data from InkyPi server.
 * 
 * The server returns raw 4-bit-per-pixel data where each byte contains
 * two pixels (high nibble = first pixel, low nibble = second pixel).
 * 
 * @return true if image fetched successfully, false otherwise
 */
bool fetchImage() {
    Serial.printf("Fetching image from: %s\n", INKYPI_URL);
    
    HTTPClient http;
    http.begin(INKYPI_URL);
    http.setTimeout(60000);
    
    int httpCode = http.GET();
    if (httpCode != HTTP_CODE_OK) {
        Serial.printf("HTTP error: %d\n", httpCode);
        http.end();
        return false;
    }
    
    int contentLength = http.getSize();
    Serial.printf("Content length: %d bytes (expected: %d)\n", contentLength, BUFFER_SIZE);
    
    // Allocate buffer if not already allocated
    // Try PSRAM first (if available), fall back to regular RAM
    if (imageBuffer == nullptr) {
        imageBuffer = (uint8_t*)ps_malloc(BUFFER_SIZE);
        if (imageBuffer == nullptr) {
            imageBuffer = (uint8_t*)malloc(BUFFER_SIZE);
        }
        if (imageBuffer == nullptr) {
            Serial.println("Failed to allocate image buffer!");
            http.end();
            return false;
        }
    }
    
    // Stream data directly into buffer
    WiFiClient* stream = http.getStreamPtr();
    uint32_t bytesRead = 0;
    uint32_t targetBytes = (contentLength > 0 && contentLength <= BUFFER_SIZE) 
                           ? contentLength : BUFFER_SIZE;
    
    while (bytesRead < targetBytes && http.connected()) {
        size_t available = stream->available();
        if (available) {
            size_t toRead = min(available, (size_t)(targetBytes - bytesRead));
            int read = stream->readBytes(imageBuffer + bytesRead, toRead);
            bytesRead += read;
            
            // Progress indicator every ~20KB
            if (bytesRead % 20000 == 0) {
                Serial.printf("Downloaded: %d / %d bytes\n", bytesRead, targetBytes);
            }
        }
        delay(1);  // Yield to prevent watchdog timeout
    }
    
    http.end();
    Serial.printf("Download complete: %d bytes\n", bytesRead);
    
    // Fill any remaining buffer space with white (index 1 packed as 0x11)
    if (bytesRead < BUFFER_SIZE) {
        memset(imageBuffer + bytesRead, 0x11, BUFFER_SIZE - bytesRead);
    }
    
    return true;
}

// =============================================================================
// DISPLAY RENDERING
// =============================================================================

/**
 * Remaps color indices and sends image to display.
 * 
 * The Waveshare Spectra 6 display has non-standard color index mapping.
 * This function converts from our Python palette indices to the actual
 * hardware indices that produce the correct colors.
 * 
 * Hardware color index mapping (determined through testing):
 *   Index 0 → Black
 *   Index 1 → White
 *   Index 2 → Yellow
 *   Index 3 → Red
 *   Index 4 → White (unused)
 *   Index 5 → Blue
 *   Index 6 → Green
 * 
 * Python palette order:
 *   0=Black, 1=White, 2=Green, 3=Blue, 4=Red, 5=Yellow
 */
void displayImage() {
    Serial.println("Preparing image for display...");
    unsigned long startTime = millis();
    
    // Color remapping table: Python index → Hardware index
    static const uint8_t colorRemap[6] = {
        0,  // Python 0 (Black)  → Hardware 0 (Black)
        1,  // Python 1 (White)  → Hardware 1 (White)
        6,  // Python 2 (Green)  → Hardware 6 (Green)
        5,  // Python 3 (Blue)   → Hardware 5 (Blue)
        3,  // Python 4 (Red)    → Hardware 3 (Red)
        2   // Python 5 (Yellow) → Hardware 2 (Yellow)
    };
    
    // Remap all pixels in the buffer
    for (uint32_t i = 0; i < BUFFER_SIZE; i++) {
        uint8_t packed = imageBuffer[i];
        
        // Extract two 4-bit pixel values
        uint8_t pixel1 = (packed >> 4) & 0x0F;  // High nibble
        uint8_t pixel2 = packed & 0x0F;          // Low nibble
        
        // Apply remapping for valid indices (0-5)
        if (pixel1 < 6) pixel1 = colorRemap[pixel1];
        if (pixel2 < 6) pixel2 = colorRemap[pixel2];
        
        // Pack back into single byte
        imageBuffer[i] = (pixel1 << 4) | pixel2;
    }
    
    Serial.printf("Buffer remapped in %lu ms\n", millis() - startTime);
    
    // Send buffer directly to display controller (much faster than drawPixel loop)
    // Parameters: data, data2(unused), x, y, width, height, invert, mirror_y, pgm
    display.writeNative(imageBuffer, nullptr, 0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, false, false, false);
    display.refresh();
    
    Serial.printf("Display complete in %lu ms\n", millis() - startTime);
}

// =============================================================================
// DEEP SLEEP
// =============================================================================

/**
 * Configures wake sources and enters deep sleep mode.
 * 
 * Wake sources:
 *   - Timer: Wakes after specified minutes
 *   - EXT0: Button 1 (GPIO14) - uses dedicated RTC wake circuit
 *   - EXT1: Button 2 (GPIO26) - uses bitmask-based wake circuit
 * 
 * Deep sleep current consumption is approximately 10µA.
 * 
 * @param minutes Time to sleep before automatic wake
 */
void goToDeepSleep(int minutes) {
    Serial.printf("Entering deep sleep for %d minutes...\n", minutes);
    
    // Free image buffer to release memory
    if (imageBuffer != nullptr) {
        free(imageBuffer);
        imageBuffer = nullptr;
    }
    
    // Configure timer wake (microseconds)
    esp_sleep_enable_timer_wakeup(minutes * 60ULL * 1000000ULL);
    
    // Configure button 1 wake (EXT0 - single pin, level-triggered)
    // Wake when pin goes LOW (button pressed pulls to GND)
    esp_sleep_enable_ext0_wakeup(BUTTON_NEXT_PIN, 0);
    
    // Configure button 2 wake (EXT1 - bitmask, supports multiple pins)
    // Wake when pin goes LOW
    esp_sleep_enable_ext1_wakeup(1ULL << BUTTON_REFRESH_PIN, ESP_EXT1_WAKEUP_ALL_LOW);
    
    // Enable internal pull-ups for buttons during deep sleep
    // This prevents floating pins from causing false wake-ups
    rtc_gpio_pullup_en(BUTTON_NEXT_PIN);
    rtc_gpio_pulldown_dis(BUTTON_NEXT_PIN);
    rtc_gpio_pullup_en(BUTTON_REFRESH_PIN);
    rtc_gpio_pulldown_dis(BUTTON_REFRESH_PIN);
    
    Serial.println("Sleep configured. Wake on: timer, button 1 (D6), or button 2 (D3)");
    Serial.flush();  // Ensure all serial data is sent before sleep
    
    esp_deep_sleep_start();
}

// =============================================================================
// MAIN SETUP
// =============================================================================

/**
 * Main setup function - runs once on each boot/wake.
 * 
 * Execution flow:
 *   1. Determine wake reason (timer, button, or first boot)
 *   2. Initialize display
 *   3. Connect to WiFi
 *   4. If button wake: send appropriate API request
 *   5. Fetch and display new image
 *   6. Enter deep sleep
 */
void setup() {
    Serial.begin(115200);
    delay(1000);  // Allow serial monitor to connect
    
    bootCount++;
    Serial.printf("\n========================================\n");
    Serial.printf("InkyPi Spectra 6 Client - Boot #%d\n", bootCount);
    Serial.printf("========================================\n\n");
    
    // Determine why we woke up
    int wakeReason = getWakeReason();
    
    // Initialize SPI bus for display
    // Parameters: CLK, MISO (unused, -1), MOSI, SS
    SPI.begin(PIN_SPI_CLK, -1, PIN_SPI_MOSI, PIN_CS);
    
    // Initialize display
    // Parameters: baud, initial(full refresh), reset_duration_ms, pulldown_rst
    display.init(115200, true, 50, false);
    display.setRotation(0);  // Landscape orientation
    
    // Connect to WiFi
    if (!connectWiFi()) {
        Serial.println("WiFi failed - sleeping for 5 minutes before retry");
        display.hibernate();
        goToDeepSleep(5);
        return;
    }
    
    // Handle button-triggered actions
    if (wakeReason == 1) {
        Serial.println("Action: Switching to next plugin...");
        sendNextPlugin();
        delay(2000);  // Wait for server to generate new image
    } 
    else if (wakeReason == 2) {
        Serial.println("Action: Refreshing current plugin...");
        sendRefresh();
        delay(2000);  // Wait for server to generate new image
    }
    
    // Fetch and display the image
    if (fetchImage()) {
        displayImage();
    } else {
        Serial.println("Image fetch failed!");
    }
    
    // Clean up and enter deep sleep
    WiFi.disconnect(true);  // Disconnect and disable WiFi
    display.hibernate();     // Put display in low-power mode
    goToDeepSleep(UPDATE_INTERVAL_MINUTES);
}

// =============================================================================
// MAIN LOOP
// =============================================================================

/**
 * Main loop - never executed in this application.
 * 
 * The ESP32 always enters deep sleep at the end of setup(),
 * so this loop function is never reached. It's included only
 * because Arduino requires it.
 */
void loop() {
    delay(1000);
}
