#include <Wire.h>
#include <U8g2lib.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <time.h>
#include <RTClib.h>
#include <INA226.h>
#include <ArduinoJson.h>

// ========== НАСТРОЙКИ (изменяются через Web Portal) ==========
// Параметры по умолчанию (будут перезаписаны после настройки)
char ntpServer[40] = "pool.ntp.org";
long gmtOffsetSec = 10800;  // GMT+3 по умолчанию
int daylightOffsetSec = 0;

char weatherCity[50] = "Moscow";
char weatherApiKey[33] = "";  // Ключ API для OpenWeatherMap

// ========== ИНИЦИАЛИЗАЦИЯ КОМПОНЕНТОВ ==========
// Для OLED 1.3" 128x64 (SSD1306 или SH1106)
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

RTC_DS3231 rtc;
INA226 ina226;

// ========== ГЛОБАЛЬНЫЕ ПЕРЕМЕННЫЕ ==========
bool wifiConnected = false;
unsigned long lastWeatherUpdate = 0;
float currentVoltage = 0.0;
float currentTemperature = 0.0;
String weatherCondition = "";
float weatherTemp = 0.0;
int weatherHumidity = 0;

// ========== WEB-ПАРАМЕТРЫ ДЛЯ НАСТРОЙКИ ==========
WiFiManager wifiManager;
WiFiManagerParameter custom_ntp("ntp", "NTP Server", ntpServer, 40);
WiFiManagerParameter custom_gmt("gmt", "GMT Offset (seconds)", "10800", 8);
WiFiManagerParameter custom_city("city", "Weather City", weatherCity, 50);
WiFiManagerParameter custom_api("apikey", "Weather API Key", weatherApiKey, 32);

// ========== ПРОТОТИПЫ ФУНКЦИЙ ==========
void saveConfigCallback();
void configModeCallback(WiFiManager *myWiFiManager);
bool loadConfig();
void initWiFiManager();
void updateTimeFromNTP();
void updateWeather();
void updateDisplay();
String getFormattedTime();
String getFormattedDate();

// ========== SETUP ==========
void setup() {
    Serial.begin(115200);
    Serial.println("\n\nЗапуск устройства...");

    // 1. Инициализация I2C шины
    Wire.begin(9, 8);  // SDA=GPIO9, SCL=GPIO8
    Serial.println("I2C шина инициализирована");

    // 2. Инициализация OLED
    u8g2.begin();
    u8g2.setFont(u8g2_font_6x10_tr);
    u8g2.enableUTF8Print();
    u8g2.clearBuffer();
    u8g2.drawStr(0, 10, "Загрузка...");
    u8g2.sendBuffer();
    Serial.println("OLED инициализирован");

    // 3. Инициализация DS3231 RTC
    if (!rtc.begin()) {
        Serial.println("Ошибка: DS3231 не найден!");
        u8g2.drawStr(0, 30, "Ошибка RTC!");
        u8g2.sendBuffer();
    } else {
        Serial.println("DS3231 найден");
    }

    // 4. Инициализация INA226
    if (!ina226.begin()) {
        Serial.println("Ошибка: INA226 не найден!");
        u8g2.drawStr(0, 40, "Ошибка INA226!");
        u8g2.sendBuffer();
    } else {
        // Настройка INA226 (опционально)
        ina226.setMaxCurrentShunt(5.0, 0.002);  // 5A max, 0.002 Ohm shunt
        Serial.println("INA226 инициализирован");
    }

    delay(2000);

    // 5. Загрузка сохранённых параметров
    loadConfig();

    // 6. Инициализация WiFiManager (точка доступа для настройки при первом запуске)
    initWiFiManager();

    // 7. Синхронизация времени
    updateTimeFromNTP();

    // 8. Первое обновление погоды
    updateWeather();

    Serial.println("Система готова к работе!");
}

