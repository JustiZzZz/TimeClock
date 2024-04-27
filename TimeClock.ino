#include "RTClib.h"        // Библиотека для работы с часами 
#include <TM1637Display.h> // Библиотека для дисплея
#include <Wire.h>          //I2C библиотека
#include <RtcDS3231.h>     //RTC библиотека
#include <GyverPortal.h>   // Библиотека для реализации интерфейсв
#include <LittleFS.h>      // Работа с прошивкой через интернет
#include <GyverButton.h>   // Библиотека для кнопки
#include <string.h>
// Определяем пины и WiFi
#define CLK 12
#define DIO 14
#define PIN_RELAY 13
#define AP_SSID "tyu" // Название сети WiFi
#define AP_PASS "56781234"         // Пароль от сети Wifi
GButton BUTTON_PIN(D3);            // Пин кнопки
// Параметры сети
IPAddress staticIP(192, 168, 1, 201);   // Первые три числа в соответствии с сетью, четвёртое желаемое
IPAddress gateway(192, 168, 1, 1);    // Основной шлюз
IPAddress subnet(255, 255, 255, 0);       // Маска подсети
IPAddress dns1(192, 168, 1, 201);       // То же самое что и статический айпи
IPAddress dns2(8, 8, 8, 8);  

boolean butt = false;  // Флаг для режима

// Создаём объекты
RTC_DS3231 rtc;
TM1637Display display = TM1637Display(D5, D6);
RtcDS3231<TwoWire> rtcObject(Wire);
GyverPortal ui(&LittleFS); // для проверки файлов

// Переменные для времени
int currentTime;
int displaytime;
int DayOfTheWeek;

GPtime startTime;
GPtime stopTime;
GPtime nowTime;
GPtime tirm
int Calls1[14] {830, 915, 930, 1015, 1030, 1115, 1135, 1220, 1240, 1325, 1340, 1425, 1445, 1633}; // Звонки будние дни
int Calls2[8] {830, 915, 920, 1005, 1010, 1055, 1145, 1210};                                      // Звонки суббота

void wifiSupport() {         // Поддержка подключения к вайфа.
  if (WiFi.status() != WL_CONNECTED) {
    // Подключаемся к Wi-Fi
    Serial.print("Попытка подключения к ");
    Serial.print(AP_SSID);
    Serial.print(":");
    WiFi.mode(WIFI_STA);
    if (WiFi.config(staticIP, gateway, subnet, dns1, dns2) == false) {     
      Serial.println("wifi config failed.");
      return;
    }
    WiFi.begin(AP_SSID, AP_PASS);
    uint8_t trycon = 0;
    while (WiFi.status() != WL_CONNECTED) {
      if (trycon++ < 30) delay(500);
      else {
        Serial.print("Нет подключения к Wi-Fi! ESP8266 перезагружается сейчас!");
        delay(1000);
        ESP.restart();             
      }
    }
    Serial.println("Подключено. \nIP: ");

    // Выводим IP ESP8266
    Serial.println(WiFi.localIP());
  }
}

void build() {
  GP.PAGE_TITLE("Звонки");
  GP.BUILD_BEGIN();
  GP.THEME(GP_DARK);
  GP.TITLE("Автоматическая система звонков", "t1");
  GP.HR();
  GP.UPDATE("tirm")
  // кнопка отправляет текст из поля txt
  GP.BLOCK_BEGIN(GP_THIN, "100%", "Общая информация");
  GP.BOX_BEGIN(GP_CENTER); GP.LABEL("1 Урок: ", "tt", GP_DEFAULT, 15);
  GP.TIME("tirm", "", true)   GP.BOX_END();  GP.BREAK();
  GP.BLOCK_END(); 
  GP.BLOCK_BEGIN(GP_THIN, "100%", "Настраиваемое расписание");
  GP.SPAN("Начало урока ㅤㅤ Конец урокаㅤ", GP_RIGHT); 
  GP.BOX_BEGIN(GP_CENTER); GP.LABEL("1 Урок: ", "tt", GP_DEFAULT, 15);
  GP.NUMBER("aaaaaa", "", 42, "77px");
  GP.TIME("stopTime", stopTime, true);     GP.BOX_END();  GP.BREAK();
  GP.BOX_BEGIN(GP_CENTER); GP.LABEL("1 Перемена: ", "tt", GP_DEFAULT, 15);
  GP.NUMBER("aaaaaa");
  GP.TIME("stopTime", stopTime, true);     GP.BOX_END();  GP.BREAK();
  GP.BOX_BEGIN(GP_CENTER); GP.LABEL("2 Урок: ", "tt", GP_DEFAULT, 15);
  GP.NUMBER("aaaaaa");
  GP.TIME("stopTime", stopTime, true);     GP.BOX_END();  GP.BREAK();
  GP.BOX_BEGIN(GP_CENTER); GP.LABEL("2 Перемена: ", "tt", GP_DEFAULT, 15);
  GP.NUMBER("aaaaaa");
  GP.TIME("stopTime", stopTime, true);     GP.BOX_END();  GP.BREAK();
}

