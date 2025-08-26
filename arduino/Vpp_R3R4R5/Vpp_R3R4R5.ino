#include <SoftwareSerial.h>
#include <WiFiEsp.h>
#include <WiFiEspClient.h>
#include <ArduinoHttpClient.h>
#include <ArduinoJson.h>

char ssid[] = "spreatics_eungam_cctv";
char password[] = "spreatics*";
char server[] = "52.63.106.255";
int port = 5001;

SoftwareSerial espSerial(2, 3); // RX, TX

#define R3_V1 A0
#define R3_V2 A1
#define R4_V1 A2
#define R4_V2 A3
#define R5_V1 A4
#define R5_V2 A5

float resistance = 330.0;

bool relayStatus[6] = {false, false, false, false, false, false};  // relay_id 1~5용 상태

unsigned long lastPostTime = 0;
unsigned long lastGetTime = 0;
const unsigned long postInterval = 2000;
const unsigned long getInterval = 30000;
unsigned long lastSOCUpdateTime = 0;

void setup() {
  Serial.begin(9600);
  espSerial.begin(9600);
  WiFi.init(&espSerial);

  Serial.println("📡 WiFi 연결 중...");
  while (WiFi.begin(ssid, password) != WL_CONNECTED) {
    Serial.println("❌ 연결 실패 → 재시도 중...");
    delay(3000);
  }
  Serial.println("✅ WiFi 연결 성공");

  fetchRelayCommands();  // 첫 실행 시에도 상태 가져오기
}

void loop() {
  unsigned long now = millis();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("📶 WiFi 끊김 감지 → 재연결 시도...");
    WiFi.disconnect();
    WiFi.begin(ssid, password);
    return;
  }

  if (now - lastGetTime >= getInterval) {
    lastGetTime = now;
    fetchRelayCommands();
  }

  if (now - lastPostTime >= postInterval) {
    lastPostTime = now;

    if (relayStatus[3]) {
      float p3 = measurePower(R3_V1, R3_V2, resistance);
      sendStatus(3, p3);
      delay(500);
    }

    if (relayStatus[4]) {
      float p4 = measurePower(R4_V1, R4_V2, resistance);
      sendStatus(4, p4);
      delay(500);
    }

    if (relayStatus[5]) {
      float p5 = measurePower(R5_V1, R5_V2, resistance);
      sendStatus(5, p5);
    }
  }
  // 1초마다 SOC만 계산 (전송은 하지 않음)
  if (now - lastSOCUpdateTime >= 1000) {
  lastSOCUpdateTime = now;

    if (relayStatus[3]) {
      float p3 = measurePower(R3_V1, R3_V2, resistance);
      updateSOC(3, p3, 1.0);  // 1초간 전력
    }
    if (relayStatus[4]) {
      float p4 = measurePower(R4_V1, R4_V2, resistance);
      updateSOC(4, p4, 1.0);
    }
    if (relayStatus[5]) {
      float p5 = measurePower(R5_V1, R5_V2, resistance);
      updateSOC(5, p5, 1.0);
    }
}
  delay(100);
}

float measurePower(int pinV1, int pinV2, float R) {
  float V1 = analogRead(pinV1) * (5.0 / 1023.0);
  float V2 = analogRead(pinV2) * (5.0 / 1023.0);
  float Vdiff = V1 - V2;
  float current_mA = (Vdiff / R) ;
  if (current_mA < 0) current_mA = 0.0;
  float power = (current_mA) * 5.0;
  return power;
}

float soc = 50.0;
const float MAX_SOC = 100.0;
const float MIN_SOC = 0.0;

// 정격용량 3.6kWh 가정
void updateSOC(int relay_id, float power_kw, float duration_sec) {
  float delta = (power_kw * duration_sec / 36) * 1000.0;

  if (relay_id == 3 || relay_id == 4) {
    soc += delta;  // 충전
  } else if (relay_id == 5) {
    soc -= delta;  // 방전
  }

  // 범위 제한
  if (soc > MAX_SOC) soc = MAX_SOC;
  if (soc < MIN_SOC) soc = MIN_SOC;

  Serial.print("🔋 SOC 업데이트 (릴레이 ");
  Serial.print(relay_id);
  Serial.print("): ");
  Serial.println(soc);
}

void sendStatus(int relay_id, float power_kw) {
  DynamicJsonDocument doc(256);
  doc["relay_id"] = relay_id;
  doc["power_kw"] = static_cast<float>(power_kw);
  doc["soc"] = soc;

  WiFiEspClient localClient;
  HttpClient client(localClient, server, port);

  client.beginRequest();
  client.post("/ardu_serv/node_status");
  client.sendHeader("Content-Type", "application/json");
  client.sendHeader("Connection", "close");
  client.sendHeader("Content-Length", measureJson(doc));
  client.beginBody();
  serializeJson(doc, client);
  client.endRequest();

  int statusCode = client.responseStatusCode();
  Serial.print("📨 응답 코드: ");
  Serial.println(statusCode);

  if (statusCode > 0) {
    String response = client.responseBody();
    Serial.println("응답 본문 ↓");
    Serial.println(response);
  } else {
    Serial.println("응답 실패");
  }

  client.stop();
  delay(500);
}

void fetchRelayCommands() {
  Serial.println("🌐 서버에서 명령어 요청 중...");
  WiFiEspClient localClient;
  HttpClient getHttp(localClient, server, port);
  getHttp.get("/serv_ardu/command");

  int statusCode = getHttp.responseStatusCode();
  if (statusCode != 200) {
    Serial.print("❌ 응답 오류: ");
    Serial.println(statusCode);
    getHttp.stop();
    return;
  }

  getHttp.skipResponseHeaders();
  String response = getHttp.responseBody();
  getHttp.stop();

  Serial.println("📥 릴레이 명령 응답 ↓");
  Serial.println(response);

  for (int id = 3; id <= 5; id++) {
    String target = "\"relay_id\": " + String(id);
    int relayIndex = response.indexOf(target);
    if (relayIndex == -1) {
      relayStatus[id] = false;
      continue;
    }

    int statusIndex = response.indexOf("\"status\":", relayIndex);
    if (statusIndex == -1) {
      relayStatus[id] = false;
      continue;
    }

    int valueStart = response.indexOf(":", statusIndex) + 1;
    while (response.charAt(valueStart) == ' ') valueStart++;
    char statusChar = response.charAt(valueStart);
    relayStatus[id] = (statusChar == '1');
  }
}
