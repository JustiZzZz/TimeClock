#include <HardwareSerial.h>
#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <ArduinoJson.h> // Требуется для UniversalTelegramBot
#include <Wire.h>
#include <RTClib.h>
#include <EEPROM.h>
#include <TM1637Display.h>
#include <TimeLib.h>
#include <ArduinoOTA.h>

// Удалены дублирующиеся определения
#define SERIAL_TX_BUFFER_SIZE 256
#define SERIAL_RX_BUFFER_SIZE 256

// Настройки Wi-Fi
const char* ssid = "TP-Link_BDC8"; // Замените на ваш SSID
const char* password = "16303102"; // Замените на ваш пароль

// Токен Telegram Bot
#define BOT_TOKEN "7778677473:AAF4zGyTZ6kzyWiG5BgZikxfhPwnu-917cw" // Убедитесь, что токен корректен и не публичен

// Инициализация объектов
WiFiClientSecure client;
UniversalTelegramBot bot(BOT_TOKEN, client);
RTC_DS3231 rtc;

// Пины для TM1637
#define CLK D5
#define DIO D6
TM1637Display display(CLK, DIO);

// Пины для других компонентов
#define BUTTON_PIN D4
#define RELAY_PIN D7

// Глобальные переменные
unsigned long lastTimeSync = 0;
const unsigned long timeSyncInterval = 3600000; // 1 час

bool isTemporarySchedule = false; // Флаг временного расписания
bool displayBlink = false;        // Флаг мигания дисплея
unsigned long lastDisplayToggle = 0;
unsigned long lastBellCheck = 0;
const unsigned long bellCheckInterval = 1000; // Проверка каждые 1 сек

bool timeSynchronized = false;          // Флаг синхронизации времени
unsigned long initialSyncStartTime = 0; // Время начала отсчёта при несинхронизированном времени

// Структуры для расписания
struct Schedule {
    uint8_t lessons;
    uint16_t lessonDurations[10]; // В минутах
    uint16_t breakDurations[10];  // В минутах
    uint8_t startHour;            // Час начала первого урока
    uint8_t startMinute;          // Минута начала первого урока
    bool isThursday;
} __attribute__((packed));

// Структура для праздников и каникул
struct Holiday {
    uint16_t startYear;
    uint8_t startMonth;
    uint8_t startDay;
    uint16_t endYear;
    uint8_t endMonth;
    uint8_t endDay;
} __attribute__((packed));

#define EEPROM_SIZE 1024
#define EEPROM_ADDR_MAIN_WEEKDAY 0
#define EEPROM_ADDR_MAIN_SATURDAY (sizeof(Schedule))
#define EEPROM_ADDR_TEMP_SCHEDULE (sizeof(Schedule) * 2)
#define EEPROM_ADDR_HOLIDAY_COUNT (sizeof(Schedule) * 3)
#define EEPROM_ADDR_HOLIDAYS (sizeof(Schedule) * 3 + sizeof(uint8_t))

// Основное и временное расписание
Schedule mainScheduleWeekday;
Schedule mainScheduleSaturday;
Schedule tempSchedule;

// Праздники и каникулы
#define MAX_HOLIDAYS 10
Holiday holidays[MAX_HOLIDAYS];
uint8_t holidayCount = 0;

// Идентификаторы администраторов Telegram
#define ADMIN_COUNT 2
const String adminIDs[ADMIN_COUNT] = {"473088478", "1201028902"};

// Массив с названиями дней недели
const char* daysOfWeek[] = {
    "Воскресенье", // 0
    "Понедельник", // 1
    "Вторник",     // 2
    "Среда",       // 3
    "Четверг",     // 4
    "Пятница",     // 5
    "Суббота"      // 6
};

// Управление состоянием пользователей
struct UserState {
    String chat_id;
    bool waitingForLessonData;
    bool waitingForBreaksData;
    bool waitingForHolidayData;
    bool waitingForFirstLessonTime;
    bool waitingForScheduleType;
} __attribute__((packed));

#define MAX_USERS 5
UserState userStates[MAX_USERS];

// Переменные для отслеживания событий звонка
int lastBellMinute = -1; // Отслеживает последнюю минуту, когда был звонок

// Структура для управления состоянием звонка
struct BellAction {
    enum State { IDLE, RINGING_SHORT, WAITING_SHORT, RINGING_LONG } state;
    unsigned long startTime;
    unsigned long duration;
    unsigned long waitDuration;
    uint8_t repeatCount;      // Количество повторов для коротких звонков
    uint8_t repeatCounter;    // Текущий счетчик повторов
} bellAction = {BellAction::IDLE, 0, 0, 0, 0, 0};

// Прототипы функций
void handleNewMessages(int numNewMessages);
void checkBell();
bool isHoliday(DateTime now);
bool syncTime();
void checkDisplay();
void initSchedules();
void startBell(unsigned long duration, uint8_t repeatCount = 1, unsigned long waitDuration = 0);
void sendLog(String message);
void checkRTC();
UserState* getUserState(String chat_id);
int calculateMinutesLeft(DateTime now);
void saveMainSchedule();
void loadMainSchedule();
void saveTempSchedule();
void loadTempSchedule();
void saveHolidays();
void loadHolidays();

