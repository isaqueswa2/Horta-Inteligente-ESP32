// ============================================================
// CONTROLE DE VERSÃO DO FIRMWARE
// ============================================================
#define FIRMWARE_VERSION "V2.0.0-BETA"
#define FIRMWARE_DATE "06/10/2026"

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#include <WiFi.h>
#include <WiFiManager.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>

#include <ESPAsyncWebServer.h>
#include <Preferences.h>

// Bibliotecas dos Sensores
#include <OneWire.h>
#include <DallasTemperature.h>

// Interface Web
#include "web_page.h"

// ============================================================
// ESTRUTURA DE PREFERÊNCIAS & SERVIDOR WEB
// ============================================================

Preferences prefs;
AsyncWebServer server(80);

struct Configuracoes {
uint16_t minBombaLigadaMin = 10;
uint16_t maxBombaDesligadaMin = 18;
uint16_t debounceSemFluxoSec = 5;
uint16_t toleranciaNivelSec = 3;
uint16_t intAlertaNivelMin = 30;
uint16_t intAlertaFluxoMin = 5;
bool notifHorariaAtiva = true;
} cfg;

void carregarPreferences() {
prefs.begin("horta_cfg", true);
cfg.minBombaLigadaMin = prefs.getUShort("minOn", 10);
cfg.maxBombaDesligadaMin = prefs.getUShort("maxOff", 18);
cfg.debounceSemFluxoSec = prefs.getUShort("debFlx", 5);
cfg.toleranciaNivelSec = prefs.getUShort("tolNvl", 3);
cfg.intAlertaNivelMin = prefs.getUShort("intNvl", 30);
cfg.intAlertaFluxoMin = prefs.getUShort("intFlx", 5);
cfg.notifHorariaAtiva = prefs.getBool("notifH", true);
prefs.end();
}

void salvarPreferences() {
prefs.begin("horta_cfg", false);
prefs.putUShort("minOn", cfg.minBombaLigadaMin);
prefs.putUShort("maxOff", cfg.maxBombaDesligadaMin);
prefs.putUShort("debFlx", cfg.debounceSemFluxoSec);
prefs.putUShort("tolNvl", cfg.toleranciaNivelSec);
prefs.putUShort("intNvl", cfg.intAlertaNivelMin);
prefs.putUShort("intFlx", cfg.intAlertaFluxoMin);
prefs.putBoolean("notifH", cfg.notifHorariaAtiva);
prefs.end();
}

// ============================================================
// CREDENCIAIS DO TELEGRAM
// ============================================================

#define BOT_TOKEN "8989762709:AAEMCYTzEN4LqdDdRC7RvywpurILz0yP274"
#define CHAT_ID "77883779"

WiFiClientSecure clientSecure;
UniversalTelegramBot bot(BOT_TOKEN, clientSecure);

// ============================================================
// MATRIZ OFICIAL DE PINOS DO ESP32-S3
// ============================================================

#define ENCODER_CLK_PIN 15
#define ENCODER_DT_PIN 16
#define ENCODER_SW_PIN 17

#define OLED_SDA 9
#define OLED_SCL 10

#define SENSOR_FLUXO_PIN 47 // Borne 3 (YF-S201)
#define SENSOR_NIVEL_PIN 48 // Borne 4 (XKC-Y25-V)
#define DS18B20_PIN 4 // Borne 5 (DS18B20)
#define PIN_RESERVA 36 // Borne 7 (Livre)

enum TipoTela { TELA_DADOS_1, TELA_DADOS_2 };
TipoTela telaAtual = TELA_DADOS_1;
TipoTela proximaTela = TELA_DADOS_2;

void iniciarSlide(TipoTela novaTela);
void desenharPagina(TipoTela tela, int offsetX);
void renderizarPaginaNoBuffer(TipoTela tela, uint8_t* destino);
void enviarStatusHorario(bool silencioso);
bool enviarMensagemTelegram(String mensagem, bool silencioso);
void exibirMenuSelecaoModo();

// ============================================================
// ENCODER ROTATIVO EC11
// ============================================================

volatile int contadorEncoder = 0;
volatile uint8_t estadoAnteriorEncoder = 0;

const int8_t TABELA_ENCODER[] PROGMEM = {
0, -1, 1, 0,
1, 0, 0, -1,
-1, 0, 0, 1,
0, 1, -1, 0
};

void IRAM_ATTR icrEncoderGiro() {
uint8_t estadoAtual = (digitalRead(ENCODER_CLK_PIN) << 1) | digitalRead(ENCODER_DT_PIN);
if (estadoAtual != estadoAnteriorEncoder) {
uint8_t indice = (estadoAnteriorEncoder << 2) | estadoAtual;
int8_t movimento = pgm_read_byte(&TABELA_ENCODER[indice]);
if (movimento != 0) contadorEncoder += movimento;
estadoAnteriorEncoder = estadoAtual;
}
}

unsigned long tempoInicioPressionado = 0;
bool botaoPressionado = false;
const unsigned long TEMPO_DEBOUNCE_BOTAO = 80;
const unsigned long TEMPO_PRESSAO_LONGA = 2000;

