#include <Arduino.h>

#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include "FS.h"
#include "SPIFFS.h"

const char* ssid = "Luigi";
const char* password = "luigi123";

AsyncWebServer server(80);

const int led = 2;

String le_temp(){
  float temp = random(0, 1000) / 10.0;
  return String(temp, 1);
}

String le_umid(){
  float umid = random(0, 1000) / 10.0;
  return String(umid, 1);
}

String processor(const String& var){
  if (var == "TEMP") return le_temp();
  if (var == "UMID") return le_umid();
  return String();
}

void setup(){
  Serial.begin(115200);
  pinMode(led, OUTPUT);
  digitalWrite(led, LOW);
  if (!SPIFFS.begin(true))
  {
    Serial.println("Erro ao montar SPIFFS");
    return;
  }
  WiFi.begin(ssid, password);
  Serial.print("Conectando ao WiFi");
  while (WiFi.status() != WL_CONNECTED){
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.println("WiFi conectado");
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());


  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(SPIFFS, "/index.html", String(), false, processor);
  });
  
  // Arquivos estáticos: css, js, png, jpg, svg, etc.
  server.serveStatic("/", SPIFFS, "/");// Comandos
    
  server.on("/on", HTTP_GET, [](AsyncWebServerRequest *request){
    digitalWrite(led, HIGH);request->redirect("/");
  });
    
  server.on("/off", HTTP_GET, [](AsyncWebServerRequest *request){
    digitalWrite(led, LOW);request->redirect("/");
  });
        
  server.on("/temp", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(200, "text/plain", le_temp());
  });
          
  server.on("/umid", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(200, "text/plain", le_umid());
  });
  
  server.begin();

}

void loop()
{}