void setup() {
    Serial.begin(115200);
    delay(1000); // Ждём, пока монитор порта подключится

    // Инициализация EEPROM
    EEPROM.begin(EEPROM_SIZE);

    // Инициализация дисплея
    display.setBrightness(0x0f);

    // Инициализация реле и кнопки
    pinMode(RELAY_PIN, OUTPUT);
    digitalWrite(RELAY_PIN, LOW); // Убедимся, что HIGH отключает реле
    pinMode(BUTTON_PIN, INPUT_PULLUP);

    // Инициализация RTC
    if (!rtc.begin()) {
        Serial.println("Не удалось найти RTC");
        while (1); // Останавливаемся, если RTC не найден
    }

    // Проверка потери питания RTC
    if (rtc.lostPower()) {
        Serial.println("RTC потерял питание, устанавливаем время!");
        // Устанавливаем текущее время, если потеряно питание
        rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
    }

    // Подключение к Wi-Fi
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);
    client.setInsecure(); // Для работы с Telegram

    // Ожидание подключения
    Serial.print("Подключение к Wi-Fi");
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\nПодключено к Wi-Fi");

    // Инициализация OTA
    ArduinoOTA.setHostname("SmartBell"); // Установите желаемое имя устройства

    ArduinoOTA.onStart([]() {
        String type;
        if (ArduinoOTA.getCommand() == U_FLASH) {
            type = "прошивка";
        } else { // U_SPIFFS
            type = "файловая система";
        }
        Serial.println("Начало обновления " + type);
        digitalWrite(RELAY_PIN, LOW); // Отключаем реле перед обновлением
    });

    ArduinoOTA.onEnd([]() {
        Serial.println("\nОбновление завершено");
    });

    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
        Serial.printf("Прогресс: %u%%\r", (progress / (total / 100)));
    });

    ArduinoOTA.onError([](ota_error_t error) {
        Serial.printf("Ошибка[%u]: ", error);
        if (error == OTA_AUTH_ERROR) {
            Serial.println("Ошибка аутентификации");
        } else if (error == OTA_BEGIN_ERROR) {
            Serial.println("Ошибка начала обновления");
        } else if (error == OTA_CONNECT_ERROR) {
            Serial.println("Ошибка подключения");
        } else if (error == OTA_RECEIVE_ERROR) {
            Serial.println("Ошибка получения данных");
        } else if (error == OTA_END_ERROR) {
            Serial.println("Ошибка завершения");
        }
    });

    ArduinoOTA.begin();
    Serial.println("OTA готова");
    Serial.print("IP адрес: ");
    Serial.println(WiFi.localIP());

    // Загрузка настроек
    loadMainSchedule();
    loadTempSchedule();
    loadHolidays();

    // Инициализация расписаний
    initSchedules();

    // Синхронизация времени
    sendLog("Начало синхронизации времени...");
    timeSynchronized = syncTime();

    if (timeSynchronized) {
        sendLog("Время синхронизировано при запуске.");
    } else {
        sendLog("Не удалось синхронизировать время при запуске. Запуск обратного отсчёта.");
        initialSyncStartTime = millis();
    }
}

void loop() {
    ArduinoOTA.handle();

    // Обработка новых сообщений Telegram
    if (millis() - bot.last_message_received > 1000) {
        int numNewMessages = bot.getUpdates(bot.last_message_received + 1);
        while (numNewMessages) {
            handleNewMessages(numNewMessages);
            numNewMessages = bot.getUpdates(bot.last_message_received + 1);
        }
    }

    // Проверка необходимости синхронизации времени
    if (millis() - lastTimeSync > timeSyncInterval) {
        bool success = syncTime();
        if (success) {
            timeSynchronized = true;
        }
        lastTimeSync = millis();
    }

    // Проверка состояния звонка
    if (millis() - lastBellCheck > bellCheckInterval) {
        checkBell();
        lastBellCheck = millis();
    }

    // Обновление отображения на дисплее
    checkDisplay();

    // Проверка RTC
    checkRTC();

    // Обработка состояния звонка
    switch (bellAction.state) {
        case BellAction::IDLE:
            // Ничего не делаем
            break;

        case BellAction::RINGING_SHORT:
            if (millis() - bellAction.startTime >= bellAction.duration) {
                digitalWrite(RELAY_PIN, LOW); // Выключаем реле
                bellAction.startTime = millis();
                if (bellAction.repeatCounter < bellAction.repeatCount - 1) {
                    bellAction.state = BellAction::WAITING_SHORT;
                } else {
                    bellAction.state = BellAction::IDLE;
                    Serial.println("Короткие звонки завершены.");
                }
            }
            break;

        case BellAction::WAITING_SHORT:
            if (millis() - bellAction.startTime >= bellAction.waitDuration) {
                digitalWrite(RELAY_PIN, HIGH); // Включаем реле
                bellAction.state = BellAction::RINGING_SHORT;
                bellAction.startTime = millis();
                bellAction.repeatCounter++;
            }
            break;

        case BellAction::RINGING_LONG:
            if (millis() - bellAction.startTime >= bellAction.duration) {
                digitalWrite(RELAY_PIN, LOW); // Выключаем реле
                bellAction.state = BellAction::IDLE;
                Serial.println("Длительный звонок завершён.");
            }
            break;
    }

    // Обработка нажатия кнопки для переключения расписания
    static bool lastButtonState = HIGH;
    bool currentButtonState = digitalRead(BUTTON_PIN);

    if (lastButtonState == HIGH && currentButtonState == LOW) {
        // Кнопка нажата
        isTemporarySchedule = !isTemporarySchedule;
        if (isTemporarySchedule) {
            sendLog("Переключено на временное расписание");
        } else {
            sendLog("Переключено на основное расписание");
        }
        delay(500); // Антидребезг
    }
    lastButtonState = currentButtonState;
}