// ============================================================
// SENSORES E VARIÁVEIS DE MEDIÇÃO
// ============================================================

OneWire oneWire(DS18B20_PIN);
DallasTemperature sensorDS18B20(&oneWire);

unsigned long ultimaLeituraDS18B20 = 0;
const unsigned long INTERVALO_LEITURA_DS18B20 = 2000UL;

unsigned long ultimaLeituraNivel = 0;
const unsigned long INTERVALO_LEITURA_NIVEL = 500UL;

volatile unsigned long contadorPulsosFluxo = 0;
unsigned long ultimoCalculoFluxo = 0;
float fluxoAgua = 0.0;

void IRAM_ATTR icrContarPulsosFluxo() { contadorPulsosFluxo++; }

float tempSolucao = NAN;
float tempAmbiente = NAN;

bool nivelBaixo = false;
bool nivelBaixoTravado = false;
unsigned long tempoInicioNivelOkContinuo = 0;
const unsigned long TEMPO_MINIMO_REABASTECIMENTO = 1200000UL;

unsigned long tempoInicioPaneNivel = 0;
const unsigned long TEMPO_LIMITE_SUCCAO_BOMBA = 43200000UL;

bool bombaAtiva = false;
unsigned long tempoInicioFluxo = 0;
unsigned long tempoFimFluxo = 0;
bool estadoFluxoAnterior = false;

unsigned long tempoInicioSemFluxo = 0;

bool alertaPaneBombaPrecoce = false;
bool alertaPaneBombaAtraso = false;
bool modoManutencao = false;

// OLED
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

#define OLED_ADDR_1 0x3C
#define OLED_ADDR_2 0x3D
uint8_t oledEndereco = OLED_ADDR_1;

const uint8_t farm_128x48_ok[] PROGMEM = {
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x0e,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x1f,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x1b,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x0e,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x1c,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x36,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x36,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x3e,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x03,0xbe,0x7f,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x07,0xbe,0xff,0xc0,0x01,0xef,0x6f,0xf0,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x0f,0xbe,0xff,0xe0,0x01,0xef,0x6f,0xf0,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x1f,0x3e,0x80,0x70,0x03,0xee,0x6e,0xf8,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x3e,0x3e,0x80,0x38,0x03,0xee,0x6e,0xf8,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x7c,0x3f,0xff,0xfc,0x07,0xc0,0x00,0xfe,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x78,0x20,0x92,0x24,0x07,0xbb,0xdd,0xee,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x78,0x20,0x92,0x24,0x07,0xbb,0xdd,0xee,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x78,0x20,0x92,0x24,0x07,0xbb,0xdd,0xee,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x7f,0xff,0xff,0xfc,0x07,0xbb,0xdd,0xee,0x00,0x77,0x77,0x70,
0x00,0x00,0x00,0x00,0x78,0x3e,0x92,0x24,0x07,0xbb,0xdd,0xee,0x00,0x77,0x77,0x70,
0x00,0x00,0x00,0x00,0x78,0x22,0x92,0x24,0x07,0xbb,0xdd,0xee,0x00,0x77,0x77,0x70,
0x00,0x00,0x00,0x00,0x78,0x22,0x92,0x24,0x07,0xbb,0xdd,0xee,0x00,0x77,0x77,0x70,
0x00,0x00,0x00,0x00,0x78,0x22,0x92,0x24,0x07,0x00,0x00,0x0e,0x00,0x77,0x77,0x70,
0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x04,0x00,0x08,0x00,0x00,0x00,0x0c,0x00,0x00,0x00,0x18,0x00,0x00,0x30,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x12,0x24,0x48,0x90,0x12,0x24,0x48,0x90,0x12,0x24,0x48,0x90,0x12,0x24,0x48,0x90,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x24,0x48,0x90,0x24,0x48,0x90,0x24,0x48,0x90,0x24,0x48,0x90,0x24,0x48,0x90,0x24,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x48,0x90,0x24,0x48,0x90,0x24,0x48,0x90,0x24,0x48,0x90,0x24,0x48,0x90,0x24,0x48,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x90,0x24,0x48,0x90,0x24,0x48,0x90,0x24,0x48,0x90,0x24,0x48,0x90,0x24,0x48,0x90,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff
};

#define HEADER_HEIGHT 16
#define DIVIDER_X 63
#define LEFT_CENTER_X 32
#define RIGHT_CENTER_X 96
#define ICON_CENTER_Y 31
#define VALUE_Y 52

#define LATITUDE -25.294137
#define LONGITUDE -54.106511

const char* NTP_SERVER_1 = "pool.ntp.org";
const char* NTP_SERVER_2 = "time.nist.gov";

const long GMT_OFFSET_SEC = -3 * 3600;
const int DAYLIGHT_OFFSET_SEC = 0;

unsigned long ultimoTempoTela = 0;
unsigned long ultimoFrameAnimacao = 0;
const unsigned long TEMPO_TELA = 7000;
const unsigned long TEMPO_ANIMACAO = 160;

unsigned long ultimoPisca = 0;
bool estadoPisca = true;
const unsigned long TEMPO_PISCA = 400;

unsigned long ultimoSegundo = 0;
bool doisPontosVisivel = true;

