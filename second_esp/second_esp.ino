#include <ESP8266WiFi.h>
#include <WiFiUdp.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPClient.h>
#include <FS.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include <MFRC522.h>

#define RST_PIN 0
#define SS_PIN 4
#define RELAY_PIN 15
#define BUZZER_PIN 5
#define RELAY_OPEN_TIME 2000
#define BUZZER_TONE_OK 1500
#define BUZZER_TONE_ERR 400
#define BEEP_DURATION 200
#define BEEP_PAUSE 120
#define BEEP_OK_COUNT 3
#define BEEP_ERR_COUNT 5

WiFiUDP udp;
const unsigned int udpPort=4210;
IPAddress firstIP;
unsigned long lastHello=0;

ESP8266WebServer server(80);

MFRC522 mfrc522(SS_PIN,RST_PIN);

struct Card{String uid;};
#define MAX_CARDS 32
String cards[MAX_CARDS];
int cardCount=0;
const char* cardsFile="/cards.json";

// ===== SPIFFS =====
void loadCards(){
  cardCount=0;
  if(!SPIFFS.exists(cardsFile)) return;
  File f=SPIFFS.open(cardsFile,"r"); if(!f) return;
  DynamicJsonDocument doc(2048);
  DeserializationError err=deserializeJson(doc,f);
  if(err){f.close();return;}
  for(String uid: doc.as<JsonArray>()){ if(cardCount<MAX_CARDS) cards[cardCount++]=uid; }
  f.close();
}
void saveCards(){
  DynamicJsonDocument doc(2048);
  JsonArray arr=doc.to<JsonArray>();
  for(int i=0;i<cardCount;i++) arr.add(cards[i]);
  File f=SPIFFS.open(cardsFile,"w"); if(f){serializeJson(doc,f); f.close();}
}

// ===== UDP =====
void broadcastHello(){
  if(millis()-lastHello>5000){
    udp.beginPacket(IPAddress(255,255,255,255),udpPort);
    udp.write("SECOND-HELLO");
    udp.endPacket();
    lastHello=millis();
  }
}
void listenHello(){
  int ps=udp.parsePacket();
  if(ps){ char buf[32]; int len=udp.read(buf,sizeof(buf)-1); if(len>0){ buf[len]=0; if(String(buf)=="FIRST-HELLO") firstIP=udp.remoteIP(); }}
}

// ===== Hardware =====
void beepMultiple(int toneVal, int count){
  for(int i=0;i<count;i++){
    tone(BUZZER_PIN, toneVal, BEEP_DURATION);
    delay(BEEP_DURATION);
    noTone(BUZZER_PIN);
    delay(BEEP_PAUSE);
  }
}

void openGate(){
  digitalWrite(RELAY_PIN,LOW);
  beepMultiple(BUZZER_TONE_OK, BEEP_OK_COUNT);
  delay(RELAY_OPEN_TIME);
  digitalWrite(RELAY_PIN,HIGH);
}
String readCard(){
  if(!mfrc522.PICC_IsNewCardPresent() || !mfrc522.PICC_ReadCardSerial()) return "";
  char buf[16];
  for(byte i=0;i<mfrc522.uid.size;i++) sprintf(buf+i*2, "%02X", mfrc522.uid.uidByte[i]);
  String uid=String(buf);
  mfrc522.PICC_HaltA(); mfrc522.PCD_StopCrypto1();
  return uid;
}

// ===== HTTP handlers =====
void handleCommand(){
  if(server.arg("open")=="1") openGate();
  server.send(200,"text/plain","OK");
}
void handleUpdate(){
  String body=server.arg("plain");
  cardCount=0;
  int start=0;
  while(start<body.length() && cardCount<MAX_CARDS){
    int comma=body.indexOf(',',start); if(comma==-1) comma=body.length();
    cards[cardCount++]=body.substring(start,comma);
    start=comma+1;
  }
  saveCards();
  server.send(200,"text/plain","UPDATED");
}
void handleRequestCard(){
  unsigned long start=millis();
  String uid="";
  while(millis()-start<10000 && uid==""){
    uid=readCard();
    delay(100);
  }
  if(uid!="") beepMultiple(BUZZER_TONE_OK,1);
  server.send(200,"text/plain",uid);
}

void handleReboot(){
  server.send(200,"text/plain","REBOOTING");
  delay(500);
  ESP.restart();
}

// ===== Main card check =====
void checkCard(){
  String uid=readCard();
  if(uid=="") return;
  for(int i=0;i<cardCount;i++) if(cards[i]==uid){ openGate(); return; }
  // если карта не найдена
  beepMultiple(BUZZER_TONE_ERR, BEEP_ERR_COUNT);
}

void setup(){
  Serial.begin(115200);
  SPIFFS.begin();
  WiFi.mode(WIFI_STA);
  WiFi.begin("YOUR_SSID","YOUR_PASSWORD");
  while(WiFi.status()!=WL_CONNECTED) { delay(500); }
  udp.begin(udpPort);
  SPI.begin(); mfrc522.PCD_Init();
  pinMode(RELAY_PIN,OUTPUT); digitalWrite(RELAY_PIN,HIGH);
  pinMode(BUZZER_PIN,OUTPUT); digitalWrite(BUZZER_PIN,LOW);
  server.on("/command",handleCommand);
  server.on("/update_cards",HTTP_POST,handleUpdate);
  server.on("/request_card",handleRequestCard);
  server.on("/reboot",handleReboot);
  server.begin();
  loadCards();
}

void loop(){
  server.handleClient();
  broadcastHello();
  listenHello();
  checkCard();
  delay(5);
}
