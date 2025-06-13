#include <ESP8266WiFi.h>
// === Архитектура ===
// Первая плата хранит список карт и общается с Telegram.
// Через UDP она ищет вторую плату и запоминает её IP.
// По HTTP отправляет ей команды открытия ворот и актуальные списки карт.
// Вторая плата занимается реле и считывателем RFID.
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
#include "commands.h"

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
const char* WEB_USER="admin";
const char* WEB_PASS="1234";
const char* AP_NAME="SetupESP8266"; // имя точки доступа

#define MAX_CARDS 32
#define MAX_USERS 8
#define MAX_PENDING 4

// ===== Структуры =====
struct User {
  String id;             
  String name;           
  String role;           
  unsigned long expire;  
};
struct Pending {
  String id;
  String name;
};

struct Card {
  int number;            
  String uid;            
  String name;           
  unsigned long expire;  
  bool isTemp;           
};
Card cards[MAX_CARDS];
int cardCount = 0;        
const char* cardsFile = "/cards.json";

User users[MAX_USERS];
int userCount = 0;
Pending pendings[MAX_PENDING];
int pendingCount = 0;
const char* usersFile = "/users.json";

String currentVersion = "1.0";
String previousVersion = "1.0";
String rebootConfirmChat = "";
unsigned long rebootRequestTime = 0;
// переменные для добавления карты
bool addCardMode = false;
String pendingUID = "";
String pendingName = "";

// === Индикация состояния ===
const int LED_PIN = LED_BUILTIN; 
unsigned long ledTimer = 0;
bool ledState = false;

// ===== Флаг наличия сети =====
bool networkReady = false;

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
  size_t len = strlen(val);
  if(len > maxLen - 1) len = maxLen - 1;
  for(size_t i=0;i<maxLen;i++)
    EEPROM.write(addr+i, (i<len)?val[i]:0);
  EEPROM.commit();
  EEPROM.end();
}
String getSSID(){return getEEPROMString(WIFI_SSID_ADDR,64,"YOUR_SSID");}
String getPass(){return getEEPROMString(WIFI_PASS_ADDR,64,"YOUR_PASSWORD");}
String getToken(){return getEEPROMString(BOT_TOKEN_ADDR,128,"YOUR_BOT_TOKEN");}
String getAdmin(){return getEEPROMString(ADMIN_ID_ADDR,32,"123456789");}

Cmd parseCmd(const String &t){
  if(t=="/open" || t=="Открыть ворота") return CMD_OPEN;
  if(t=="/cardlist" || t=="Список карт") return CMD_CARDLIST;
  if(t=="/addcard" || t=="Добавить карту" || t=="Добавитькарту") return CMD_ADD;
  if(t=="/users" || t=="Пользователи") return CMD_USERS;
  if(t=="/request" || t=="Запросить доступ") return CMD_REQUEST;
  if(t=="/help" || t=="Помощь") return CMD_HELP;
  if(t=="/status" || t=="Статус") return CMD_STATUS;
  if(t=="/reboot" || t=="Перезапуск") return CMD_REBOOT;
  if(t.startsWith("Удалитькарту-")) return CMD_DELETE_CARD;
  if(t.startsWith("/grant-")) return CMD_GRANT;
  if(t.startsWith("/reject-")) return CMD_REJECT;
  if(t.startsWith("Удалить-")) return CMD_GRANT;
  return CMD_NONE;
}