unsigned long ultimaAtualizacaoInternet = 0;
const unsigned long INTERVALO_TEMPERATURA = 10UL * 60UL * 1000UL;

unsigned long ultimaTentativaWiFi = 0;
const unsigned long INTERVALO_RECONEXAO_WIFI = 30000UL;

unsigned long ultimaNotificacaoHora = 0;
const unsigned long INTERVALO_NOTIFICACAO_HORA = 3600000UL;

unsigned long ultimoAlertaNivel = 0;
unsigned long ultimoAlertaFluxo = 0;

bool alertaNivelEnviado = false;
bool notificacaoBootEnviada = false;

bool emTransicao = false;
int deslocamentoSlide = 0;
const int VELOCIDADE_SLIDE = 8;
int faseAnimacao = 0;

// ============================================================
// CONFIGURAÇÃO DOS ENDPOINTS DA API WEB
// ============================================================

void configurarRotasWebServer() {
server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
request->send_P(200, "text/html", PAGE_MAIN);
});

server.on("/api/data", HTTP_GET, [](AsyncWebServerRequest *request) {
JsonDocument doc;
doc["ip"] = WiFi.localIP().toString();
doc["tempSolucao"] = isnan(tempSolucao) ? "--.- °C" : String(tempSolucao, 1) + " °C";
doc["tempAmbiente"] = isnan(tempAmbiente) ? "--.- °C" : String(tempAmbiente, 1) + " °C";
doc["fluxoAgua"] = String(fluxoAgua, 2);
doc["bombaAtiva"] = bombaAtiva;
doc["nivelBaixo"] = nivelBaixo;
doc["modoManutencao"] = modoManutencao;

JsonObject cfgObj = doc["cfg"].to<JsonObject>();
cfgObj["minOn"] = cfg.minBombaLigadaMin;
cfgObj["maxOff"] = cfg.maxBombaDesligadaMin;
cfgObj["debFluxo"] = cfg.debounceSemFluxoSec;
cfgObj["tolNivel"] = cfg.toleranciaNivelSec;
cfgObj["intNivel"] = cfg.intAlertaNivelMin;
cfgObj["intFluxo"] = cfg.intAlertaFluxoMin;
cfgObj["notifHoraria"] = cfg.notifHorariaAtiva;

String jsonResponse;
serializeJson(doc, jsonResponse);
request->send(200, "application/json", jsonResponse);
});

server.on("/api/action", HTTP_POST, [](AsyncWebServerRequest *request) {
if (request->hasParam("cmd")) {
String cmd = request->getParam("cmd")->value();
if (cmd == "manutencao" && request->hasParam("val")) {
modoManutencao = (request->getParam("val")->value() == "1");
} else if (cmd == "resetNivel") {
nivelBaixoTravado = false;
tempoInicioPaneNivel = 0;
} else if (cmd == "resetBomba") {
alertaPaneBombaPrecoce = false;
alertaPaneBombaAtraso = false;
}
}
request->send(200, "text/plain", "OK");
});

server.on("/api/save", HTTP_POST, [](AsyncWebServerRequest *request) {
if (request->hasParam("minOn")) cfg.minBombaLigadaMin = request->getParam("minOn")->value().toInt();
if (request->hasParam("maxOff")) cfg.maxBombaDesligadaMin = request->getParam("maxOff")->value().toInt();
if (request->hasParam("debFluxo")) cfg.debounceSemFluxoSec = request->getParam("debFluxo")->value().toInt();
if (request->hasParam("tolNivel")) cfg.toleranciaNivelSec = request->getParam("tolNivel")->value().toInt();
if (request->hasParam("intNivel")) cfg.intAlertaNivelMin = request->getParam("intNivel")->value().toInt();
if (request->hasParam("intFluxo")) cfg.intAlertaFluxoMin = request->getParam("intFluxo")->value().toInt();
if (request->hasParam("notifHoraria")) cfg.notifHorariaAtiva = (request->getParam("notifHoraria")->value() == "1");

salvarPreferences();
request->send(200, "text/plain", "Configurações salvas no ESP32-S3 com sucesso!");
});

server.begin();
Serial.println("[WEB] Servidor Asíncrono iniciado!");
}

// ============================================================
// MENU INTERATIVO DE INICIALIZAÇÃO E ENCODER
// ============================================================

