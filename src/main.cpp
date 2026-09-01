#include <Arduino.h>

#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <Preferences.h>
#include "FS.h"
#include "SPIFFS.h"

#include <Wire.h>
#include "DHT.h"
#include <Adafruit_BMP085.h>

/* ------------------------------------------------------------------------
 * Modo AP (Access Point)
 * O ESP32 cria a própria rede Wi-Fi para que o usuário configure as
 * credenciais da rede existente. IP padrão do WiFi.softAP(): 192.168.4.1
 * (mesmo valor usado no memorial descritivo e nos slides da disciplina).
 * ---------------------------------------------------------------------- */
const char* ssidAP = "ESP32_Embarcados";
const char* senhaAP = "12345678";

/* ------------------------------------------------------------------------
 * Pinos utilizados
 * ---------------------------------------------------------------------- */
#define PINO_RELE 2          // mesmo pino "led" do projeto original -> agora controla o relé
#define PINO_VENTILADOR 26   // aciona o ventilador (via relé/transistor)
#define DHTPIN 4              // dado do DHT22 (comunicação one-wire)
#define DHTTYPE DHT22
// BMP085 usa I2C: pinos padrão do ESP32 (SDA = GPIO21, SCL = GPIO22)

/* ------------------------------------------------------------------------
 * Objetos globais
 * ---------------------------------------------------------------------- */
AsyncWebServer server(80);
Preferences preferencias;
DHT dht(DHTPIN, DHTTYPE);
Adafruit_BMP085 bmp;

/* ------------------------------------------------------------------------
 * Estado atual dos sensores e atuadores (atualizado a cada 2 segundos)
 * ---------------------------------------------------------------------- */
float g_temperatura = 0.0;
float g_umidade = 0.0;
int32_t g_pressao = 0;      // Pa
float g_altitude = 0.0;     // m
bool g_ventiladorLigado = false;
bool g_releLigado = false;

const float TEMPERATURA_LIGA_VENTILADOR = 25.0;
const unsigned long INTERVALO_LEITURA_MS = 2000;
unsigned long g_ultimaLeitura = 0;

/* ------------------------------------------------------------------------
 * Leitura dos sensores (DHT22 + BMP085)
 * ---------------------------------------------------------------------- */
void lerSensores() {
  float t = dht.readTemperature();
  float h = dht.readHumidity();

  // O DHT22 pode falhar esporadicamente numa leitura; nesse caso mantém
  // o último valor válido em vez de mostrar um número inválido na página.
  if (!isnan(t)) g_temperatura = t;
  if (!isnan(h)) g_umidade = h;

  g_pressao = bmp.readPressure();
  g_altitude = bmp.readAltitude();
}

void controlaVentilador() {
  g_ventiladorLigado = (g_temperatura > TEMPERATURA_LIGA_VENTILADOR);
  digitalWrite(PINO_VENTILADOR, g_ventiladorLigado ? HIGH : LOW);
}

/* ------------------------------------------------------------------------
 * Substituição dos placeholders (%VAR%) do index.html
 * ---------------------------------------------------------------------- */
String processor(const String& var) {
  if (var == "TEMP")      return String(g_temperatura, 1);
  if (var == "UMID")      return String(g_umidade, 1);
  if (var == "PRESSAO")   return String(g_pressao / 100.0, 1); // Pa -> hPa
  if (var == "ALTITUDE")  return String(g_altitude, 1);
  if (var == "VENTILADOR") return g_ventiladorLigado ? "Ligado" : "Desligado";
  if (var == "VENT_CLASSE") return g_ventiladorLigado ? "on" : "off";
  if (var == "RELE")      return g_releLigado ? "Ligado" : "Desligado";
  if (var == "RELE_CLASSE") return g_releLigado ? "on" : "off";
  return String();
}

/* ------------------------------------------------------------------------
 * Modo STA: rotas do dashboard (página principal + controle do relé)
 * ---------------------------------------------------------------------- */