// Первичная настройка параметров микроконтроллера и датчиков
void setup() {
  Serial.begin(115200);
  pinMode(PIN_RELAY, OUTPUT);     // Объявляем пин реле как выход
  digitalWrite(PIN_RELAY, HIGH);  // Выключаем реле - посылаем высокий сигнал
  rtcObject.Begin();     //Запуск I2C

  // подключаем конструктор и запускаем
  ui.attachBuild(build);
  ui.start();
  ui.attach(action);
  ui.log.start(30);   // размер буфера
  ui.enableOTA();   // без пароля
  //ui.enableOTA("admin", "pass");  // с паролем

  if (!LittleFS.begin()) Serial.println("FS Error");
  ui.downloadAuto(true);

  // Смотрим подключены ли часы 
  if (! rtc.begin()) {
    Serial.println("Couldn't find RTC");
    while (1);
  }

  // Если часы отключатся от питания:
  if (rtc.lostPower()) {
    Serial.println("Нет питания, установите время!");
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__))); // Устанавливаем время посредством переменных
    //rtc.adjust(DateTime(2024, 2, 4, 12, 7, 0));   // Или введя вручную
  }

  display.setBrightness(3); // Яркость
  display.clear();          // Очистка дисплея
}

void workweek() {
    static unsigned long lastRingTime = 0;
    static bool isRinging = false;
    unsigned long currentMillis = millis();

    // Определяем массив звонков в зависимости от дня недели
    int* calls;
    int callsSize;
    if (DayOfTheWeek > 0 && DayOfTheWeek < 6) {
        calls = Calls1;
        callsSize = 14;
    } else if (DayOfTheWeek == 6) {
        calls = Calls2;
        callsSize = 8;
    } else {
        return; // Нет звонков в воскресенье
    }

    // Проверяем, есть ли звонок
    for (int i = 0; i < callsSize; i++) {
        if (displaytime == calls[i] && currentTime <= 5) {
            // Начало звонка
            if (!isRinging) {
                digitalWrite(PIN_RELAY, LOW); // Начинаем звонок
                lastRingTime = currentMillis;
                isRinging = true;
            }
        }
    }

    // Остановка звонка
    if (isRinging && currentMillis - lastRingTime >= 7000) {
    digitalWrite(PIN_RELAY, HIGH); // Останавливаем звонок
        isRinging = false;
    }
}

void tempo() {

}

void nothing() {

}

void action() {
  if (portal.update()) {
    ("tirm") = 
  }

// Основной цикл 
void loop() {
 BUTTON_PIN.tick();                            // Опрос кнопки
 if (BUTTON_PIN.isPress()) { butt = !butt; }   // Смена флага после нажатия кнопки

 DateTime now = rtc.now();                        // Получаем текущую дату и время 
 currentTime = now.second();                      // Секунды
 displaytime = (now.hour() * 100) + now.minute(); // Часы и минуты в int для дисплея 
 DayOfTheWeek = now.dayOfTheWeek();               // Получаем текущий день недели

 display.showNumberDecEx(displaytime, 0b11100000, true); // Вывод времени, ведущие нули

 if (!butt) {   // Включение нужного режима в зависимости от флага кнопки/диода
   workweek();
 }
 else if (butt) {
   tempo();
   butt = !butt;            // Выполнение режима единожды
 }

  ui.tick();
  static uint32_t tmr;
  if (millis() - tmr >= 5000) {
   tmr = millis();
   wifiSupport();
  }

}