// Функция обработки новых сообщений Telegram
void handleNewMessages(int numNewMessages) {
    for (int i = 0; i < numNewMessages; i++) {
        String chat_id = String(bot.messages[i].chat_id);
        String text = bot.messages[i].text;

        // Проверка прав доступа
        bool isAdmin = false;
        for (uint8_t j = 0; j < ADMIN_COUNT; j++) {
            if (adminIDs[j] == chat_id) {
                isAdmin = true;
                break;
            }
        }
        if (!isAdmin) {
            bot.sendMessage(chat_id, "У вас нет прав для выполнения этой команды.", "");
            continue;
        }

        // Получение состояния пользователя
        UserState* userState = getUserState(chat_id);

        // Проверка ожидания ввода от пользователя
        if (userState->waitingForLessonData) {
            // Обработка данных уроков
            String lessonData = text;
            userState->waitingForLessonData = false;
            // Разбор строки lessonData и обновление tempSchedule
            // Пример: "5 45 45 45 45 45"
            int lessonCount = 0;
            int durations[10];
            int index = 0;
            char* ptr = strtok((char*)lessonData.c_str(), " ");
            while (ptr != NULL && index < 11) { // Первое число - количество уроков
                if (index == 0) {
                    lessonCount = atoi(ptr);
                    if (lessonCount < 1 || lessonCount > 10) {
                        bot.sendMessage(chat_id, "Некорректное количество уроков. Попробуйте снова.", "");
                        break;
                    }
                } else {
                    durations[index - 1] = atoi(ptr);
                }
                ptr = strtok(NULL, " ");
                index++;
            }
            if (index - 1 != lessonCount) {
                bot.sendMessage(chat_id, "Количество введённых длительностей уроков не соответствует указанному количеству уроков.", "");
            } else {
                tempSchedule.lessons = lessonCount;
                for (int k = 0; k < lessonCount; k++) {
                    tempSchedule.lessonDurations[k] = durations[k];
                }
                bot.sendMessage(chat_id, "Введите длительность перемен через пробел. Например:\n15 15 20 20 15", "");
                userState->waitingForBreaksData = true;
            }
            continue;
        }

        if (userState->waitingForBreaksData) {
            // Обработка данных перемен
            String breaksData = text;
            userState->waitingForBreaksData = false;
            // Разбор строки breaksData и обновление tempSchedule
            // Пример: "15 15 20 20 15"
            int breakDurations[10];
            int index = 0;
            char* ptr = strtok((char*)breaksData.c_str(), " ");
            while (ptr != NULL && index < 10) {
                breakDurations[index] = atoi(ptr);
                ptr = strtok(NULL, " ");
                index++;
            }
            if (index != tempSchedule.lessons - 1) {
                bot.sendMessage(chat_id, "Количество введённых длительностей перемен не соответствует количеству уроков - 1.", "");
            } else {
                for (int k = 0; k < index; k++) {
                    tempSchedule.breakDurations[k] = breakDurations[k];
                }
                // Сохранение временного расписания
                saveTempSchedule();
                bot.sendMessage(chat_id, "Временное расписание успешно обновлено.", "");
            }
            continue;
        }

        if (userState->waitingForHolidayData) {
            // Обработка данных праздников и каникул
            String holidayData = text;
            userState->waitingForHolidayData = false;
            // Разбор строки holidayData и обновление holidays
            // Пример: "25.12.2024-10.01.2025"
            int day1, month1, year1, day2, month2, year2;
            if (sscanf(holidayData.c_str(), "%d.%d.%d-%d.%d.%d", &day1, &month1, &year1, &day2, &month2, &year2) == 6) {
                if (holidayCount < MAX_HOLIDAYS) {
                    holidays[holidayCount].startDay = day1;
                    holidays[holidayCount].startMonth = month1;
                    holidays[holidayCount].startYear = year1;
                    holidays[holidayCount].endDay = day2;
                    holidays[holidayCount].endMonth = month2;
                    holidays[holidayCount].endYear = year2;
                    holidayCount++;
                    saveHolidays();
                    bot.sendMessage(chat_id, "Праздничные дни успешно добавлены.", "");
                } else {
                    bot.sendMessage(chat_id, "Достигнуто максимальное количество праздничных дней.", "");
                }
            } else {
                bot.sendMessage(chat_id, "Некорректный формат дат. Попробуйте снова.", "");
            }
            continue;
        }

        if (userState->waitingForFirstLessonTime) {
            // Обработка времени начала первого урока
            String timeData = text;
            userState->waitingForFirstLessonTime = false;
            // Разбор строки timeData и обновление текущего расписания
            // Пример: "08:00"
            int hour, minute;
            if (sscanf(timeData.c_str(), "%d:%d", &hour, &minute) == 2) {
                if (hour < 0 || hour > 23 || minute < 0 || minute > 59) {
                    bot.sendMessage(chat_id, "Некорректное время. Попробуйте снова.", "");
                } else {
                    // Установка времени начала для текущего расписания
                    Schedule* currentSchedule;
                    if (isTemporarySchedule) {
                        currentSchedule = &tempSchedule;
                    } else {
                        DateTime now = rtc.now();
                        if (now.dayOfTheWeek() == 6) { // Суббота (исправлено с 7 на 6)
                            currentSchedule = &mainScheduleSaturday;
                        } else {
                            currentSchedule = &mainScheduleWeekday;
                        }
                    }
                    currentSchedule->startHour = hour;
                    currentSchedule->startMinute = minute;
                    if (isTemporarySchedule) {
                        saveTempSchedule();
                    } else {
                        saveMainSchedule();
                    }
                    bot.sendMessage(chat_id, "Время начала первого урока успешно установлено.", "");
                }
            } else {
                bot.sendMessage(chat_id, "Некорректный формат. Используйте ЧЧ:ММ.", "");
            }
            continue;
        }

        if (userState->waitingForScheduleType) {
            // Обработка выбора типа расписания
            String option = text;
            userState->waitingForScheduleType = false;
            if (option == "1") {
                isTemporarySchedule = false;
                bot.sendMessage(chat_id, "Установлено основное расписание (Пн-Пт).", "");
            }
            else if (option == "2") {
                isTemporarySchedule = false;
                bot.sendMessage(chat_id, "Установлено основное расписание (Суббота).", "");
            }
            else if (option == "3") {
                isTemporarySchedule = true;
                bot.sendMessage(chat_id, "Установлено временное расписание.", "");
            }
            else if (option == "4") {
                // Установка отладочного расписания
                isTemporarySchedule = true;
                tempSchedule.lessons = 5;
                for (int i = 0; i < tempSchedule.lessons; i++) {
                    tempSchedule.lessonDurations[i] = 3; // 3 минуты
                }
                for (int i = 0; i < tempSchedule.lessons - 1; i++) {
                    tempSchedule.breakDurations[i] = 2; // 2 минуты
                }
                tempSchedule.startHour = 0;
                tempSchedule.startMinute = 0;
                saveTempSchedule();
                bot.sendMessage(chat_id, "Отладочное расписание установлено: 5 уроков по 3 мин и 4 перемены по 2 мин.", "");
            }
            else {
                bot.sendMessage(chat_id, "Неверный выбор. Попробуйте снова.", "");
            }
            continue;
        }

        // Обработка команд
        if (text == "/start") {
            String welcome = "Добро пожаловать! Выберите действие:\n";
            welcome += "/set_temp_schedule - Установить временное расписание\n";
            welcome += "/show_schedule - Показать текущее расписание\n";
            welcome += "/set_breaks - Изменить перемены\n";
            welcome += "/settings - Настройки\n";
            welcome += "/time - Показать текущее время\n";
            welcome += "/set_first_lesson_time - Установить время начала первого урока\n";
            welcome += "/select_schedule_type - Выбрать тип расписания";
            bot.sendMessage(chat_id, welcome, "");
        }
        else if (text.startsWith("/set_temp_schedule")) {
            // Установка временного расписания
            bot.sendMessage(chat_id, "Введите количество уроков и продолжительность каждого урока через пробел. Например:\n5 45 45 45 45 45", "");
            userState->waitingForLessonData = true;
        }
        else if (text == "/show_schedule") {
            // Отправка текущего расписания
            String scheduleInfo = "Текущее расписание:\n";
            Schedule currentSchedule;
            if (isTemporarySchedule) {
                currentSchedule = tempSchedule;
                scheduleInfo += "Временное расписание\n";
            } else {
                DateTime now = rtc.now();
                if (now.dayOfTheWeek() == 6) { // Суббота (исправлено с 7 на 6)
                    currentSchedule = mainScheduleSaturday;
                    scheduleInfo += "Основное расписание (Суббота)\n";
                } else {
                    currentSchedule = mainScheduleWeekday;
                    scheduleInfo += "Основное расписание (Пн-Пт)\n";
                }
            }

            // Начало занятий
            uint16_t scheduleStart = currentSchedule.startHour * 60 + currentSchedule.startMinute; // В минутах
            uint16_t timePointer = scheduleStart;

            for (uint8_t k = 0; k < currentSchedule.lessons; k++) {
                // Время начала урока
                uint8_t lessonHour = timePointer / 60;
                uint8_t lessonMinute = timePointer % 60;
                scheduleInfo += "Урок " + String(k + 1) + ": " + String(lessonHour) + ":" + (lessonMinute < 10 ? "0" : "") + String(lessonMinute) + "\n";
                timePointer += currentSchedule.lessonDurations[k];

                if (k < currentSchedule.lessons - 1) {
                    // Время начала перемены
                    uint8_t breakHour = timePointer / 60;
                    uint8_t breakMinute = timePointer % 60;
                    scheduleInfo += "Перемена: " + String(breakHour) + ":" + (breakMinute < 10 ? "0" : "") + String(breakMinute) + "\n";
                    timePointer += currentSchedule.breakDurations[k];
                }
            }
            bot.sendMessage(chat_id, scheduleInfo, "");
        }
        else if (text.startsWith("/set_breaks")) {
            // Изменение перемен
            bot.sendMessage(chat_id, "Введите длительность перемен через пробел. Например:\n15 15 20 20 15", "");
            userState->waitingForBreaksData = true;
        }
        else if (text == "/settings") {
            // Настройки
            String settingsMenu = "Настройки:\n";
            settingsMenu += "/set_holidays - Установить праздничные дни и каникулы\n";
            settingsMenu += "/sync_time - Синхронизировать время\n";
            settingsMenu += "/ota_update - Обновление прошивки\n";
            settingsMenu += "/time - Показать текущее время\n";
            settingsMenu += "/set_first_lesson_time - Установить время начала первого урока\n";
            settingsMenu += "/select_schedule_type - Выбрать тип расписания";
            bot.sendMessage(chat_id, settingsMenu, "");
        }
        else if (text.startsWith("/set_holidays")) {
            // Установка праздников и каникул
            bot.sendMessage(chat_id, "Введите даты начала и конца каникул в формате ДД.ММ.ГГГГ-ДД.ММ.ГГГГ. Например:\n25.12.2024-10.01.2025", "");
            userState->waitingForHolidayData = true;
        }
        else if (text == "/sync_time") {
            // Синхронизация времени
            bool success = syncTime();
            if (success) {
                timeSynchronized = true;
                sendLog("Время успешно синхронизировано через Telegram.");
                initialSyncStartTime = 0;
                bot.sendMessage(chat_id, "Время успешно синхронизировано.", "");
            } else {
                sendLog("Не удалось синхронизировать время через Telegram.");
                bot.sendMessage(chat_id, "Не удалось синхронизировать время.", "");
            }
        }
        else if (text == "/ota_update") {
            // Обновление прошивки
            bot.sendMessage(chat_id, "Чтобы обновить прошивку по воздуху, подключитесь к устройству по IP " + WiFi.localIP().toString() + " с помощью Arduino IDE или другого инструмента OTA.", "");
        }
        else if (text == "/time") { // Команда для проверки времени
            DateTime now = rtc.now();

            // Получение дня недели
            int dow = now.dayOfTheWeek(); // 0 = Воскресенье, 6 = Суббота
            String dayName = (dow >= 0 && dow <= 6) ? String(daysOfWeek[dow]) : "Неизвестный день";

            // Форматирование даты и времени                                              
            String currentTime = "Текущее время:\n";
            currentTime += "День недели: " + dayName + "\n";
            currentTime += "Дата: " + String(now.day()) + "." + String(now.month()) + "." + String(now.year()) + "\n";
            currentTime += "Время: " + String(now.hour()) + ":" + (now.minute() < 10 ? "0" : "") + String(now.minute()) + ":" + (now.second() < 10 ? "0" : "") + String(now.second());

            bot.sendMessage(chat_id, currentTime, "");
        }
        else if (text.startsWith("/set_first_lesson_time")) {
            // Команда для установки времени начала первого урока
            bot.sendMessage(chat_id, "Введите время начала первого урока в формате ЧЧ:ММ (например, 08:00)", "");
            userState->waitingForFirstLessonTime = true;
        }
        else if (text.startsWith("/select_schedule_type")) {
            // Команда для выбора типа расписания
            String options = "Выберите тип расписания:\n";
            options += "1. Основное расписание (Пн-Пт)\n";
            options += "2. Основное расписание (Суббота)\n";
            options += "3. Временное расписание\n";
            options += "4. Отладочное расписание\n";
            bot.sendMessage(chat_id, options, "");
            userState->waitingForScheduleType = true;
        }
        else {
            bot.sendMessage(chat_id, "Неизвестная команда. Введите /start для просмотра доступных команд.", "");
        }
    }
}

