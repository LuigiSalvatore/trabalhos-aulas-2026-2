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
 * ---------------------------------------------------------------------- */
const char* ssidAP = "ssid";
const char* senhaAP = "123456789";

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
bool g_bmpConectado = false;

const float TEMPERATURA_LIGA_VENTILADOR = 25.0;
const unsigned long INTERVALO_LEITURA_MS = 200;
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

  if (g_bmpConectado) {
    g_pressao = bmp.readPressure();
    g_altitude = bmp.readAltitude();
  } else {
    g_pressao = random(95000, 105000); // Pa
    g_altitude = random(0, 500);       // metros
  }
}

void controlaVentilador() {

  g_ventiladorLigado = (g_temperatura > TEMPERATURA_LIGA_VENTILADOR);
    //Serial.println("Temp lida sensor g_temperatura: " + String(g_temperatura));
  if (g_ventiladorLigado){
    //Serial.println("liga ventilador");
    digitalWrite(PINO_VENTILADOR, HIGH);
  }
  else{
    digitalWrite(PINO_VENTILADOR, LOW);
    //Serial.println("desliga ventilador");
  }
   // Serial.println("Sai func ventilador");
  
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
    Serial.println("liga relé");
    digitalWrite(PINO_RELE, HIGH);
    request->redirect("/");
  });

  server.on("/rele_off", HTTP_GET, [](AsyncWebServerRequest *request) {
    g_releLigado = false;
    digitalWrite(PINO_RELE, LOW);
    Serial.println("desliga relé");
    request->redirect("/");
  });
}

/* ------------------------------------------------------------------------
 * Leitura manual do corpo do POST de "/save"
 * A detecção automática de formulário (application/x-www-form-urlencoded)
 * de algumas versões do ESPAsyncWebServer falha dependendo de como o
 * navegador do celular monta o Content-Type, e request->hasParam(..., true)
 * acaba nunca encontrando os campos. Para não depender só dessa detecção
 * automática, lemos o corpo bruto da requisição nós mesmos.
 * ---------------------------------------------------------------------- */
String g_corpoSave;

String extrairValorForm(const String &corpo, const String &chave) {
  String busca = chave + "=";
  int inicio = corpo.indexOf(busca);
  if (inicio < 0) return "";
  inicio += busca.length();
  int fim = corpo.indexOf('&', inicio);
  if (fim < 0) fim = corpo.length();
  String bruto = corpo.substring(inicio, fim);

  // decodifica application/x-www-form-urlencoded: '+' vira espaço, %XX vira o caractere
  String valor;
  valor.reserve(bruto.length());
  for (unsigned int i = 0; i < bruto.length(); i++) {
    char c = bruto[i];
    if (c == '+') {
      valor += ' ';
    } else if (c == '%' && i + 2 < bruto.length()) {
      char hex[3] = { bruto[i + 1], bruto[i + 2], 0 };
      valor += (char) strtol(hex, NULL, 16);
      i += 2;
    } else {
      valor += c;
    }
  }
  return valor;
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

  server.on("/save", HTTP_POST,
    [](AsyncWebServerRequest *request) {
      // 1) tenta primeiro o corpo bruto que capturamos no onBody (mais confiável)
      String novoSSID = extrairValorForm(g_corpoSave, "ssid");
      String novaSenha = extrairValorForm(g_corpoSave, "password");
      g_corpoSave = ""; // limpa para a próxima requisição

      // 2) se por algum motivo o corpo não chegou no onBody, tenta o parser
      //    automático da biblioteca como reforço
      if (novoSSID.length() == 0 && request->hasParam("ssid", true)) {
        novoSSID = request->getParam("ssid", true)->value();
      }
      if (novaSenha.length() == 0 && request->hasParam("password", true)) {
        novaSenha = request->getParam("password", true)->value();
      }

      if (novoSSID.length() > 0 && novaSenha.length() > 0) {
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
    },
    NULL, // onUpload: não usamos upload de arquivo neste formulário
    [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
      // onBody: vai acumulando o corpo bruto da requisição POST
      if (index == 0) g_corpoSave = "";
      for (size_t i = 0; i < len; i++) {
        g_corpoSave += (char) data[i];
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

  Wire.begin(); 
  
  g_bmpConectado = bmp.begin();
  if (!g_bmpConectado) {
    Serial.println("Sensor BMP085 nao encontrado. Entrando em modo de simulacao.");
  } else {
    Serial.println("Sensor BMP085 conectado com sucesso!");
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