void exibirMenuSelecaoModo() {
int opcaoSelecionada = 0;
unsigned long tempoInicioMenu = millis();
const unsigned long TIMEOUT_MENU = 8000;
bool confirmado = false;

noInterrupts();
contadorEncoder = 0;
estadoAnteriorEncoder = (digitalRead(ENCODER_CLK_PIN) << 1) | digitalRead(ENCODER_DT_PIN);
interrupts();

while ((millis() - tempoInicioMenu < TIMEOUT_MENU) && !confirmado) {
if (abs(contadorEncoder) >= 2) {
if (contadorEncoder > 0) opcaoSelecionada = (opcaoSelecionada + 1) % 2;
else opcaoSelecionada = (opcaoSelecionada == 0) ? 1 : 0;
noInterrupts(); contadorEncoder = 0; interrupts();
tempoInicioMenu = millis();
}

if (digitalRead(ENCODER_SW_PIN) == LOW) {
delay(60);
if (digitalRead(ENCODER_SW_PIN) == LOW) {
confirmado = true;
while (digitalRead(ENCODER_SW_PIN) == LOW) { delay(10); }
}
}

display.clearDisplay();
display.setTextSize(1);
display.setTextColor(WHITE);
display.setCursor(12, 2);
display.print("SELECIONE O MODO");
display.drawFastHLine(0, 13, 128, WHITE);

if (opcaoSelecionada == 0) { display.fillRect(4, 18, 120, 18, WHITE); display.setTextColor(BLACK, WHITE); }
else display.setTextColor(WHITE, BLACK);
display.setCursor(12, 23); display.print("1. MODO NORMAL");

if (opcaoSelecionada == 1) { display.fillRect(4, 38, 120, 18, WHITE); display.setTextColor(BLACK, WHITE); }
else display.setTextColor(WHITE, BLACK);
display.setCursor(12, 43); display.print("2. MANUTENCAO");

int tempoRestante = TIMEOUT_MENU - (millis() - tempoInicioMenu);
int larguraBarra = map(tempoRestante, 0, TIMEOUT_MENU, 0, 128);
display.fillRect(0, 60, larguraBarra, 3, WHITE);

display.display();
delay(15);
}

modoManutencao = (opcaoSelecionada == 1);
display.clearDisplay(); display.setTextSize(1); display.setTextColor(WHITE); display.setCursor(10, 25);
display.print(modoManutencao ? "MODO MANUTENCAO!" : "MODO NORMAL ATIVO!");
display.display();
delay(1000);
}

void atualizarEncoder() {
if (abs(contadorEncoder) >= 2) {
bool giraDireita = (contadorEncoder > 0);
noInterrupts(); contadorEncoder = 0; interrupts();
if (!emTransicao) iniciarSlide(telaAtual == TELA_DADOS_1 ? TELA_DADOS_2 : TELA_DADOS_1);
}

int estadoSW = digitalRead(ENCODER_SW_PIN);
if (estadoSW == LOW) {
if (!botaoPressionado) {
botaoPressionado = true;
tempoInicioPressionado = millis();
} else if ((millis() - tempoInicioPressionado >= TEMPO_PRESSAO_LONGA) && tempoInicioPressionado != 0) {
tempoInicioPressionado = 0;
modoManutencao = !modoManutencao;
enviarMensagemTelegram(modoManutencao ? "🛠️ MODO MANUTENÇÃO ATIVADO" : "✅ MODO MANUTENÇÃO DESATIVADO", true);
}
} else if (botaoPressionado) {
unsigned long duracaoClique = millis() - tempoInicioPressionado;
if (tempoInicioPressionado != 0 && duracaoClique >= TEMPO_DEBOUNCE_BOTAO && duracaoClique < TEMPO_PRESSAO_LONGA) {
enviarStatusHorario(true);
}
botaoPressionado = false; tempoInicioPressionado = 0;
}
}

// ============================================================
// LEITURA DE SENSORES E PARÂMETROS DINÂMICOS
// ============================================================

void atualizarTemperaturaSolucao() {
unsigned long agora = millis();
if (agora - ultimaLeituraDS18B20 >= INTERVALO_LEITURA_DS18B20) {
ultimaLeituraDS18B20 = agora;
sensorDS18B20.requestTemperatures();
float tempLida = sensorDS18B20.getTempCByIndex(0);
if (tempLida != DEVICE_DISCONNECTED_C && tempLida > -55.0 && tempLida < 125.0) tempSolucao = tempLida;
else tempSolucao = NAN;
}
}

void atualizarNivelReservatorio() {
unsigned long agora = millis();
if (agora - ultimaLeituraNivel < INTERVALO_LEITURA_NIVEL) return;
ultimaLeituraNivel = agora;

int leituraPino = digitalRead(SENSOR_NIVEL_PIN);

if (leituraPino == HIGH) {
if (!nivelBaixoTravado) {
nivelBaixoTravado = true;
tempoInicioPaneNivel = agora;
}
tempoInicioNivelOkContinuo = 0;
} else {
if (tempoInicioNivelOkContinuo == 0) tempoInicioNivelOkContinuo = agora;
else if (agora - tempoInicioNivelOkContinuo >= TEMPO_MINIMO_REABASTECIMENTO) {
nivelBaixoTravado = false;
tempoInicioPaneNivel = 0;
}
}

nivelBaixo = nivelBaixoTravado;
}