// Функция получения состояния пользователя
UserState* getUserState(String chat_id) {
    for (int i = 0; i < MAX_USERS; i++) {
        if (userStates[i].chat_id == chat_id) {
            return &userStates[i];
        } else if (userStates[i].chat_id == "") {
            userStates[i].chat_id = chat_id;
            userStates[i].waitingForLessonData = false;
            userStates[i].waitingForBreaksData = false;
            userStates[i].waitingForHolidayData = false;
            userStates[i].waitingForFirstLessonTime = false;
            userStates[i].waitingForScheduleType = false;
            return &userStates[i];
        }
    }
    // Если достигнуто максимальное количество пользователей
    return &userStates[0]; // Возвращаем первый элемент как запасной вариант
}

// Функция проверки необходимости звонка
void checkBell() {
    DateTime now = rtc.now();
    Serial.print("Сегодня день недели: ");
    Serial.println(now.dayOfTheWeek()); // 0 = Воскресенье, 6 = Суббота

    // Если используется временное расписание, пропускаем проверки на праздники и выходные
    bool skipHolidayCheck = isTemporarySchedule;

    // Проверка на праздники и выходные
    if (!skipHolidayCheck && (isHoliday(now) || now.dayOfTheWeek() == 0)) { // Воскресенье (исправлено с 1 на 0)
        // Сегодня звонки не требуются
        return;
    }

    // Выбор расписания
Schedule currentSchedule;
if (isTemporarySchedule) {
    currentSchedule = tempSchedule;
} else {
    if (now.dayOfTheWeek() == 4) { // Четверг
        // Создаём временную копию основного расписания
        currentSchedule = mainScheduleWeekday;
        Schedule thursdaySchedule = currentSchedule;
        // Меняем только копию
        for (uint8_t i = 0; i < thursdaySchedule.lessons; i++) {
            thursdaySchedule.lessonDurations[i] = 45;
        }
        thursdaySchedule.lessonDurations[6] = 30;
        uint16_t breaksThursday[] = {15, 15, 20, 20, 15, 35};
        memcpy(thursdaySchedule.breakDurations, breaksThursday, sizeof(breaksThursday));
        thursdaySchedule.startHour = 8;
        thursdaySchedule.startMinute = 30;
        // Используем модифицированную копию
        currentSchedule = thursdaySchedule;
    } else if (now.dayOfTheWeek() == 6) {
        currentSchedule = mainScheduleSaturday;
    } else {
        currentSchedule = mainScheduleWeekday;
    }
}

    // Определение начала занятий
    uint16_t scheduleStart = currentSchedule.startHour * 60 + currentSchedule.startMinute;
    uint16_t timePointer = scheduleStart;

    // Текущее время в минутах
    uint16_t totalMinutes = now.hour() * 60 + now.minute();

    for (uint8_t i = 0; i < currentSchedule.lessons; i++) {
        // Предварительный звонок за 1 минуту до начала урока
        if (totalMinutes == (timePointer - 1)) {
            if (lastBellMinute != (timePointer - 1)) {
                if (bellAction.state == BellAction::IDLE) {
                    startBell(500, 1, 500); // Два коротких звонка по 0,5 сек с интервалом 0,5 сек (исправлено время)
                    Serial.println("Предварительный звонок перед уроком.");
                    lastBellMinute = (timePointer - 1);
                }
            }
        }

        // Звонок на начало урока
        if (totalMinutes == timePointer) {
            if (lastBellMinute != timePointer) {
                if (bellAction.state == BellAction::IDLE) {
                    startBell(3000, 1, 0); // Один длительный звонок на 3 секунды (исправлено время и duration)
                    Serial.println("Звонок на начало урока.");
                    lastBellMinute = timePointer;
                }
            }
        }

        // Добавляем длительность урока
        timePointer += currentSchedule.lessonDurations[i];

        // Звонок на конец урока
        if (totalMinutes == timePointer) {
            if (lastBellMinute != timePointer) {
                if (bellAction.state == BellAction::IDLE) {
                    startBell(3000, 1, 0); // Один длительный звонок на 3 секунды (исправлено время и duration)
                    Serial.println("Звонок на конец урока.");
                    lastBellMinute = timePointer;
                }
            }
        }

        // Добавляем длительность перемены
        if (i < currentSchedule.lessons - 1) {
            timePointer += currentSchedule.breakDurations[i];
        }
    }
}

