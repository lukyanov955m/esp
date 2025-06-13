#include <ESP8266WiFi.h>
#include <WiFiUdp.h>
#include <ESP8266HTTPClient.h>
#include <FS.h>
#include <EEPROM.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h>
#include <ArduinoJson.h>
#include <time.h>

#define EEPROM_SIZE 512
#define WIFI_SSID_ADDR 0
#define WIFI_PASS_ADDR 64
#define BOT_TOKEN_ADDR 128
#define ADMIN_ID_ADDR 256

WiFiUDP udp;
const unsigned int udpPort = 4210;
IPAddress secondIP;
unsigned long lastHello = 0;

WiFiClientSecure tgClient;
UniversalTelegramBot* bot = nullptr;

ESP8266WebServer apServer(80);
ESP8266HTTPUpdateServer httpUpdater;

#define MAX_CARDS 32
// Структура карты доступа
struct Card {
  int number;             // порядковый номер
  String uid;             // UID карты
  String name;            // имя владельца
  unsigned long expire;   // время окончания действия, 0 - постоянно
  bool isTemp;            // временная карта
};
Card cards[MAX_CARDS];
int cardCount = 0;        // количество карт в памяти
const char* cardsFile = "/cards.json";

String currentVersion = "1.0";
// переменные для добавления карты
bool addCardMode = false;
String pendingUID = "";
String pendingName = "";

// ===== EEPROM helpers =====
void getEEPROMString(int addr, char* dest, size_t maxLen, const char* def="") {
  EEPROM.begin(EEPROM_SIZE);
  bool empty = true;
  for (size_t i=0;i<maxLen;i++) {
    dest[i]=EEPROM.read(addr+i);
    if (dest[i]!=0 && dest[i]!=255) empty=false;
  }
  dest[maxLen-1]=0;
  if (empty && def) strncpy(dest, def, maxLen-1);
  EEPROM.end();
}
String getEEPROMString(int addr, size_t maxLen, const char* def="") {
  char buf[130]={0};
  getEEPROMString(addr, buf, maxLen, def);
  return String(buf);
}
void saveEEPROMString(int addr, const char* val, size_t maxLen) {
  EEPROM.begin(EEPROM_SIZE);
  for(size_t i=0;i<maxLen;i++) EEPROM.write(addr+i, (i<strlen(val))?val[i]:0);
  EEPROM.commit();
  EEPROM.end();
}
String getSSID(){return getEEPROMString(WIFI_SSID_ADDR,64,"YOUR_SSID");}
String getPass(){return getEEPROMString(WIFI_PASS_ADDR,64,"YOUR_PASSWORD");}
String getToken(){return getEEPROMString(BOT_TOKEN_ADDR,128,"YOUR_BOT_TOKEN");}
String getAdmin(){return getEEPROMString(ADMIN_ID_ADDR,32,"123456");}

// ===== SPIFFS =====
void loadCards(){
  cardCount=0;
  if(!SPIFFS.exists(cardsFile)) return;
  File f=SPIFFS.open(cardsFile,"r");
  if(!f) return;
  DynamicJsonDocument doc(2048);
  DeserializationError err=deserializeJson(doc,f);
  if(err){f.close();return;}
  for(JsonObject obj: doc.as<JsonArray>()){
    if(cardCount>=MAX_CARDS) break;
    cards[cardCount].number = obj["number"].as<int>();
    cards[cardCount].uid    = obj["uid"].as<String>();
    cards[cardCount].name   = obj["name"].as<String>();
    cards[cardCount].expire = obj["expire"].as<unsigned long>();
    cards[cardCount].isTemp = obj["isTemp"].as<bool>();
    cardCount++;}
  f.close();
}
void saveCards(){
  DynamicJsonDocument doc(2048);
  JsonArray arr=doc.to<JsonArray>();
  for(int i=0;i<cardCount;i++){
    JsonObject o=arr.createNestedObject();
    o["number"] = cards[i].number;
    o["uid"]    = cards[i].uid;
    o["name"]   = cards[i].name;
    o["expire"] = cards[i].expire;
    o["isTemp"] = cards[i].isTemp;
  }
  File f=SPIFFS.open(cardsFile,"w");
  if(f){serializeJson(doc,f);f.close();}
}