void calcularFluxoAgua() {
unsigned long agora = millis();
if (agora - ultimoCalculoFluxo >= 1000) {
noInterrupts();
unsigned long pulsos = contadorPulsosFluxo;
contadorPulsosFluxo = 0;
interrupts();

fluxoAgua = (float)pulsos / 7.5;
ultimoCalculoFluxo = agora;

bool fluxoDetectado = (fluxoAgua > 0.3);

if (fluxoDetectado) {
tempoInicioSemFluxo = 0;
if (!estadoFluxoAnterior) {
bombaAtiva = true;
tempoInicioFluxo = agora;
estadoFluxoAnterior = true;
alertaPaneBombaAtraso = false;
alertaPaneBombaPrecoce = false;
}
} else if (!fluxoDetectado && estadoFluxoAnterior) {
if (tempoInicioSemFluxo == 0) {
tempoInicioSemFluxo = agora;
} else if (agora - tempoInicioSemFluxo >= (cfg.debounceSemFluxoSec * 1000UL)) { // VALOR DINÂMICO DA WEB
bombaAtiva = false;
tempoFimFluxo = agora;
estadoFluxoAnterior = false;
tempoInicioSemFluxo = 0;

unsigned long duracaoLigada = agora - tempoInicioFluxo;
if (duracaoLigada < (cfg.minBombaLigadaMin * 60000UL)) { // VALOR DINÂMICO DA WEB
alertaPaneBombaPrecoce = true;
} else {
alertaPaneBombaPrecoce = false;
}
}
}

if (!bombaAtiva && tempoFimFluxo > 0) {
if (agora - tempoFimFluxo > (cfg.maxBombaDesligadaMin * 60000UL)) { // VALOR DINÂMICO DA WEB
alertaPaneBombaAtraso = true;
}
}
}
}

// ============================================================
// OLED & RENDERIZAÇÃO
// ============================================================

void desenharGrau(int x, int y) {
display.drawPixel(x + 1, y, WHITE); display.drawPixel(x + 2, y, WHITE);
display.drawPixel(x, y + 1, WHITE); display.drawPixel(x + 3, y + 1, WHITE);
display.drawPixel(x, y + 2, WHITE); display.drawPixel(x + 3, y + 2, WHITE);
display.drawPixel(x + 1, y + 3, WHITE); display.drawPixel(x + 2, y + 3, WHITE);
}

void textoCentralizado(const char* texto, int centroX, int y, uint8_t tamanho = 1) {
int16_t x1, y1; uint16_t w, h;
display.setTextSize(tamanho); display.setTextColor(WHITE);
display.getTextBounds(texto, 0, y, &x1, &y1, &w, &h);
int x = centroX - (w / 2);
if (x < 0) x = 0; if (x + w > SCREEN_WIDTH) x = SCREEN_WIDTH - w;
display.setCursor(x, y); display.print(texto);
}

void temperaturaCentralizada(float temperatura, int centroX, int y) {
char numero[12];
if (isnan(temperatura)) { textoCentralizado("--.- C", centroX, y, 1); return; }
dtostrf(temperatura, 0, 1, numero);
display.setTextSize(1); display.setTextColor(WHITE);
int16_t x1, y1; uint16_t w, h;
display.getTextBounds(numero, 0, y, &x1, &y1, &w, &h);
int larguraTotal = w + 4 + 6;
int xInicial = centroX - (larguraTotal / 2);
display.setCursor(xInicial, y); display.print(numero);
int xGrau = xInicial + w + 1;
desenharGrau(xGrau, y - 1);
display.setCursor(xGrau + 5, y); display.print("C");
}

void iconeTemperatura(int cx, int cy) {
display.drawRoundRect(cx - 4, cy - 12, 8, 20, 4, WHITE);
display.fillCircle(cx, cy + 8, 6, WHITE); display.fillCircle(cx, cy + 8, 3, BLACK);
display.drawLine(cx, cy - 8, cx, cy + 6, WHITE);
const int niveis[] = { 3, 5, 7, 9, 7, 5 };
display.drawLine(cx, cy + 5, cx, cy + 5 - niveis[faseAnimacao % 6], WHITE);
int pulso = faseAnimacao % 3;
display.drawLine(cx + 6, cy - 6, cx + 8, cy - 6, WHITE);
display.drawLine(cx + 6, cy, cx + 8, cy, WHITE);
display.drawLine(cx + 6, cy + 6 + pulso, cx + 8, cy + 6 + pulso, WHITE);
}

void iconeFluxo(int cx, int cy) {
display.fillTriangle(cx, cy - 13, cx - 8, cy + 2, cx + 8, cy + 2, WHITE);
display.fillCircle(cx, cy + 1, 8, WHITE);
display.fillTriangle(cx, cy - 8, cx - 5, cy + 2, cx + 5, cy + 2, BLACK);
display.fillCircle(cx, cy + 2, 5, BLACK);
int deslocamento = (faseAnimacao % 4);
display.drawLine(cx - 11 + deslocamento, cy + 10, cx - 4 + deslocamento, cy + 10, WHITE);
display.drawLine(cx + 3 + deslocamento, cy + 10, cx + 10 + deslocamento, cy + 10, WHITE);
}

void iconeReservatorio(int cx, int cy, bool minimo) {
if (minimo && !estadoPisca) return;
int x = cx - 12; int y = cy - 11;
display.drawRoundRect(x, y, 24, 23, 3, WHITE);
if (!minimo) {
display.fillRect(x + 2, y + 7, 20, 14, WHITE);
} else {
display.fillRect(x + 2, y + 17, 20, 4, WHITE);
}
}