// Функция проверки, является ли текущий день праздничным
bool isHoliday(DateTime now) {
    for (uint8_t i = 0; i < holidayCount; i++) {
        DateTime startDate(holidays[i].startYear, holidays[i].startMonth, holidays[i].startDay, 0, 0, 0);
        DateTime endDate(holidays[i].endYear, holidays[i].endMonth, holidays[i].endDay, 23, 59, 59);
        if (now >= startDate && now <= endDate) {
            return true;
        }
    }
    return false;
}

// Функция синхронизации времени с NTP-сервером
bool syncTime() {
    configTime(3 * 3600, 0, "pool.ntp.org", "time.nist.gov"); // Устанавливаем часовой пояс GMT+3
    Serial.println("Синхронизация времени с NTP...");

    int retries = 0;
    const int maxRetries = 10;

    // Ожидание получения времени
    while (time(nullptr) < 100000 && retries < maxRetries) { // Проверяем, установлено ли время
        Serial.print(".");
        delay(1000);
        retries++;
    }

    time_t now = time(nullptr);

    if (now < 100000) { // Если не удалось получить время
        sendLog("Не удалось синхронизировать время с NTP-сервером.");
        Serial.println("\nНе удалось синхронизировать время с NTP-сервером.");
        return false;
    }

    struct tm* timeinfo = localtime(&now);
    rtc.adjust(DateTime(
        timeinfo->tm_year + 1900,
        timeinfo->tm_mon + 1,
        timeinfo->tm_mday,
        timeinfo->tm_hour,
        timeinfo->tm_min,
        timeinfo->tm_sec
    ));

    sendLog("Время синхронизировано с NTP-сервером.");
    Serial.println("\nВремя синхронизировано с NTP-сервером.");
    Serial.print("Текущее время RTC: ");
    Serial.println(rtc.now().timestamp(DateTime::TIMESTAMP_TIME));
    return true;
}