// ========== LOOP ==========
void loop() {
    static unsigned long lastDisplayUpdate = 0;
    static unsigned long lastMeasureTime = 0;
    static unsigned long lastWeatherCheck = 0;

    // Измерение напряжения каждые 500 мс
    if (millis() - lastMeasureTime >= 500) {
        lastMeasureTime = millis();
        
        // Чтение напряжения с INA226
        currentVoltage = ina226.readBusVoltage();
        
        // Чтение температуры с DS3231 (опционально)
        currentTemperature = rtc.getTemperature();
        
        Serial.printf("Напряжение: %.2f V, Температура: %.2f C\n", currentVoltage, currentTemperature);
    }

    // Обновление дисплея каждую секунду
    if (millis() - lastDisplayUpdate >= 1000) {
        lastDisplayUpdate = millis();
        updateDisplay();
    }

    // Обновление погоды каждые 10 минут
    if (millis() - lastWeatherCheck >= 600000) {  // 10 минут
        lastWeatherCheck = millis();
        if (WiFi.status() == WL_CONNECTED) {
            updateWeather();
        }
    }

    // Поддержание WiFi-соединения
    if (WiFi.status() != WL_CONNECTED && wifiConnected) {
        wifiConnected = false;
        Serial.println("WiFi отключен, попытка переподключения...");
    } else if (WiFi.status() == WL_CONNECTED && !wifiConnected) {
        wifiConnected = true;
        Serial.println("WiFi переподключен");
        updateTimeFromNTP();  // Обновить время при переподключении
        updateWeather();
    }
}

// ========== ИНИЦИАЛИЗАЦИЯ WIFIMANAGER ==========
void initWiFiManager() {
    wifiManager.setSaveConfigCallback(saveConfigCallback);
    wifiManager.setAPCallback(configModeCallback);
    
    // Добавление пользовательских параметров
    wifiManager.addParameter(&custom_ntp);
    wifiManager.addParameter(&custom_gmt);
    wifiManager.addParameter(&custom_city);
    wifiManager.addParameter(&custom_api);
    
    // Попытка подключения к сохранённой сети
    if (!wifiManager.autoConnect("VoltClockSetup", "12345678")) {
        Serial.println("Не удалось подключиться к WiFi");
        // Продолжаем работу без WiFi (RTC будет показывать время)
    } else {
        wifiConnected = true;
        Serial.println("Подключено к WiFi!");
        Serial.print("IP адрес: ");
        Serial.println(WiFi.localIP());
    }
}

// Callback при сохранении параметров
void saveConfigCallback() {
    Serial.println("Параметры сохранены");
    
    // Сохранение параметров в NVS (энергонезависимую память)
    strcpy(ntpServer, custom_ntp.getValue());
    gmtOffsetSec = atol(custom_gmt.getValue());
    strcpy(weatherCity, custom_city.getValue());
    strcpy(weatherApiKey, custom_api.getValue());
    
    // Здесь можно сохранить параметры в файл или Preferences
    // Для простоты — сохраняем в Preferences
    #include <Preferences.h>
    Preferences prefs;
    prefs.begin("settings", false);
    prefs.putString("ntp", ntpServer);
    prefs.putLong("gmt", gmtOffsetSec);
    prefs.putString("city", weatherCity);
    prefs.putString("api", weatherApiKey);
    prefs.end();
}

// Callback при входе в режим настройки AP
void configModeCallback(WiFiManager *myWiFiManager) {
    Serial.println("Режим настройки AP активирован");
    Serial.print("SSID точки доступа: ");
    Serial.println(myWiFiManager->getConfigPortalSSID());
    
    // Отображение информации на OLED
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x10_tr);
    u8g2.drawStr(0, 10, "Режим настройки");
    u8g2.drawStr(0, 25, "Подключитесь к:");
    u8g2.drawStr(0, 40, myWiFiManager->getConfigPortalSSID().c_str());
    u8g2.drawStr(0, 55, "IP: 192.168.4.1");
    u8g2.sendBuffer();
}

// Загрузка сохранённых параметров
bool loadConfig() {
    #include <Preferences.h>
    Preferences prefs;
    prefs.begin("settings", true);
    
    if (prefs.isKey("ntp")) {
        strcpy(ntpServer, prefs.getString("ntp", "pool.ntp.org").c_str());
        gmtOffsetSec = prefs.getLong("gmt", 10800);
        strcpy(weatherCity, prefs.getString("city", "Moscow").c_str());
        strcpy(weatherApiKey, prefs.getString("api", "").c_str());
        prefs.end();
        return true;
    }
    prefs.end();
    return false;
}