void iconeBomba(int cx, int cy, bool ligada) {
if (!ligada && !estadoPisca) return;
int x = cx - 12; int y = cy - 11;
display.drawRoundRect(x, y, 24, 23, 3, WHITE);
display.drawCircle(cx, cy, 9, WHITE); display.fillCircle(cx, cy, 7, BLACK);
display.fillCircle(cx, cy, 2, WHITE);
}

const uint8_t wifi_icon_3b_centered_16x13[] PROGMEM = {
0x1F, 0xF8, 0x3F, 0xFC, 0x70, 0x0E, 0x00, 0x00,
0x0F, 0xF0, 0x1F, 0xF8, 0x18, 0x18, 0x00, 0x00,
0x01, 0x80, 0x01, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

void iconeWiFi(int cx, int cy) {
if (WiFi.status() == WL_CONNECTED || estadoPisca) {
display.drawBitmap(cx - 8, cy, wifi_icon_3b_centered_16x13, 16, 13, WHITE);
}
}

void desenharCabecalho() {
display.fillRect(0, 0, SCREEN_WIDTH, HEADER_HEIGHT, BLACK);
if (modoManutencao) {
if (estadoPisca) textoCentralizado("MANUTENCAO", 64, 4, 1);
display.drawLine(0, 15, 127, 15, WHITE); return;
}
struct tm timeinfo; char hora[3] = "--"; char minutos[3] = "--";
if (getLocalTime(&timeinfo, 0)) {
strftime(hora, sizeof(hora), "%H", &timeinfo);
strftime(minutos, sizeof(minutos), "%M", &timeinfo);
}
display.setTextSize(1); display.setTextColor(WHITE, BLACK);
display.setCursor(4, 4); display.print(hora);
display.setCursor(16, 4); display.print(doisPontosVisivel ? ":" : " ");
display.setCursor(22, 4); display.print(minutos);

char temp[8];
if (isnan(tempAmbiente)) strcpy(temp, "--.-"); else dtostrf(tempAmbiente, 0, 1, temp);
int16_t x1, y1; uint16_t w, h;
display.getTextBounds(temp, 0, 4, &x1, &y1, &w, &h);
int xInicial = 128 - (w + 12) - 2;
iconeWiFi(50, 1);
display.setCursor(xInicial, 4); display.print(temp);
desenharGrau(xInicial + w + 2, 0);
display.setCursor(xInicial + w + 8, 4); display.print("C");
display.drawLine(0, 15, 127, 15, WHITE);
}

void desenharTelaDados1(int offsetX) {
display.drawLine(DIVIDER_X + offsetX, HEADER_HEIGHT, DIVIDER_X + offsetX, 63, WHITE);
iconeTemperatura(LEFT_CENTER_X + offsetX, ICON_CENTER_Y);
temperaturaCentralizada(tempSolucao, LEFT_CENTER_X + offsetX, VALUE_Y);
iconeFluxo(RIGHT_CENTER_X + offsetX, ICON_CENTER_Y);
char textoFluxo[20]; snprintf(textoFluxo, sizeof(textoFluxo), "%.2f L/M", fluxoAgua);
textoCentralizado(textoFluxo, RIGHT_CENTER_X + offsetX, VALUE_Y, 1);
}

void desenharTelaDados2(int offsetX) {
display.drawLine(DIVIDER_X + offsetX, HEADER_HEIGHT, DIVIDER_X + offsetX, 63, WHITE);
iconeReservatorio(LEFT_CENTER_X + offsetX, ICON_CENTER_Y, nivelBaixo);
textoCentralizado(modoManutencao ? "PAUSADO" : (nivelBaixo ? "MINIMO" : "CHEIO"), LEFT_CENTER_X + offsetX, VALUE_Y, 1);
iconeBomba(RIGHT_CENTER_X + offsetX, ICON_CENTER_Y, bombaAtiva);
textoCentralizado(modoManutencao ? "PAUSADO" : (bombaAtiva ? "LIGADA" : "DESLIGADA"), RIGHT_CENTER_X + offsetX, VALUE_Y, 1);
}

void desenharPagina(TipoTela tela, int offsetX) {
desenharCabecalho();
if (tela == TELA_DADOS_1) desenharTelaDados1(offsetX);
else if (tela == TELA_DADOS_2) desenharTelaDados2(offsetX);
}

void iniciarSlide(TipoTela novaTela) { if (!emTransicao) { proximaTela = novaTela; emTransicao = true; deslocamentoSlide = 0; } }

uint8_t bufferTelaAtual[SCREEN_WIDTH * SCREEN_HEIGHT / 8];
uint8_t bufferProximaTela[SCREEN_WIDTH * SCREEN_HEIGHT / 8];

void copiarBufferOLED(uint8_t* destino) { memcpy(destino, display.getBuffer(), SCREEN_WIDTH * SCREEN_HEIGHT / 8); }
bool lerPixelBuffer(const uint8_t* buffer, int x, int y) {
if (x < 0 || x >= SCREEN_WIDTH || y < 0 || y >= SCREEN_HEIGHT) return false;
return (buffer[x + (y / 8) * SCREEN_WIDTH] & (1 << (y & 7)));
}
void escreverPixelBuffer(uint8_t* buffer, int x, int y) {
if (x < 0 || x >= SCREEN_WIDTH || y < 0 || y >= SCREEN_HEIGHT) return;
buffer[x + (y / 8) * SCREEN_WIDTH] |= (1 << (y & 7));
}

void renderizarPaginaNoBuffer(TipoTela tela, uint8_t* destino) {
display.clearDisplay(); desenharPagina(tela, 0); copiarBufferOLED(destino);
}

void executarSlide() {
if (!emTransicao) return;
renderizarPaginaNoBuffer(telaAtual, bufferTelaAtual);
renderizarPaginaNoBuffer(proximaTela, bufferProximaTela);
uint8_t* bufferSaida = display.getBuffer();
memset(bufferSaida, 0, SCREEN_WIDTH * SCREEN_HEIGHT / 8);
int limite = SCREEN_WIDTH - deslocamentoSlide;

for (int y = 0; y < SCREEN_HEIGHT; y++) {
for (int x = 0; x < SCREEN_WIDTH; x++) {
bool pixel = (y < HEADER_HEIGHT) ? lerPixelBuffer(bufferTelaAtual, x, y) :
(x < limite) ? lerPixelBuffer(bufferTelaAtual, x + deslocamentoSlide, y) :
lerPixelBuffer(bufferProximaTela, x - limite, y);
if (pixel) escreverPixelBuffer(bufferSaida, x, y);
}
}
display.display(); deslocamentoSlide += VELOCIDADE_SLIDE;
if (deslocamentoSlide >= SCREEN_WIDTH) {
emTransicao = false; telaAtual = proximaTela; deslocamentoSlide = 0; ultimoTempoTela = millis();
display.clearDisplay(); desenharPagina(telaAtual, 0); display.display();
}
}

// ============================================================
// TELEGRAM
// ============================================================

bool enviarMensagemTelegram(String mensagem, bool silencioso) {
if (WiFi.status() != WL_CONNECTED) return false;
clientSecure.stop(); clientSecure.setInsecure(); clientSecure.setTimeout(6000);
bool sucesso = bot.sendMessage(CHAT_ID, mensagem, "Markdown", silencioso);
if (!sucesso) sucesso = bot.sendMessage(CHAT_ID, mensagem, "Markdown", silencioso);
return sucesso;
}

void enviarStatusHorario(bool silencioso) {
if (!cfg.notifHorariaAtiva && silencioso) return; // Respeita a flag da Web

String msg = "🌱 STATUS DA HORTA (RELATÓRIO)\n\n";
if (modoManutencao) msg += "⚠️ MODO MANUTENÇÃO ATIVO\n";
msg += "🌡️ Temp. Ambiente: " + (isnan(tempAmbiente) ? String("--.-") : String(tempAmbiente, 1)) + " °C\n";
msg += "🧪 Temp. Solução: " + (isnan(tempSolucao) ? String("--.-") : String(tempSolucao, 1)) + " °C\n";
msg += "💧 Fluxo de Água: " + String(fluxoAgua, 2) + " L/M\n";
msg += "🛢️ Reservatório: " + String(nivelBaixo ? "⚠️ MÍNIMO (LATCH)" : "✅ CHEIO") + "\n";
msg += "⚙️ Bomba: " + String(bombaAtiva ? "✅ EM OPERAÇÃO" : "⏸️ EM REPOUSO");
enviarMensagemTelegram(msg, silencioso);
}

void verificarAlertasInstantaneos() {
if (modoManutencao) return;
unsigned long agora = millis();

// ALERTA DE NÍVEL (Intervalo vindo das configurações Web)
if (nivelBaixo) {
if (agora - ultimoAlertaNivel >= (cfg.intAlertaNivelMin * 60000UL) || !alertaNivelEnviado) {
String msg = "🚨 ATENÇÃO: PANE DE NÍVEL MÍNIMO!\n\n🛢️️ Reservatório: Nível crítico atingido!\n_Aviso repetido conforme configurado._";
ultimoAlertaNivel = agora; alertaNivelEnviado = true;
enviarMensagemTelegram(msg, false);
}
} else if (!nivelBaixo && alertaNivelEnviado) {
alertaNivelEnviado = false;
enviarMensagemTelegram("✅ RESOLVIDO: Reservatório REABASTECIDO.", true);
}

// ALERTA DE FLUXO (Intervalo vindo das configurações Web)
bool paneFluxo = (alertaPaneBombaPrecoce || alertaPaneBombaAtraso);
if (paneFluxo) {
if (agora - ultimoAlertaFluxo >= (cfg.intAlertaFluxoMin * 60000UL)) {
String msg = "🚨 ATENÇÃO: PANE NO FLUXO DA BOMBA!\n\n";
if (alertaPaneBombaPrecoce) msg += "⚠️ DESLIGAMENTO PRECOCE!\n";
if (alertaPaneBombaAtraso) msg += "⚙️ FALHA NO ACIONAMENTO!\n";
ultimoAlertaFluxo = agora;
enviarMensagemTelegram(msg, false);
}
}
}

void desenharTelaInicializacao(const char* mensagem = "Inicializando...") {
for (int f = 0; f < 10; f++) {
faseAnimacao++; display.clearDisplay();
textoCentralizado(mensagem, 64, 4, 1);
display.drawFastHLine(0, 15, 128, WHITE);
display.drawBitmap(0, 16, farm_128x48_ok, 128, 48, WHITE);
int progresso = ((faseAnimacao % 10) + 1) * 12;
display.fillRect(2, 62, min(progresso, 124), 1, WHITE);
display.display(); delay(90);
}
}

void conectarWiFi() {
WiFiManager wm; wm.setConfigPortalTimeout(60);
desenharTelaInicializacao("Iniciando...");
if (wm.autoConnect("Horta-Setup")) desenharTelaInicializacao("Conectado!");
else desenharTelaInicializacao("Modo Offline...");
}

void sincronizarHora() { if (WiFi.status() == WL_CONNECTED) configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER_1, NTP_SERVER_2); }

