#include <SoftwareSerial.h>
#include <WiFiEsp.h>
#include <WiFiEspClient.h>
#include <ArduinoHttpClient.h>
#include <ArduinoJson.h>

// 📡 WiFi 및 서버 설정
char ssid[] = "spreatics_eungam_cctv";
char password[] = "spreatics*";
char server[] = "52.63.106.255";
int port = 5001;

// 📡 ESP-01 통신 설정
SoftwareSerial espSerial(2, 3); // RX, TX
WiFiEspClient WiFiClient;

// 🔌 릴레이 핀 (1~5)
const int relayPins[5] = {4, 5, 6, 7, 8};
bool relayStatus[6] = {false, false, false, false, false, false};  // 1~5 릴레이 상태 저장

// 🔍 릴레이 1, 2 회로 측정용 아날로그 핀
#define LED1_V1 A0
#define LED1_V2 A1
#define LED2_V1 A2
#define LED2_V2 A3
float resistance1 = 330.0;
float resistance2 = 330.0;

// ⏱️ 타이밍 제어
unsigned long lastGetTime = 0;
unsigned long lastPostTime = 0;
unsigned long lastWifiCheck = 0;
const unsigned long interval = 3000;
const unsigned long wifiCheckInterval = 30000;

void setup() {
  Serial.begin(9600);
  espSerial.begin(9600);
  WiFi.init(&espSerial);

  for (int i = 0; i < 5; i++) {
    pinMode(relayPins[i], OUTPUT);
    digitalWrite(relayPins[i], HIGH); // 릴레이 OFF (LOW가 ON이므로)
  }

  Serial.println("📡 WiFi 연결 중...");
  while (WiFi.begin(ssid, password) != WL_CONNECTED) {
    Serial.println("❌ 연결 실패 → 재시도 중...");
    delay(3000);
  }
  Serial.println("✅ WiFi 연결 성공");
  fetchRelayCommands();
  }


//  전력 측정 함수 (V=IR, P=VI)
float measurePower(int pinV1, int pinV2, float R) {
  float V1 = analogRead(pinV1) * (5.0 / 1023.0);
  float V2 = analogRead(pinV2) * (5.0 / 1023.0);
  float Vdiff = V1 - V2;
  float current_mA = (Vdiff / R) * 1000.0;
  float power = (current_mA / 1000.0) * 5.0; // 5V 기준
  return power;
}

// 릴레이 명령 요청 및 제어
void fetchRelayCommands() {
  Serial.println("🌐 서버에서 명령어 요청 중...");
  
  HttpClient getHttp(WiFiClient, server, port);
  getHttp.get("/serv_ardu/command");

  int statusCode = getHttp.responseStatusCode();
  if (statusCode != 200) {
    Serial.print("응답 오류: ");
    Serial.println(statusCode);
    getHttp.stop();
    return;
  }

  getHttp.skipResponseHeaders();
  String response = getHttp.responseBody();
  getHttp.stop();


  // 릴레이 1~5 추출 및 제어
  for (int id = 1; id <= 5; id++) {
    String target = "\"relay_id\": " + String(id);
    int relayIndex = response.indexOf(target);

    if (relayIndex == -1) {
      Serial.print("릴레이 ");
      Serial.print(id);
      Serial.println(" 정보 없음");
      continue;
    }

    int statusIndex = response.indexOf("\"status\":", relayIndex);
    if (statusIndex == -1) {
      Serial.print(" 상태값 없음 for 릴레이 ");
      Serial.println(id);
      continue;
    }

    // 👇 공백 스킵 후 상태 숫자 추출
    int valueStart = response.indexOf(":", statusIndex) + 1;
    while (response.charAt(valueStart) == ' ') valueStart++;
    char statusChar = response.charAt(valueStart);
    int status = statusChar - '0';

    digitalWrite(relayPins[id - 1], status == 1 ? LOW : HIGH);
    relayStatus[id] = (status == 1);  // 릴레이 상태 저장

    Serial.print("릴레이 ");
    Serial.print(id);
    Serial.print(" → ");
    Serial.println(status == 1 ? "ON" : "OFF");
  }
}



//  측정값 서버에 POST 전송
void sendStatus(int relay_id, float power_kw) {
  if (power_kw < 0) power_kw = 0.0;

  DynamicJsonDocument doc(256);
  doc["relay_id"] = relay_id;
  doc["power_kw"] = power_kw;
  doc["soc"] = nullptr;

  WiFiEspClient localClient;
  HttpClient client(localClient, server, port);

  client.beginRequest();
  client.post("/ardu_serv/node_status");
  client.sendHeader("Content-Type", "application/json");
  client.sendHeader("Connection", "close");
  client.sendHeader("Content-Length", measureJson(doc));  
  client.beginBody();
  serializeJson(doc, client);  // 
  client.endRequest();

  int statusCode = client.responseStatusCode();
  Serial.print("📨 응답 코드: ");
  Serial.println(statusCode);

  if (statusCode > 0) {
    String response = client.responseBody();
  } else {
    Serial.println(" 응답 실패");
  }

  client.stop();
  delay(500);  // ✅ 소켓 정리 시간 확보
}

void loop() {
  unsigned long now = millis();

  // 🔄 WiFi 상태 감시 (10초마다 확인)
  //if (now - lastWifiCheck >= wifiCheckInterval) {
  //  lastWifiCheck = now;

    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("📶 WiFi 끊김 감지 → 재연결 시도...");
      WiFi.disconnect();
      WiFi.begin(ssid, password);
      return;
    }
  //}

  // 🔁 릴레이 명령 주기적 요청
  if (now - lastGetTime >= 30000) {
    lastGetTime = now;
    fetchRelayCommands();
  }

  // 📤 상태 전송 주기적 수행
if (now - lastPostTime >= 2000) {
  lastPostTime = now;

  if (relayStatus[1]) {
    float p1 = measurePower(LED1_V1, LED1_V2, resistance1);
    sendStatus(1, p1);
    delay(500);
  }

  if (relayStatus[2]) {
    float p2 = measurePower(LED2_V1, LED2_V2, resistance2);
    sendStatus(2, p2);
    delay(500);
  }
 
}
  delay(100);
}