// удаляем просроченные временные карты
void removeExpiredCards(){
  unsigned long now = time(nullptr);
  bool changed = false;
  int i = 0;
  while(i < cardCount){
    if(cards[i].isTemp && cards[i].expire > 0 && cards[i].expire <= now){
      for(int j=i;j<cardCount-1;j++) cards[j]=cards[j+1];
      cardCount--; changed = true; continue;
    }
    i++;
  }
  if(changed) saveCards();
}

// ===== UDP discovery =====
void broadcastHello(){
  if(millis()-lastHello>5000){
    udp.beginPacket(IPAddress(255,255,255,255),udpPort);
    udp.write("FIRST-HELLO");
    udp.endPacket();
    lastHello=millis();
  }
}
void listenHello(){
  int ps=udp.parsePacket();
  if(ps){
    char buf[32];
    int len=udp.read(buf,sizeof(buf)-1); if(len>0){ buf[len]=0; if(String(buf)=="SECOND-HELLO") secondIP=udp.remoteIP(); }}
}

// ===== HTTP helpers =====
void sendCards(){
  if(!secondIP) return;
  WiFiClient client; HTTPClient http;
  String url=String("http://")+secondIP.toString()+"/update_cards";
  String body="";
  for(int i=0;i<cardCount;i++){body+=cards[i].uid; if(i<cardCount-1) body+=",";}
  if(http.begin(client,url)){ http.POST(body); http.end(); }
}
String requestCard(){
  if(!secondIP) return "";
  WiFiClient client; HTTPClient http;
  String url=String("http://")+secondIP.toString()+"/request_card";
  if(http.begin(client,url)){ int code=http.GET(); String r=""; if(code==200) r=http.getString(); http.end(); return r; } return ""; }
void openGate(){
  if(!secondIP) return;
  WiFiClient client; HTTPClient http;
  String url=String("http://")+secondIP.toString()+"/command?open=1";
  if(http.begin(client,url)){ http.GET(); http.end(); }
}

// ===== WEB UI =====
void handleRoot(){
  String html="<html><body><h2>Настройка</h2><form method='POST' action='/setup'>";
  html+="SSID: <input name='s' value='"+getSSID()+"'><br>";
  html+="PASS: <input name='p' value='"+getPass()+"'><br>";
  html+="BOT: <input name='b' value='"+getToken()+"'><br>";
  html+="ADMIN: <input name='a' value='"+getAdmin()+"'><br>";
  html+="<input type='submit'></form></body></html>";
  apServer.send(200,"text/html",html);
}
void handleSetup(){
  if(apServer.hasArg("s")) saveEEPROMString(WIFI_SSID_ADDR,apServer.arg("s").c_str(),64);
  if(apServer.hasArg("p")) saveEEPROMString(WIFI_PASS_ADDR,apServer.arg("p").c_str(),64);
  if(apServer.hasArg("b")) saveEEPROMString(BOT_TOKEN_ADDR,apServer.arg("b").c_str(),128);
  if(apServer.hasArg("a")) saveEEPROMString(ADMIN_ID_ADDR,apServer.arg("a").c_str(),32);
  apServer.send(200,"text/html","<html><body>Сохранено. Перезагрузка...</body></html>");
  delay(1000); ESP.restart();
}
void setupWeb(){ apServer.on("/",handleRoot); apServer.on("/setup",HTTP_POST,handleSetup); httpUpdater.setup(&apServer); apServer.begin(); }