// ===== SPIFFS =====
void loadCards(){
  cardCount=0;
  if(!SPIFFS.exists(cardsFile)) {
    Serial.println("[SPIFFS] Файл карт не найден, создаю новый");
    File nf = SPIFFS.open(cardsFile, "w");
    if(nf){ nf.print("[]"); nf.close(); }
    cardCount = 0;
    return;
  }
  File f=SPIFFS.open(cardsFile,"r");
  if(!f){
    Serial.println("[SPIFFS] Не удалось открыть файл карт");
    return;
  }
  if(f.size()>2048){
    Serial.println("[SPIFFS] Файл карт слишком большой");
    f.close();
    return;
  }
  DynamicJsonDocument doc(2048);
  DeserializationError err=deserializeJson(doc,f);
  if(err){
    Serial.println("[SPIFFS] Ошибка разбора JSON карт");
    f.close();
    return;
  }
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
bool saveCards(){
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
  if(!f){
    Serial.println("[SPIFFS] Не удалось открыть файл для записи карт");
    return false;
  }
  if(serializeJson(doc,f)==0){
    Serial.println("[SPIFFS] Ошибка записи файла карт");
    f.close();
    return false;
  }
  f.close();
  return true;
}
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

// ===== Работа с пользователями =====
int findUserIndex(String id){
  for(int i=0;i<userCount;i++) if(users[i].id==id) return i;
  return -1;
}
int findPendingIndex(String id){
  for(int i=0;i<pendingCount;i++) if(pendings[i].id==id) return i;
  return -1;
}

void saveUsers(){
  DynamicJsonDocument doc(1024);
  JsonArray arr = doc.to<JsonArray>();
  for(int i=0;i<userCount;i++){
    JsonObject o=arr.createNestedObject();
    o["id"]=users[i].id;
    o["name"]=users[i].name;
    o["role"]=users[i].role;
    o["expire"]=users[i].expire;
  }
  File f=SPIFFS.open(usersFile,"w");
  if(f){serializeJson(doc,f);f.close();}
}
void loadUsers(){
  userCount=0;
  if(!SPIFFS.exists(usersFile)){
    Serial.println("[SPIFFS] Файл пользователей не найден");
    return;
  }
  File f=SPIFFS.open(usersFile,"r");
  if(!f){
    Serial.println("[SPIFFS] Не удалось открыть файл пользователей");
    return;
  }
  if(f.size()>2048){
    Serial.println("[SPIFFS] Файл пользователей слишком большой");
    f.close();
    return;
  }
  DynamicJsonDocument doc(1024); DeserializationError err=deserializeJson(doc,f);
  if(err){
    Serial.println("[SPIFFS] Ошибка разбора JSON пользователей");
    f.close();
    return;
  }
  for(JsonObject o: doc.as<JsonArray>()){
    if(userCount>=MAX_USERS) break;
    users[userCount].id=o["id"].as<String>();
    users[userCount].name=o["name"].as<String>();
    users[userCount].role=o["role"].as<String>();
    users[userCount].expire=o["expire"].as<unsigned long>();
    userCount++;
  }
  f.close();
}
void ensureMainAdmin(){
  if(findUserIndex(getAdmin())==-1 && userCount<MAX_USERS){
    users[userCount].id=getAdmin();
    users[userCount].name="Главный админ";
    users[userCount].role="admin";
    users[userCount].expire=0;
    userCount++;
    saveUsers();
  }
}
void sendToAllAdmins(String msg){
  if (!bot) return;
  for(int i=0;i<userCount;i++) if(users[i].role=="admin")
    bot->sendMessage(users[i].id,msg);
}
void notifyAdminsReboot(){
  if (!bot) return;
  sendToAllAdmins("⚠️ Система была перезагружена (" + getCurrentTimeStr() + ")");
}
String getCurrentTimeStr(){
  time_t now = time(nullptr);
  struct tm* ti = localtime(&now);
  char buf[24];
  strftime(buf, sizeof(buf), "%d-%m-%Y %H:%M", ti);
  return String(buf);
}
void requestAccess(String id, String name){
  if (!bot) return;
  if(findPendingIndex(id)!=-1) return;
  if(pendingCount<MAX_PENDING){
    pendings[pendingCount].id=id;
    pendings[pendingCount].name=name;
    pendingCount++;
  }
  String msg="Пользователь "+name+" ("+id+") просит доступ.";
  msg+="\nДля выдачи: /grant-"+id+"-guest-6 или /grant-"+id+"-admin";
  sendToAllAdmins(msg);
  bot->sendMessage(id,"Запрос отправлен администраторам.");
}
void approveRequest(String id, String role, int hours){
  if (!bot) return;
  int p=findPendingIndex(id);
  if(p!=-1){
    for(int j=p;j<pendingCount-1;j++) pendings[j]=pendings[j+1];
    pendingCount--;
  }
  int idx=findUserIndex(id);
  if(idx==-1 && userCount<MAX_USERS){
    idx=userCount++;
  }
  users[idx].id=id;
  users[idx].name="User";
  users[idx].role=role;
  if(role=="guest" && hours>0) users[idx].expire=time(nullptr)+hours*3600;
  else users[idx].expire=0;
  saveUsers();
  bot->sendMessage(id,"Вам выдан доступ: "+role);
  sendToAllAdmins("Пользователь "+id+" получил права "+role);
}
void rejectRequest(String id){
  if (!bot) return;
  int p=findPendingIndex(id);
  if(p!=-1){
    for(int j=p;j<pendingCount-1;j++) pendings[j]=pendings[j+1];
    pendingCount--;
  }
  bot->sendMessage(id,"Ваш запрос отклонён.");
}
void removeUserById(String id){
  int idx=findUserIndex(id);
  if(idx==-1) return;
  for(int i=idx;i<userCount-1;i++) users[i]=users[i+1];
  userCount--; saveUsers();
}
void sendUserList(String chat){
  if (!bot) return;
  String msg="Пользователи:\n";
  for(int i=0;i<userCount;i++){
    msg+=users[i].id+" - "+users[i].role+"\n";
  }
  bot->sendMessage(chat,msg);
}
void checkExpiredUsers(){
  unsigned long now=time(nullptr); bool changed=false;
  for(int i=0;i<userCount;i++){
    if(users[i].role=="guest" && users[i].expire>0 && users[i].expire<=now){
      users[i].role="user"; users[i].expire=0; changed=true;
    }
  }
  if(changed) saveUsers();
}
String getHelpText(String role){
  String m="Команды:\n/help - помощь\n";
  m+="/open - открыть ворота\n";
  if(role=="admin"){
    m+="/users - список пользователей\n";
    m+="/addcard - добавить карту\n";
    m+="/cardlist - список карт\n";
  }
  m+="/request - запросить доступ";
  return m;
}
void sendAvailableCommands(String chat, String role){
  if (!bot) return;
  bot->sendMessage(chat,getHelpText(role));
}
void sendAdminStatusWithStats(String chat){
  if (!bot) return;
  String msg = "\xF0\x9F\x93\x8A *Статус первой платы*\n\n"; 
  unsigned long up = millis() / 1000;
  unsigned long days = up / 86400;
  unsigned long hours = (up % 86400) / 3600;
  unsigned long mins = (up % 3600) / 60;
  unsigned long secs = up % 60;
  msg += "\xF0\x9F\x95\x92 *Время работы*: ";
  if(days>0) msg += String(days)+" д ";
  if(days>0 || hours>0) msg += String(hours)+" ч ";
  msg += String(mins)+" мин "+String(secs)+" сек\n";
  msg += "\xF0\x9F\x93\x89 *Фрагментация памяти*: " + String(ESP.getHeapFragmentation()) + "%\n";
  msg += "\xF0\x9F\x97\x83 *Свободно для скетча*: " + String(ESP.getFreeSketchSpace()/1024) + " КБ\n";
  msg += "\xF0\x9F\x94\x8B *Текущая версия прошивки*: " + currentVersion + "\n";
  msg += "\xF0\x9F\x94\x8B *Предыдущая версия*: " + previousVersion + "\n";
  msg += "\xF0\x9F\x95\xB0 *Текущее время*: `" + getCurrentTimeStr() + "`\n";
  msg += "\n\xF0\x9F\x93\xA1 *Wi-Fi*:\n";
  if(WiFi.status()==WL_CONNECTED){
    msg += "\xE2\x80\xA2 Подключено к сети: " + WiFi.SSID() + "\n";
    msg += "\xE2\x80\xA2 Локальный IP: `" + WiFi.localIP().toString() + "`\n";
    msg += "\xE2\x80\xA2 MAC-адрес: `" + WiFi.macAddress() + "`\n";
    msg += "\xE2\x80\xA2 Шлюз: `" + WiFi.gatewayIP().toString() + "`\n";
    msg += "\xE2\x80\xA2 Маска подсети: `" + WiFi.subnetMask().toString() + "`\n";
    msg += "\xE2\x80\xA2 DNS: `" + WiFi.dnsIP().toString() + "`";
  }
  String second;
  if(getSecondStatus(second))
    bot->sendMessage(chat, second, "Markdown");
  else
    bot->sendMessage(chat, "Вторая плата не подключена");
  bot->sendMessage(chat, msg, "Markdown");
}

// ===== Поиск второй платы (UDP) =====
void broadcastHello(){
  if(!networkReady) return;
  if(millis()-lastHello>5000){
    udp.beginPacket(IPAddress(255,255,255,255),udpPort);
    udp.write("FIRST-HELLO");
    udp.endPacket();
    lastHello=millis();
  }
}
void listenHello(){
  if(!networkReady) return;
  int ps=udp.parsePacket();
  if(ps){
    char buf[32];
    int len=udp.read(buf,sizeof(buf)-1);
    if(len>0){
      buf[len]=0;
      if(String(buf)=="SECOND-HELLO"){
        IPAddress newIp=udp.remoteIP();
        if(newIp!=secondIP){
          secondIP=newIp;
          sendCards();
        }
      }
    }
  }
}

// ===== Обмен по HTTP =====
bool sendCards(){
  if(!networkReady || !secondIP){
    Serial.println("[HTTP] IP второй платы неизвестен или нет сети");
    return false;
  }
  WiFiClient client; HTTPClient http;
  String url=String("http://")+secondIP.toString()+"/update_cards";
  String body="";
  for(int i=0;i<cardCount;i++){body+=cards[i].uid; if(i<cardCount-1) body+=",";}
  if(!http.begin(client,url)){
    Serial.println("[HTTP] Не удалось сформировать запрос /update_cards");
    return false;
  }
  int code=http.POST(body);
  http.end();
  if(code!=200){
    Serial.printf("[HTTP] Ошибка отправки карт: %d\n", code);
    return false;
  }
  return true;
}
String requestCard(){
  if(!networkReady || !secondIP){
    Serial.println("[HTTP] IP второй платы неизвестен или нет сети");
    return "";
  }
  WiFiClient client; HTTPClient http;
  String url=String("http://")+secondIP.toString()+"/request_card";
  if(!http.begin(client,url)){
    Serial.println("[HTTP] Ошибка запроса /request_card");
    return "";
  }
  int code=http.GET();
  String r="";
  if(code==200) r=http.getString();
  else Serial.printf("[HTTP] Код ответа /request_card: %d\n", code);
  http.end();
  return r;
}
void openGate(){
  if(!networkReady || !secondIP){
    Serial.println("[HTTP] IP второй платы неизвестен или нет сети");
    return;
  }
  WiFiClient client; HTTPClient http;
  String url=String("http://")+secondIP.toString()+"/command?open=1";
  if(!http.begin(client,url)){
    Serial.println("[HTTP] Не удалось сформировать запрос открытия");
    return;
  }
  int code=http.GET();
  http.end();
  if(code!=200) Serial.printf("[HTTP] Ошибка открытия ворот: %d\n", code);
}
void rebootSecondBoard(){
  if(!networkReady || !secondIP){
    Serial.println("[HTTP] IP второй платы неизвестен или нет сети");
    return;
  }
  WiFiClient client; HTTPClient http;
  String url=String("http://")+secondIP.toString()+"/reboot";
  if(!http.begin(client,url)){
    Serial.println("[HTTP] Не удалось отправить запрос перезагрузки");
    return;
  }
  int code=http.GET();
  http.end();
  if(code!=200) Serial.printf("[HTTP] Ошибка перезагрузки второй платы: %d\n", code);
}
bool getSecondStatus(String &out){
  if(!networkReady || !secondIP){
    Serial.println("[HTTP] IP второй платы неизвестен или нет сети");
    return false;
  }
  WiFiClient client; HTTPClient http;
  String url = String("http://") + secondIP.toString() + "/status";
  if(!http.begin(client, url)){
    Serial.println("[HTTP] Ошибка запроса статуса");
    return false;
  }
  int code = http.GET();
  if(code==200){
    out = http.getString();
    http.end();
    return true;
  }
  Serial.printf("[HTTP] Код статуса: %d\n", code);
  http.end();
  return false;
}

// ===== Веб-интерфейс =====
void handleRoot(){
  if(!apServer.authenticate(WEB_USER, WEB_PASS)) return apServer.requestAuthentication();
  String html="<html><body><h2>Настройка</h2><form method='POST' action='/setup'>";
  html+="SSID: <input name='s' value='"+getSSID()+"'><br>";
  html+="PASS: <input name='p' value='"+getPass()+"'><br>";
  html+="BOT: <input name='b' value='"+getToken()+"'><br>";
  html+="ADMIN: <input name='a' value='"+getAdmin()+"'><br>";
  html+="<input type='submit'></form></body></html>";
  apServer.send(200,"text/html",html);
}
void handleSetup(){
  if(!apServer.authenticate(WEB_USER, WEB_PASS)) return apServer.requestAuthentication();
  if(apServer.hasArg("s")) saveEEPROMString(WIFI_SSID_ADDR,apServer.arg("s").c_str(),64);
  if(apServer.hasArg("p")) saveEEPROMString(WIFI_PASS_ADDR,apServer.arg("p").c_str(),64);
  if(apServer.hasArg("b")) saveEEPROMString(BOT_TOKEN_ADDR,apServer.arg("b").c_str(),128);
  if(apServer.hasArg("a")) saveEEPROMString(ADMIN_ID_ADDR,apServer.arg("a").c_str(),32);
  apServer.send(200,"text/html","<html><body>Сохранено. Перезагрузка...</body></html>");
  delay(1000); ESP.restart();
}
void setupWeb(){ apServer.on("/",handleRoot); apServer.on("/setup",HTTP_POST,handleSetup); httpUpdater.setup(&apServer); apServer.begin(); }
void sendCardList(String chat){
  if (!bot) return;
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
  if (!bot) return;
  static unsigned long lastPoll=0; if(millis()-lastPoll<1200) return; lastPoll=millis();
  int n=bot->getUpdates(bot->last_message_received + 1);
  for(int i=0;i<n;i++){
    String chat=bot->messages[i].chat_id;
    String text=bot->messages[i].text;
    String fromId = String(bot->messages[i].from_id);
    String userName = bot->messages[i].from_name;
    int uidx = findUserIndex(fromId);
    String role = (uidx!=-1)?users[uidx].role:"user";

    if(rebootConfirmChat==chat && millis()-rebootRequestTime<10000 && (text=="/reboot" || text=="Перезапуск")){
      sendToAllAdmins("Запрос перезагрузки: перезагружаю вторую плату");
      rebootSecondBoard();
      unsigned long waitStart=millis();
      bool ok=false;
      while(millis()-waitStart<10000){
        listenHello();
        if(secondIP){ ok=true; break; }
        delay(100);
      }
      if(ok) sendToAllAdmins("Вторая плата перезагрузилась");
      else sendToAllAdmins("Не удалось подтвердить перезагрузку второй платы");
      sendToAllAdmins("Первая плата перезагружается");
      notifyAdminsReboot();
      delay(500);
      ESP.restart();
    }

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
          if(saveCards() && sendCards())
            bot->sendMessage(chat,"Карта добавлена! UID: "+pendingUID);
          else
            bot->sendMessage(chat,"Ошибка сохранения карт");
        } else bot->sendMessage(chat,"Лимит карт заполнен");
        addCardMode=false; pendingUID=""; pendingName="";
        continue;
      }
    }
    Cmd cmd = parseCmd(text);
    switch(cmd){
      case CMD_OPEN:
        openGate();
        bot->sendMessage(chat,"Ворота открыты");
        break;
      case CMD_CARDLIST:
        sendCardList(chat);
        break;
      case CMD_DELETE_CARD:
        if(role=="admin"){ int num=text.substring(text.indexOf("-")+1).toInt();
          for(int j=0;j<cardCount;j++) if(cards[j].number==num){
            for(int k=j;k<cardCount-1;k++) cards[k]=cards[k+1];
            cardCount--; if(saveCards() && sendCards())
              bot->sendMessage(chat,"Карта удалена");
            else bot->sendMessage(chat,"Ошибка удаления карты");
            break; } }
        break;
      case CMD_ADD:
        if(role=="admin"){ addCardMode=true; pendingUID=requestCard();
          if(pendingUID.length())
            bot->sendMessage(chat,"Карта считана. UID: `"+pendingUID+"`\nВведите имя владельца:","Markdown");
          else {
            String err = secondIP ? "Не удалось считать карту" : "Вторая плата не подключена";
            bot->sendMessage(chat, err);
            addCardMode=false;
          } }
        break;
      case CMD_USERS:
        if(role=="admin") sendUserList(chat); else bot->sendMessage(chat,"Нет доступа");
        break;
      case CMD_GRANT:
        if(role=="admin"){ int p1=text.indexOf("-",7); int p2=text.indexOf("-",p1+1); String uid=text.substring(7,p1); String rl=text.substring(p1+1,p2); int hrs=text.substring(p2+1).toInt(); approveRequest(uid,rl,hrs); }
        break;
      case CMD_REJECT:
        if(role=="admin"){ String uid=text.substring(8); rejectRequest(uid); }
        break;
      case CMD_REQUEST:
        requestAccess(fromId,userName);
        break;
      case CMD_HELP:
        sendAvailableCommands(chat,role);
        break;
      case CMD_STATUS:
        if(role=="admin") sendAdminStatusWithStats(chat); else bot->sendMessage(chat,"Версия "+currentVersion);
        break;
      case CMD_REBOOT:
        if(role=="admin"){ rebootConfirmChat=chat; rebootRequestTime=millis(); bot->sendMessage(chat,"Повторите команду ещё раз для подтверждения"); }
        break;
      case CMD_NONE:
      default:
        if(text.startsWith("Удалить-") && role=="admin"){ String uid=text.substring(text.indexOf("-")+1); removeUserById(uid); bot->sendMessage(chat,"Пользователь удалён"); }
        else sendAvailableCommands(chat,role);
    }
  }
}