bool atualizarTemperaturaAmbiente() {
if (WiFi.status() != WL_CONNECTED) return false;
HTTPClient http;
String url = "https://api.open-meteo.com/v1/forecast?latitude=" + String(LATITUDE, 6) + "&longitude=" + String(LONGITUDE, 6) + "&current=temperature_2m&timezone=America%2FSao_Paulo";
http.begin(url);
int codigo = http.GET();
if (codigo != HTTP_CODE_OK) { http.end(); return false; }
String resp = http.getString(); http.end();
JsonDocument doc;
if (deserializeJson(doc, resp) || doc["current"]["temperature_2m"].isNull()) return false;
tempAmbiente = doc["current"]["temperature_2m"].as<float>();
return true;
}

// ============================================================
// SETUP & LOOP
// ============================================================

void setup() {
Serial.begin(115200);

// Carrega parâmetros salvos na memória Flash NVS
carregarPreferences();

pinMode(ENCODER_CLK_PIN, INPUT_PULLUP);
pinMode(ENCODER_DT_PIN, INPUT_PULLUP);
pinMode(ENCODER_SW_PIN, INPUT_PULLUP);
attachInterrupt(digitalPinToInterrupt(ENCODER_CLK_PIN), icrEncoderGiro, CHANGE);

pinMode(SENSOR_NIVEL_PIN, INPUT);
pinMode(SENSOR_FLUXO_PIN, INPUT_PULLUP);
pinMode(PIN_RESERVA, INPUT_PULLUP);

sensorDS18B20.begin();
attachInterrupt(digitalPinToInterrupt(SENSOR_FLUXO_PIN), icrContarPulsosFluxo, FALLING);

Wire.begin(OLED_SDA, OLED_SCL);
if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR_1)) display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR_2);

