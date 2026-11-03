#include <Arduino.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>

#ifndef SMARTFEEDER_DEBUG
#define SMARTFEEDER_DEBUG 1
#endif

namespace {

constexpr char AP_SSID[] = "SmartFeeder-Setup";
constexpr char AP_PASSWORD[] = "smartfeeder";
constexpr byte DNS_PORT = 53;
constexpr uint32_t WIFI_TIMEOUT_MS = 15000;
constexpr uint32_t WIFI_RETRY_INTERVAL_MS = 10000;

WebServer server(80);
DNSServer dnsServer;
Preferences preferences;

String pendingSsid;
String pendingPassword;
uint32_t connectionStartedAt = 0;
uint32_t lastConnectionAttempt = 0;
bool apActive = false;
bool connectionInProgress = false;

void debugLog(const String &message) {
#if SMARTFEEDER_DEBUG
    Serial.println("[DEBUG] " + message);
#else
    (void)message;
#endif
}

String htmlEscape(const String &value) {
    String escaped;
    escaped.reserve(value.length() + 16);
    for (size_t index = 0; index < value.length(); ++index) {
        switch (value[index]) {
        case '&':
            escaped += F("&amp;");
            break;
        case '<':
            escaped += F("&lt;");
            break;
        case '>':
            escaped += F("&gt;");
            break;
        case '"':
            escaped += F("&quot;");
            break;
        case '\'':
            escaped += F("&#39;");
            break;
        default:
            escaped += value[index];
            break;
        }
    }
    return escaped;
}

String jsonEscape(const String &value) {
    String escaped;
    escaped.reserve(value.length() + 8);
    for (size_t index = 0; index < value.length(); ++index) {
        const char character = value[index];
        if (character == '\\' || character == '"') {
            escaped += '\\';
        }
        escaped += character;
    }
    return escaped;
}

String page(const String &message = "") {
    String content = F(
        "<!doctype html><html lang='es'><head>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<meta charset='utf-8'><title>SmartFeeder</title>"
        "<style>body{font-family:Arial;max-width:560px;margin:2rem auto;padding:0 1rem}"
        "input,button{box-sizing:border-box;font-size:1rem;padding:.7rem;width:100%;margin:.35rem 0}"
        "button{cursor:pointer}li{margin:.5rem 0}.ok{color:green}.error{color:#b00020}"
        "</style></head><body><h1>SmartFeeder</h1>");
    if (message.length() > 0) {
        content += F("<p class='ok'>");
        content += htmlEscape(message);
        content += F("</p>");
    }
    content += F(
        "<p>Conecta el dispositivo a una red Wi-Fi.</p>"
        "<button type='button' onclick='scan()'>Escanear redes</button>"
        "<ul id='networks'><li>Pulsa escanear para buscar redes.</li></ul>"
        "<form method='post' action='/connect'>"
        "<label>Nombre de red (SSID)</label><input id='ssid' name='ssid' required>"
        "<label>Contraseña</label><input name='password' type='password'>"
        "<button type='submit'>Conectar</button></form>"
        "<script>"
        "async function scan(){const list=document.getElementById('networks');"
        "list.innerHTML='<li>Escaneando...</li>';"
        "try{const networks=await (await fetch('/scan')).json();list.innerHTML='';"
        "if(!networks.length){list.innerHTML='<li>No se encontraron redes.</li>';return;}"
        "networks.forEach(n=>{const li=document.createElement('li');"
        "const b=document.createElement('button');b.type='button';"
        "b.textContent=n.ssid||'(red oculta)';b.onclick=()=>document.getElementById('ssid').value=n.ssid;"
        "li.appendChild(b);list.appendChild(li);});}"
        "catch(e){list.innerHTML='<li>No se pudo escanear.</li>';}}"
        "</script></body></html>");
    return content;
}

void handleRoot() {
    server.send(200, "text/html; charset=utf-8", page());
}

void handleScan() {
    debugLog("Escaneando redes Wi-Fi");
    const int networkCount = WiFi.scanNetworks();
    String response = "[";
    for (int index = 0; index < networkCount; ++index) {
        if (index > 0) {
            response += ',';
        }
        response += F("{\"ssid\":\"");
        response += jsonEscape(WiFi.SSID(index));
        response += F("\",\"rssi\":");
        response += String(WiFi.RSSI(index));
        response += '}';
    }
    response += ']';
    WiFi.scanDelete();
    server.send(200, "application/json", response);
}

void handleNotFound() {
    if (apActive) {
        server.sendHeader("Location", "http://" + WiFi.softAPIP().toString() + "/", true);
        server.send(302, "text/plain", "");
        return;
    }
    server.send(404, "text/plain", "No encontrado");
}

void beginStationConnection(const String &ssid, const String &password) {
    pendingSsid = ssid;
    pendingPassword = password;
    connectionStartedAt = millis();
    connectionInProgress = true;
    lastConnectionAttempt = millis();
    WiFi.mode(apActive ? WIFI_AP_STA : WIFI_STA);
    WiFi.begin(pendingSsid.c_str(), pendingPassword.c_str());
    debugLog("Intentando conectar a " + pendingSsid);
}

void handleConnect() {
    if (!server.hasArg("ssid") || server.arg("ssid").isEmpty()) {
        server.send(400, "text/html; charset=utf-8", page("Falta el nombre de la red."));
        return;
    }
    beginStationConnection(server.arg("ssid"), server.arg("password"));
    server.send(200, "text/html; charset=utf-8",
                page("Intentando conectar a " + pendingSsid + ". Revisa el monitor serie."));
}

void startAccessPoint() {
    WiFi.mode(WIFI_AP_STA);
    if (!WiFi.softAP(AP_SSID, AP_PASSWORD)) {
        debugLog("No se pudo abrir el punto de acceso");
        return;
    }
    apActive = true;
    dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
    debugLog("Punto de acceso abierto: " + String(AP_SSID));
    debugLog("Abre http://" + WiFi.softAPIP().toString());
}

void stopAccessPoint() {
    if (!apActive) {
        return;
    }
    dnsServer.stop();
    WiFi.softAPdisconnect(true);
    apActive = false;
    debugLog("Punto de acceso cerrado");
}

void saveCredentials() {
    preferences.begin("wifi", false);
    preferences.putString("ssid", pendingSsid);
    preferences.putString("password", pendingPassword);
    preferences.end();
}

bool connectSavedNetwork() {
    preferences.begin("wifi", true);
    const String savedSsid = preferences.getString("ssid", "");
    const String savedPassword = preferences.getString("password", "");
    preferences.end();
    if (savedSsid.isEmpty()) {
        return false;
    }

    debugLog("Red guardada encontrada: " + savedSsid);
    WiFi.mode(WIFI_STA);
    WiFi.begin(savedSsid.c_str(), savedPassword.c_str());
    const uint32_t startedAt = millis();
    while (WiFi.status() != WL_CONNECTED &&
           millis() - startedAt < WIFI_TIMEOUT_MS) {
        delay(100);
    }
    if (WiFi.status() == WL_CONNECTED) {
        debugLog("Se ha conectado al nombre " + savedSsid);
        return true;
    }
    debugLog("No se pudo conectar a la red guardada");
    WiFi.disconnect();
    return false;
}

void processConnection() {
    if (!connectionInProgress) {
        return;
    }
    if (WiFi.status() == WL_CONNECTED) {
        connectionInProgress = false;
        saveCredentials();
        debugLog("Se ha conectado al nombre " + pendingSsid);
        stopAccessPoint();
        return;
    }
    if (millis() - connectionStartedAt >= WIFI_TIMEOUT_MS) {
        connectionInProgress = false;
        WiFi.disconnect();
        debugLog("No se pudo conectar a " + pendingSsid);
        lastConnectionAttempt = millis();
    }
}

void setupServer() {
    server.on("/", HTTP_GET, handleRoot);
    server.on("/scan", HTTP_GET, handleScan);
    server.on("/connect", HTTP_POST, handleConnect);
    server.onNotFound(handleNotFound);
    server.begin();
}

} // namespace

void setup() {
    Serial.begin(115200);
    pinMode(LED_BUILTIN, OUTPUT);
    debugLog("SmartFeeder iniciado");

    if (!connectSavedNetwork()) {
        startAccessPoint();
    }
    setupServer();
}

void loop() {
    if (apActive) {
        dnsServer.processNextRequest();
    }
    server.handleClient();
    processConnection();
    digitalWrite(LED_BUILTIN, WiFi.status() == WL_CONNECTED ? HIGH : LOW);
    delay(10);
}