void registrarRotasSTA() {
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(SPIFFS, "/index.html", String(), false, processor);
  });

  // Arquivos estáticos: css, imagens, etc. (não precisa de server.on para cada um)
  server.serveStatic("/", SPIFFS, "/");

  server.on("/rele_on", HTTP_GET, [](AsyncWebServerRequest *request) {
    g_releLigado = true;
    digitalWrite(PINO_RELE, HIGH);
    request->redirect("/");
  });

  server.on("/rele_off", HTTP_GET, [](AsyncWebServerRequest *request) {
    g_releLigado = false;
    digitalWrite(PINO_RELE, LOW);
    request->redirect("/");
  });
}

/* ------------------------------------------------------------------------
 * Modo AP: página de configuração da rede Wi-Fi (grava credenciais na NVS)
 * ---------------------------------------------------------------------- */
void setupAP() {
  Serial.println("Iniciando modo Access Point (AP)...");

  WiFi.mode(WIFI_AP);
  WiFi.softAP(ssidAP, senhaAP);

  Serial.print("Rede criada: ");
  Serial.println(ssidAP);
  Serial.print("IP do Access Point: ");
  Serial.println(WiFi.softAPIP());

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(SPIFFS, "/config.html", "text/html");
  });

  server.on("/save", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (request->hasParam("ssid", true) && request->hasParam("password", true)) {
      String novoSSID = request->getParam("ssid", true)->value();
      String novaSenha = request->getParam("password", true)->value();

      // Salva as credenciais na NVS (memória não volátil), como ensinado em aula.
      preferencias.begin("wifi", false);
      preferencias.putString("ssid", novoSSID);
      preferencias.putString("password", novaSenha);
      preferencias.end();

      request->send(200, "text/html",
        "<!DOCTYPE html><html><head><meta charset='UTF-8'>"
        "<link rel='stylesheet' href='/novo_estilo.css'></head>"
        "<body><h3>Configuracao salva! Reiniciando o ESP32...</h3></body></html>");

      delay(500); // garante que a resposta HTTP seja enviada antes do reset
      ESP.restart();
    } else {
      request->send(400, "text/plain", "Erro: parametros invalidos");
    }
  });

  server.serveStatic("/", SPIFFS, "/");

  server.begin();
}

/* ------------------------------------------------------------------------
 * Modo STA: tenta conectar na rede salva na NVS.
 * Retorna true se conseguiu conectar (e já deixa o servidor rodando).
 * ---------------------------------------------------------------------- */
bool setupSTA(const String &ssidRede, const String &senhaRede) {
  Serial.print("Conectando na rede: ");
  Serial.println(ssidRede);

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssidRede.c_str(), senhaRede.c_str());

  unsigned long inicio = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - inicio < 10000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi conectado com sucesso!");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());

    registrarRotasSTA();
    server.begin();
    return true;
  }

  Serial.println("Falha ao conectar na rede salva.");
  return false;
}

void setup() {
  Serial.begin(115200);

  pinMode(PINO_RELE, OUTPUT);
  pinMode(PINO_VENTILADOR, OUTPUT);
  digitalWrite(PINO_RELE, LOW);
  digitalWrite(PINO_VENTILADOR, LOW);

  dht.begin();

  Wire.begin(); // SDA = GPIO21, SCL = GPIO22 (padrão do ESP32)
  if (!bmp.begin()) {
    Serial.println("Sensor BMP085 nao encontrado. Verifique a fiacao (I2C).");
  }

  if (!SPIFFS.begin(true)) {
    Serial.println("Erro ao montar SPIFFS");
    return;
  }

  // Lê as credenciais da rede wi-fi salvas na NVS.
  preferencias.begin("wifi", true);
  String ssidSalvo = preferencias.getString("ssid", "");
  String senhaSalva = preferencias.getString("password", "");
  preferencias.end();

  if (ssidSalvo == "" || senhaSalva == "") {
    // Nenhuma credencial salva ainda: entra em modo AP para o usuário configurar.
    setupAP();
  } else if (!setupSTA(ssidSalvo, senhaSalva)) {
    // Falhou ao conectar na rede salva dentro do tempo limite: volta para o modo AP.
    setupAP();
  }
}

void loop() {
  // AsyncWebServer não depende do loop() para atender requisições HTTP.
  // Usamos o loop() apenas para ler os sensores a cada ~2 segundos.
  if (millis() - g_ultimaLeitura >= INTERVALO_LEITURA_MS) {
    g_ultimaLeitura = millis();
    lerSensores();
    controlaVentilador();
  }
}