// ===== Telegram =====
// вывод списка карт с информацией о сроке действия
void sendCardList(String chat){
  unsigned long now = time(nullptr);
  String msg = "Список карт:\n";
  for(int i=0;i<cardCount;i++){
    msg += String(cards[i].number)+". "+cards[i].name+" ("+cards[i].uid+")\n";
    if(cards[i].isTemp){
      if(cards[i].expire>now){
        unsigned long rem = cards[i].expire-now;
        unsigned long h = rem/3600; unsigned long d = rem/(24*3600);
        if(d>0) msg += "  осталось: " + String(d) + " д.";
        else msg += "  осталось: " + String(h) + " ч.";
      }else msg += "  срок истёк";
    }else msg += "  постоянная";
    msg += "\n";
  }
  bot->sendMessage(chat, msg);
}
void handleTelegram(){
  static unsigned long lastPoll=0; if(millis()-lastPoll<1200) return; lastPoll=millis();
  int n=bot->getUpdates(bot->last_message_received + 1);
  for(int i=0;i<n;i++){
    String chat=bot->messages[i].chat_id;
    String text=bot->messages[i].text;
    // этап ввода имени и срока действия при добавлении карты
    if(addCardMode && chat==getAdmin()){
      if(pendingUID.length()>0 && pendingName==""){
        pendingName=text;
        bot->sendMessage(chat,
          "Выберите срок действия:",
          "[[\"6 часов\",\"12 часов\",\"24 часа\"],[\"3 дня\",\"7 дней\",\"Постоянная\"]]"
        );
        continue;
      }else if(pendingUID.length()>0 && pendingName!=""){
        unsigned long now=time(nullptr); unsigned long exp=0; bool temp=true;
        if(text=="6 часов") exp=now+6*3600;
        else if(text=="12 часов") exp=now+12*3600;
        else if(text=="24 часа") exp=now+24*3600;
        else if(text=="3 дня") exp=now+3*86400;
        else if(text=="7 дней") exp=now+7*86400;
        else if(text=="Постоянная"){ exp=0; temp=false; }
        else { bot->sendMessage(chat,"Неверный формат срока. Повторите."); continue; }
        int next=1; for(int j=0;j<cardCount;j++) if(cards[j].number>=next) next=cards[j].number+1;
        if(cardCount<MAX_CARDS){
          cards[cardCount++]={next,pendingUID,pendingName,exp,temp};
          saveCards(); sendCards();
          bot->sendMessage(chat,"Карта добавлена! UID: "+pendingUID);
        } else bot->sendMessage(chat,"Лимит карт заполнен");
        addCardMode=false; pendingUID=""; pendingName="";
        continue;
      }
    }
    if(text=="/open" || text=="Открыть ворота"){ openGate(); bot->sendMessage(chat,"Ворота открыты"); }
    else if(text=="/cardlist" || text=="Список карт"){ sendCardList(chat); }
    else if(text.startsWith("Удалитькарту-")){ int num=text.substring(text.indexOf('-')+1).toInt(); for(int j=0;j<cardCount;j++) if(cards[j].number==num){ for(int k=j;k<cardCount-1;k++) cards[k]=cards[k+1]; cardCount--; saveCards(); sendCards(); bot->sendMessage(chat,"Карта удалена"); break; } }
    else if(text=="/addcard" || text=="Добавить карту"){ addCardMode=true; pendingUID=requestCard(); if(pendingUID.length()){ bot->sendMessage(chat,"Карта считана. UID: `"+pendingUID+"`\nВведите имя владельца:","Markdown"); } else { bot->sendMessage(chat,"Не удалось считать карту"); addCardMode=false; } }
    else if(text=="/status" || text=="Статус"){ bot->sendMessage(chat,"Версия "+currentVersion); }
    else bot->sendMessage(chat,"Неизвестная команда");
  }
}

void connectWiFi(){
  WiFi.mode(WIFI_STA); WiFi.begin(getSSID().c_str(), getPass().c_str());
  unsigned long start=millis(); while(WiFi.status()!=WL_CONNECTED && millis()-start<15000){ delay(500); }
}

void setup(){
  Serial.begin(115200); SPIFFS.begin(); EEPROM.begin(EEPROM_SIZE);
  setupWeb();
  connectWiFi();
  udp.begin(udpPort);
  tgClient.setInsecure();
  bot=new UniversalTelegramBot(getToken(), tgClient);
  configTime(5*3600,0,"pool.ntp.org","time.nist.gov");
  loadCards();
}

void loop(){
  apServer.handleClient();
  broadcastHello();
  listenHello();
  handleTelegram();
  static unsigned long lastExp = 0;
  if(millis()-lastExp > 60000){ lastExp = millis(); removeExpiredCards(); }
  delay(5);
}