// Функция обновления отображения на дисплее
void checkDisplay() {
    DateTime now = rtc.now();

    if (!timeSynchronized) {
        if (initialSyncStartTime == 0) {
            initialSyncStartTime = millis();
        }
        // Переключение между двумя отсчётами каждые 2 секунды
        if (millis() - lastDisplayToggle > 2000) {
            displayBlink = !displayBlink;
            lastDisplayToggle = millis();
        }
        if (displayBlink) {
            // Показываем первоначальный отсчёт (3:00)
            unsigned long elapsedSeconds = (millis() - initialSyncStartTime) / 1000;
            int secondsLeft = 180 - elapsedSeconds; // 3:00
            if (secondsLeft < 0) secondsLeft = 0;
            display.showNumberDec(secondsLeft, true);
        } else {
            // Показываем следующий отсчёт (3:30)
            unsigned long elapsedSeconds = (millis() - initialSyncStartTime) / 1000;
            int secondsLeft = 210 - elapsedSeconds; // 3:30
            if (secondsLeft < 0) secondsLeft = 0;
            display.showNumberDec(secondsLeft, true);
        }
        return;
    }

    // Обычное отображение: время и оставшееся время
    if (millis() - lastDisplayToggle > 2000) {
        displayBlink = !displayBlink;
        lastDisplayToggle = millis();
    }

    if (displayBlink) {
        // Отображение текущего времени
        uint16_t displayTime = now.hour() * 100 + now.minute();
        display.showNumberDecEx(displayTime, 0b01000000, true);
    } else {
        // Отображение оставшегося времени до следующего события
        int minutesLeft = calculateMinutesLeft(now);
        if (minutesLeft > 9999) minutesLeft = 9999; // Ограничение на 4 цифры
        display.showNumberDec(minutesLeft, true);
    }

    // Мигание дисплея при временном расписании
    if (isTemporarySchedule) {
        static bool blinkState = true;
        static unsigned long lastBlinkTime = 0;
        if (millis() - lastBlinkTime > 50) { // Частота 20 Гц
            blinkState = !blinkState;
            lastBlinkTime = millis();
            if (blinkState) {
                display.setBrightness(0x0f);
            } else {
                display.setBrightness(0x00);
            }
        }
    } else {
        display.setBrightness(0x0f);
    }
}