// ========== ФУНКЦИИ ВРЕМЕНИ ==========
void updateTimeFromNTP() {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("Нет WiFi, используем только RTC");
        return;
    }
    
    Serial.println("Синхронизация времени с NTP...");
    configTime(gmtOffsetSec, daylightOffsetSec, ntpServer);
    
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo, 10000)) {
        Serial.println("Не удалось получить время от NTP");
        return;
    }
    
    // Установка времени в DS3231
    rtc.adjust(DateTime(
        timeinfo.tm_year + 1900,
        timeinfo.tm_mon + 1,
        timeinfo.tm_mday,
        timeinfo.tm_hour,
        timeinfo.tm_min,
        timeinfo.tm_sec
    ));
    
    Serial.println("Время синхронизировано и сохранено в RTC");
}

// Получение отформатированного времени
String getFormattedTime() {
    DateTime now = rtc.now();
    char buffer[9];
    sprintf(buffer, "%02d:%02d:%02d", now.hour(), now.minute(), now.second());
    return String(buffer);
}

String getFormattedDate() {
    DateTime now = rtc.now();
    char buffer[12];
    sprintf(buffer, "%02d.%02d.%04d", now.day(), now.month(), now.year());
    return String(buffer);
}

// ========== ФУНКЦИИ ПОГОДЫ ==========
void updateWeather() {
    if (WiFi.status() != WL_CONNECTED || strlen(weatherApiKey) == 0) {
        Serial.println("Нет WiFi или API ключа, пропускаем обновление погоды");
        return;
    }
    
    HTTPClient http;
    String url = "http://api.openweathermap.org/data/2.5/weather?q=" + 
                 String(weatherCity) + "&units=metric&appid=" + String(weatherApiKey);
    
    http.begin(url);
    int httpCode = http.GET();
    
    if (httpCode == 200) {
        String payload = http.getString();
        DynamicJsonDocument doc(1024);
        deserializeJson(doc, payload);
        
        weatherTemp = doc["main"]["temp"];
        weatherHumidity = doc["main"]["humidity"];
        weatherCondition = doc["weather"][0]["main"].as<String>();
        
        Serial.printf("Погода: %.1f C, %d%%, %s\n", weatherTemp, weatherHumidity, weatherCondition.c_str());
    } else {
        Serial.printf("Ошибка получения погоды: %d\n", httpCode);
    }
    
    http.end();
}

// ========== ФУНКЦИЯ ОБНОВЛЕНИЯ ДИСПЛЕЯ ==========
void updateDisplay() {
    u8g2.clearBuffer();
    
    // Строка 1: Время (крупный шрифт)
    u8g2.setFont(u8g2_font_logisoso22_tf);
    u8g2.setCursor(0, 24);
    u8g2.print(getFormattedTime());
    
    // Строка 2: Дата
    u8g2.setFont(u8g2_font_6x10_tr);
    u8g2.setCursor(0, 38);
    u8g2.print(getFormattedDate());
    
    // Строка 3: Напряжение
    u8g2.setCursor(0, 50);
    u8g2.print("V: ");
    u8g2.print(currentVoltage, 2);
    u8g2.print(" V");
    
    // Строка 4: Погода (если есть WiFi и данные)
    if (wifiConnected && weatherApiKey[0] != '\0') {
        u8g2.setCursor(0, 62);
        u8g2.print(weatherCondition.substring(0, 3));
        u8g2.print(" ");
        u8g2.print(weatherTemp, 1);
        u8g2.print("C ");
        u8g2.print(weatherHumidity);
        u8g2.print("%");
    } else {
        // Или температура с RTC
        u8g2.setCursor(0, 62);
        u8g2.print("T: ");
        u8g2.print(currentTemperature, 1);
        u8g2.print("C");
    }
    
    u8g2.sendBuffer();
}