// Подключение к Wi-Fi или запуск точки доступа при неудаче
void connectWiFi(){
  Serial.println("[WiFi] Попытка подключения...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(getSSID().c_str(), getPass().c_str());
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(500);
    Serial.print('.');
  }
  Serial.println();
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiFi] Не удалось подключиться, включаю AP");
    WiFi.disconnect();
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(AP_NAME);
    Serial.print("[WiFi] Точка доступа запущена, IP: ");
    Serial.println(WiFi.softAPIP());
  } else {
    Serial.print("[WiFi] Подключено, IP: ");
    Serial.println(WiFi.localIP());
  }
}

// обновление мигания светодиода
void updateLed(){
  bool ap = (WiFi.status() != WL_CONNECTED);
  unsigned long onMs = ap ? 1500 : 500;
  unsigned long offMs = ap ? 1000 : 500;
  unsigned long interval = ledState ? onMs : offMs;
  if(millis() - ledTimer >= interval){
    ledTimer = millis();
    ledState = !ledState;
    digitalWrite(LED_PIN, ledState ? LOW : HIGH);
  }
}

// === MAIN SETUP/LOOP ===

void setup(){
  Serial.begin(115200);
  delay(500); 
  Serial.println();
  Serial.println("=== ЗАПУСК ПЕРВОЙ ПЛАТЫ ===");
  Serial.print("Причина последнего сброса: ");
  Serial.println(ESP.getResetReason());

  if(!SPIFFS.begin())
    Serial.println("[ERR] Не удалось смонтировать SPIFFS");
  else
    Serial.println("[INFO] SPIFFS смонтирован");

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);

  connectWiFi();
  setupWeb();

  networkReady = (WiFi.status() == WL_CONNECTED);

  if(networkReady){
    Serial.println("[INFO] Запуск сетевых сервисов");
    udp.begin(udpPort);
    tgClient.setInsecure();
    bot = new UniversalTelegramBot(getToken(), tgClient);

    configTime(5*3600,0,"pool.ntp.org","time.nist.gov");

    loadCards();
    loadUsers();
    ensureMainAdmin();
    Serial.printf("[INFO] Свободная память: %u байт\n", ESP.getFreeHeap());
    notifyAdminsReboot();
  } else {
    Serial.println("[INFO] Работаем в режиме точки доступа, сетевые сервисы не запускаем");
  }
}

void loop(){
  apServer.handleClient();
  updateLed();

  if(networkReady) {
    broadcastHello();
    listenHello();
    handleTelegram();
    static unsigned long lastExp = 0;
    if(millis()-lastExp > 60000){
      lastExp = millis();
      removeExpiredCards();
      checkExpiredUsers();
    }
    if(rebootConfirmChat!="" && millis()-rebootRequestTime>10000){
      rebootConfirmChat="";
    }
  }
  delay(5);
}