// Функция вычисления оставшегося времени до следующего события
int calculateMinutesLeft(DateTime now) {
    // Выбор расписания
    Schedule currentSchedule;
    if (isTemporarySchedule) {
        currentSchedule = tempSchedule;
    } else {
        if (now.dayOfTheWeek() == 6) { // Суббота (исправлено с 7 на 6)
            currentSchedule = mainScheduleSaturday;
        } else {
            currentSchedule = mainScheduleWeekday;
        }
    }

    // Определение начала занятий
    uint16_t scheduleStart = currentSchedule.startHour * 60 + currentSchedule.startMinute;
    uint16_t timePointer = scheduleStart;

    // Текущее время в минутах
    uint16_t totalMinutes = now.hour() * 60 + now.minute();

    // Проход по расписанию для нахождения следующего события
    for (uint8_t i = 0; i < currentSchedule.lessons; i++) {
        if (totalMinutes < timePointer) {
            return timePointer - totalMinutes;
        }
        timePointer += currentSchedule.lessonDurations[i];

        if (totalMinutes < timePointer) {
            return timePointer - totalMinutes;
        }

        if (i < currentSchedule.lessons - 1) {
            timePointer += currentSchedule.breakDurations[i];
            if (totalMinutes < timePointer) {
                return timePointer - totalMinutes;
            }
        }
    }

    // После окончания занятий
    return 0;
}