desenharTelaInicializacao("Inicializando...");
conectarWiFi();

if (WiFi.status() == WL_CONNECTED) {
desenharTelaInicializacao("Sincronizando..."); sincronizarHora();
desenharTelaInicializacao("Carregando..."); atualizarTemperaturaAmbiente();

// Inicializa o Servidor Web Assíncrono
configurarRotasWebServer();
}

exibirMenuSelecaoModo();

ultimoTempoTela = millis(); ultimoFrameAnimacao = millis();
ultimoPisca = millis(); ultimoSegundo = millis();
ultimaTentativaWiFi = millis(); ultimaNotificacaoHora = millis();

display.clearDisplay(); desenharPagina(telaAtual, 0); display.display();
}

void loop() {
unsigned long agora = millis();

atualizarEncoder();
atualizarTemperaturaSolucao();
atualizarNivelReservatorio();
calcularFluxoAgua();

if (!notificacaoBootEnviada && WiFi.status() == WL_CONNECTED) {
notificacaoBootEnviada = true;
enviarMensagemTelegram("🚀 Horta Inteligente Ligada (V2.0.0-BETA)!", true);
enviarStatusHorario(true);
}

if (WiFi.status() != WL_CONNECTED && (agora - ultimaTentativaWiFi >= INTERVALO_RECONEXAO_WIFI)) {
ultimaTentativaWiFi = agora; WiFi.reconnect();
}

verificarAlertasInstantaneos();

if (agora - ultimaNotificacaoHora >= INTERVALO_NOTIFICACAO_HORA) {
ultimaNotificacaoHora = agora;
if (WiFi.status() == WL_CONNECTED) enviarStatusHorario(true);
}

if (agora - ultimaAtualizacaoInternet >= INTERVALO_TEMPERATURA) {
ultimaAtualizacaoInternet = agora; atualizarTemperaturaAmbiente();
}

if (agora - ultimoPisca >= TEMPO_PISCA) { ultimoPisca = agora; estadoPisca = !estadoPisca; }
if (agora - ultimoSegundo >= 1000) { ultimoSegundo = agora; doisPontosVisivel = !doisPontosVisivel; }

if (!emTransicao && (agora - ultimoTempoTela >= TEMPO_TELA)) {
iniciarSlide(telaAtual == TELA_DADOS_1 ? TELA_DADOS_2 : TELA_DADOS_1);
}

if (emTransicao) executarSlide();
else if (agora - ultimoFrameAnimacao >= TEMPO_ANIMACAO) {
ultimoFrameAnimacao = agora; faseAnimacao++;
display.clearDisplay(); desenharPagina(telaAtual, 0); display.display();
}

delay(5);
}