// Функция инициализации расписаний
void initSchedules() {
    // Инициализация основного расписания (Пн-Пт)
    mainScheduleWeekday.lessons = 7;
    for (uint8_t i = 0; i < mainScheduleWeekday.lessons; i++) {
        mainScheduleWeekday.lessonDurations[i] = 45;
    }
    uint16_t breaksWeekday[] = {15, 15, 20, 20, 15, 15};
    memcpy(mainScheduleWeekday.breakDurations, breaksWeekday, sizeof(breaksWeekday));
    mainScheduleWeekday.startHour = 8;
    mainScheduleWeekday.startMinute = 30;

    // Инициализация расписания на субботу
    mainScheduleSaturday.lessons = 4;
    for (uint8_t i = 0; i < mainScheduleSaturday.lessons; i++) {
        mainScheduleSaturday.lessonDurations[i] = 45;
    }
    uint16_t breaksSaturday[] = {5, 5, 30};
    memcpy(mainScheduleSaturday.breakDurations, breaksSaturday, sizeof(breaksSaturday));
    mainScheduleSaturday.startHour = 8;
    mainScheduleSaturday.startMinute = 30;

    // Если временное расписание не установлено, инициализируем его как основное
    if (tempSchedule.lessons == 0) {
        tempSchedule = mainScheduleWeekday;
    }
}

// Функция запуска звонка на заданную длительность
void startBell(unsigned long duration, uint8_t repeatCount, unsigned long waitDuration) {
    if (repeatCount > 0 && duration > 0) {
        if (repeatCount > 1) {
            // Короткие звонки
            bellAction.state = BellAction::RINGING_SHORT;
            bellAction.startTime = millis();
            bellAction.duration = duration;
            bellAction.waitDuration = waitDuration;
            bellAction.repeatCount = repeatCount;
            bellAction.repeatCounter = 0;
            digitalWrite(RELAY_PIN, HIGH); // Включаем реле
            Serial.println("Начат короткий звонок.");
        } else {
            // Длительный звонок
            bellAction.state = BellAction::RINGING_LONG;
            bellAction.startTime = millis();
            bellAction.duration = duration;
            digitalWrite(RELAY_PIN, HIGH); // Включаем реле
            Serial.println("Начат длительный звонок.");
        }
    }
}

// Функция отправки логов в Telegram
void sendLog(String message) {
    for (uint8_t i = 0; i < ADMIN_COUNT; i++) {
        bot.sendMessage(adminIDs[i], message, "");
    }
    Serial.println("Лог: " + message);
}

// Функция проверки работы RTC
void checkRTC() {
    static DateTime lastTime = rtc.now();
    DateTime currentTime = rtc.now();
    TimeSpan timeDifference = currentTime - lastTime;
    if (abs(timeDifference.totalseconds()) > 10) {
        // Разница более 10 секунд за последний час
        sendLog("Обнаружена погрешность хода RTC. Проверьте батарею.");
        Serial.println("Обнаружена погрешность хода RTC. Проверьте батарею.");
    }
    lastTime = currentTime;
}

// Функции сохранения и загрузки данных из EEPROM
void saveMainSchedule() {
    EEPROM.put(EEPROM_ADDR_MAIN_WEEKDAY, mainScheduleWeekday);
    EEPROM.put(EEPROM_ADDR_MAIN_SATURDAY, mainScheduleSaturday);
    EEPROM.commit();
    Serial.println("Основные расписания сохранены в EEPROM");
}

void loadMainSchedule() {
    EEPROM.get(EEPROM_ADDR_MAIN_WEEKDAY, mainScheduleWeekday);
    EEPROM.get(EEPROM_ADDR_MAIN_SATURDAY, mainScheduleSaturday);
    Serial.println("Основные расписания загружены из EEPROM");
}

void saveTempSchedule() {
    EEPROM.put(EEPROM_ADDR_TEMP_SCHEDULE, tempSchedule);
    EEPROM.commit();
    Serial.println("Временное расписание сохранено в EEPROM");
}

void loadTempSchedule() {
    EEPROM.get(EEPROM_ADDR_TEMP_SCHEDULE, tempSchedule);
    Serial.println("Временное расписание загружено из EEPROM");
}

void saveHolidays() {
    EEPROM.put(EEPROM_ADDR_HOLIDAY_COUNT, holidayCount);
    EEPROM.put(EEPROM_ADDR_HOLIDAYS, holidays);
    EEPROM.commit();
    Serial.println("Праздники сохранены в EEPROM");
}

void loadHolidays() {
    EEPROM.get(EEPROM_ADDR_HOLIDAY_COUNT, holidayCount);
    EEPROM.get(EEPROM_ADDR_HOLIDAYS, holidays);
    Serial.println("Праздники загружены из EEPROM");
}
  if (millis() - tmr >= 5000) {
   tmr = millis();
   wifiSupport();
  }

}
