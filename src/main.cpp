#include <Arduino.h>
#include <ArduinoJson.h>
#include <DNSServer.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_wifi.h> // Necesario para configurar el protocolo PHY de la antena
#include <time.h>

#ifndef SMARTFEEDER_DEBUG
#define SMARTFEEDER_DEBUG 1
#endif

#ifndef SMARTFEEDER_IGNORE_SAVED_WIFI
#define SMARTFEEDER_IGNORE_SAVED_WIFI 0
#endif

namespace {

constexpr char AP_SSID[] = "SmartFeeder-Setup";
constexpr char AP_PASSWORD[] = "smartfeeder";
constexpr byte DNS_PORT = 53;
constexpr uint8_t AP_CHANNEL = 6; // Cambiado al canal 6 para máxima compatibilidad con teléfonos
constexpr uint32_t WIFI_TIMEOUT_MS = 15000;
constexpr uint32_t HTTP_TIMEOUT_MS = 5000;
constexpr uint32_t TELEMETRY_INTERVAL_MS = 30000;
constexpr char DEFAULT_BACKEND_URL[] = "http://192.168.1.65:3000";
constexpr char DEVICE_CODE[] = "SF-ESP32-001";
constexpr char TEST_CODE[] = "SMART-FEEDER-TEST";
constexpr char FIRMWARE_VERSION[] = "0.1.0-debug";

WebServer server(80);
DNSServer dnsServer;
Preferences preferences;

String pendingSsid;
String pendingPassword;
String pendingBackendUrl;
String feederId;
uint32_t connectionStartedAt = 0;
bool apActive = false;
bool connectionInProgress = false;
uint32_t lastTelemetryAt = 0;

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
        case '&': escaped += F("&amp;"); break;
        case '<': escaped += F("&lt;"); break;
        case '>': escaped += F("&gt;"); break;
        case '"': escaped += F("&quot;"); break;
        case '\'': escaped += F("&#39;"); break;
        default: escaped += value[index]; break;
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

String normalizeBackendUrl(String url) {
    url.trim();
    while (url.endsWith("/")) {
        url.remove(url.length() - 1);
    }
    if (!url.endsWith("/api/v1")) {
        url += "/api/v1";
    }
    return url;
}

String page(const String &message = "") {
    String content = F(
        "<!doctype html><html lang='es'><head>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<meta charset='utf-8'><meta name='theme-color' content='#101827'>"
        "<title>SmartFeeder | Configuración</title>"
        "<style>"
        ":root{color-scheme:dark;--bg:#101318;--card:#191e26;--line:#303744;"
        "--text:#f4f6f8;--muted:#aab2bf;--accent:#7ee2b8;--blue:#83b8ff}"
        "*{box-sizing:border-box}body{margin:0;min-height:100vh;background:var(--bg);"
        "color:var(--text);font:15px/1.45 system-ui,-apple-system,Segoe UI,sans-serif}"
        ".shell{width:min(100% - 32px,480px);margin:0 auto;padding:42px 0}"
        ".brand{display:flex;align-items:center;gap:12px;margin-bottom:28px}"
        ".logo{display:grid;place-items:center;width:38px;height:38px;border-radius:10px;"
        "background:var(--accent);color:#10221b;font-size:21px;font-weight:800}"
        "h1{font-size:1.25rem;letter-spacing:-.02em;margin:0}.subtitle{color:var(--muted);"
        "font-size:.85rem}.card{background:var(--card);border:1px solid var(--line);"
        "border-radius:16px;padding:20px}.card h2{font-size:1.1rem;margin:0 0 5px}"
        ".hint{color:var(--muted);font-size:.88rem;margin:0 0 19px}.notice{background:#19382e;"
        "border:1px solid #34765d;border-radius:10px;color:#c4f4dc;padding:11px 13px;"
        "margin-bottom:14px}label{display:block;color:var(--muted);font-size:.82rem;"
        "font-weight:600;margin:16px 0 6px}select,input,button{font:inherit;width:100%;"
        "border-radius:10px;padding:11px 12px}select,input{background:#11151b;"
        "border:1px solid var(--line);color:var(--text);outline:none}select:focus,input:focus{"
        "border-color:var(--blue);box-shadow:0 0 0 3px #83b8ff22}button{border:0;"
        "background:var(--accent);color:#10221b;font-weight:700;cursor:pointer;margin-top:12px}"
        "button:hover{filter:brightness(1.08)}button.secondary{background:transparent;"
        "border:1px solid var(--line);color:var(--text);margin:0}.row{display:flex;gap:8px}"
        ".row select{flex:1}.row button{width:auto;min-width:102px;margin:0}"
        ".password{display:flex;gap:8px}.password input{flex:1}.password button{width:auto;"
        "margin:0;background:transparent;border:1px solid var(--line);color:var(--muted)}"
        ".status{color:var(--muted);font-size:.8rem;margin-top:16px}.dot{display:inline-block;"
        "width:7px;height:7px;border-radius:50%;background:var(--accent);margin-right:6px}"
        ".footer{text-align:center;color:#707987;font-size:.76rem;margin-top:18px}"
        "</style></head><body><main class='shell'><header class='brand'>"
        "<div class='logo'>S</div><div><h1>SmartFeeder</h1>"
        "<div class='subtitle'>Configura tu conexión Wi-Fi</div></div></header>");
    if (message.length() > 0) {
        content += F("<div class='notice'>");
        content += htmlEscape(message);
        content += F("</div>");
    }
    content += F(
        "<section class='card'><h2>Conecta tu dispositivo</h2>"
        "<p class='hint'>Busca tu red, selecciona una opción y escribe su contraseña.</p>"
        "<form id='wifi-form' method='post' action='/connect'><label for='ssid'>Red Wi-Fi</label>"
        "<div class='row'><select id='ssid' name='ssid' required>"
        "<option value=''>Selecciona una red</option></select>"
        "<button class='secondary' type='button' onclick='scan()' id='scanButton'>Buscar</button></div>"
        "<label for='password'>Contraseña</label><div class='password'><input id='password' "
        "name='password' type='password' placeholder='Contraseña de Wi-Fi' autocomplete='current-password'>"
        "<button type='button' onclick='togglePassword()' aria-label='Mostrar contraseña'>Ver</button></div>"
        "<button type='submit'>Guardar y conectar</button></form>"
        "<div class='status'><span class='dot'></span>El punto de configuración está activo</div></section>"
        "<section class='card'><h2>Backend</h2>"
        "<p class='hint'>Dirección del servidor que registrará este dispositivo.</p>"
        "<label for='backend'>URL del backend</label>"
        "<input id='backend' name='backend' form='wifi-form' value='");
    content += htmlEscape(pendingBackendUrl.isEmpty() ? String(DEFAULT_BACKEND_URL) : pendingBackendUrl);
    content += F("' placeholder='http://192.168.1.50:3000'>"
        "<p class='status'>Usa la IP de tu computadora, no localhost.</p></section>"
        "<div class='footer'>SmartFeeder · ESP32-C3</div></main>"
        "<script>"
        "const select=document.getElementById('ssid'),scanButton=document.getElementById('scanButton');"
        "async function scan(){scanButton.disabled=true;scanButton.textContent='Buscando redes...';"
        "try{const response=await fetch('/scan'),networks=await response.json();"
        "select.innerHTML='<option value=\"\">'+(networks.length?'Selecciona una red':'No se encontraron redes')+'</option>';"
        "networks.forEach(n=>{const option=document.createElement('option');option.value=n.ssid;"
        "option.textContent=n.ssid+' ('+n.rssi+' dBm)';select.appendChild(option);});}"
        "catch(e){select.innerHTML='<option value=\"\">No se pudo escanear</option>';}"
        "finally{scanButton.disabled=false;scanButton.textContent='Buscar';}}"
        "function togglePassword(){const input=document.getElementById('password');"
        "input.type=input.type==='password'?'text':'password';}"
        "</script></body></html>");
    return content;
}

void handleRoot() {
    server.send(200, "text/html; charset=utf-8", page());
}

void handleScan() {
    debugLog("Escaneando redes Wi-Fi...");
    WiFi.mode(WIFI_AP_STA);
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

    WiFi.mode(WIFI_AP_STA);
    WiFi.begin(pendingSsid.c_str(), pendingPassword.c_str());
    debugLog("Intentando conectar a: " + pendingSsid);
}

void handleConnect() {
    if (!server.hasArg("ssid") || server.arg("ssid").isEmpty()) {
        server.send(400, "text/html; charset=utf-8", page("Falta el nombre de la red."));
        return;
    }
    if (!server.hasArg("backend") || server.arg("backend").isEmpty()) {
        server.send(400, "text/html; charset=utf-8", page("Falta la URL del backend."));
        return;
    }
    pendingBackendUrl = normalizeBackendUrl(server.arg("backend"));
    beginStationConnection(server.arg("ssid"), server.arg("password"));
    server.send(200, "text/html; charset=utf-8",
                page("Intentando conectar a " + pendingSsid + ". El dispositivo se reiniciará si tiene éxito."));
}

void startAccessPoint() {
    WiFi.persistent(false);
    WiFi.mode(WIFI_AP);
    delay(100);

    // Forzar protocolo WiFi 802.11g para mejorar compatibilidad con smartphones
    esp_wifi_set_protocol(WIFI_IF_AP, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);

    IPAddress apIp(192, 168, 4, 1);
    IPAddress apGateway(192, 168, 4, 1);
    IPAddress apSubnet(255, 255, 255, 0);

    WiFi.softAPConfig(apIp, apGateway, apSubnet);

    // Abrir el AP en el Canal 6 (fijo y compatible)
    if (!WiFi.softAP(AP_SSID, AP_PASSWORD, AP_CHANNEL, false, 4)) {
        debugLog("Error crítico: No se pudo abrir el punto de acceso");
        return;
    }

    delay(500);
    apActive = true;
    dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());

    debugLog("Punto de acceso abierto con éxito en Canal 6: " + String(AP_SSID));
    debugLog("Portal activo en: http://" + WiFi.softAPIP().toString());
}

void stopAccessPoint() {
    if (!apActive) return;
    dnsServer.stop();
    WiFi.softAPdisconnect(true);
    apActive = false;
    debugLog("Punto de acceso cerrado");
}

void saveCredentials() {
    if (!preferences.begin("wifi", false)) {
        debugLog("Error al abrir preferencias para guardar credenciales");
        return;
    }
    preferences.putString("ssid", pendingSsid);
    preferences.putString("password", pendingPassword);
    preferences.putString("backend", normalizeBackendUrl(pendingBackendUrl));
    if (!feederId.isEmpty()) {
        preferences.putString("feederId", feederId);
    }
    preferences.end();
    debugLog("Credenciales guardadas correctamente.");
}

bool registerDevice() {
    if (pendingBackendUrl.isEmpty()) {
        debugLog("No hay URL de backend configurada");
        return false;
    }

    HTTPClient http;
    const String endpoint = normalizeBackendUrl(pendingBackendUrl) + "/device/connect";
    if (!http.begin(endpoint)) {
        debugLog("No se pudo abrir la URL de registro: " + endpoint);
        return false;
    }

    http.setTimeout(HTTP_TIMEOUT_MS);
    http.useHTTP10(true);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Connection", "close");
    JsonDocument request;
    request["deviceCode"] = DEVICE_CODE;
    request["testCode"] = TEST_CODE;
    String body;
    serializeJson(request, body);

    debugLog("Registrando dispositivo en: " + endpoint);
    const int statusCode = http.POST(body);
    const String response = http.getString();
    if (statusCode < 200 || statusCode >= 300) {
        debugLog("Registro rechazado por backend. HTTP " + String(statusCode));
        http.end();
        return false;
    }

    JsonDocument result;
    const DeserializationError error = deserializeJson(result, response);
    if (error || !result["data"]["feederId"].is<const char *>()) {
        debugLog("Respuesta de registro invalida");
        http.end();
        return false;
    }

    feederId = result["data"]["feederId"].as<String>();
    debugLog("Dispositivo registrado. feederId: " + feederId);
    http.end();
    return true;
}

String currentIsoTimestamp() {
    const time_t now = time(nullptr);
    if (now < 1700000000) {
        return "1970-01-01T00:00:00Z";
    }

    struct tm utcTime;
    gmtime_r(&now, &utcTime);
    char timestamp[25];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", &utcTime);
    return String(timestamp);
}

bool sendTelemetry() {
    if (WiFi.status() != WL_CONNECTED || feederId.isEmpty() || pendingBackendUrl.isEmpty()) {
        debugLog("Telemetria omitida: falta Wi-Fi, feederId o backend");
        return false;
    }

    HTTPClient http;
    const String endpoint = normalizeBackendUrl(pendingBackendUrl) +
                            "/device/" + feederId + "/telemetry";
    if (!http.begin(endpoint)) {
        debugLog("No se pudo abrir la URL de telemetria: " + endpoint);
        return false;
    }

    http.setTimeout(HTTP_TIMEOUT_MS);
    http.addHeader("Content-Type", "application/json");
    JsonDocument request;
    request["reportedAt"] = currentIsoTimestamp();
    request["motorStatus"] = "ready";
    request["sensorStatus"] = "not_calibrated";
    request["wifiSignal"] = WiFi.RSSI();
    request["firmwareVersion"] = FIRMWARE_VERSION;

    String body;
    serializeJson(request, body);
    debugLog("Enviando telemetria de prueba a: " + endpoint);
    const int statusCode = http.POST(body);
    const String response = http.getString();
    http.end();

    if (statusCode < 200 || statusCode >= 300) {
        debugLog("Telemetria rechazada por backend. HTTP " + String(statusCode));
        return false;
    }

    debugLog("Telemetria enviada. HTTP " + String(statusCode) + ": " + response);
    return true;
}

bool connectSavedNetwork() {
#if SMARTFEEDER_DEBUG && SMARTFEEDER_IGNORE_SAVED_WIFI
    debugLog("Modo debug: se ignoran las credenciales Wi-Fi guardadas");
    return false;
#endif

    if (!preferences.begin("wifi", true)) {
        return false;
    }
    const String savedSsid = preferences.getString("ssid", "");
    const String savedPassword = preferences.getString("password", "");
    pendingBackendUrl = preferences.getString("backend", DEFAULT_BACKEND_URL);
    if (pendingBackendUrl.isEmpty() || pendingBackendUrl.indexOf("localhost") >= 0) {
        pendingBackendUrl = DEFAULT_BACKEND_URL;
    }
    feederId = preferences.getString("feederId", "");
    preferences.end();

    if (savedSsid.isEmpty()) {
        return false;
    }

    debugLog("Red guardada encontrada, conectando a: " + savedSsid);
    WiFi.mode(WIFI_STA);
    WiFi.begin(savedSsid.c_str(), savedPassword.c_str());

    const uint32_t startedAt = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startedAt < WIFI_TIMEOUT_MS) {
        delay(100);
    }

    if (WiFi.status() == WL_CONNECTED) {
        debugLog("Conexión exitosa a la red: " + savedSsid);
        configTime(0, 0, "pool.ntp.org", "time.nist.gov");
        if (registerDevice()) {
            saveCredentials();
        }
        return true;
    }

    debugLog("Fallo al conectar con la red guardada.");
    WiFi.disconnect();
    return false;
}

void processConnection() {
    if (!connectionInProgress) return;

    if (WiFi.status() == WL_CONNECTED) {
        connectionInProgress = false;
        saveCredentials();
        debugLog("¡Conectado exitosamente a " + pendingSsid + "!");
        configTime(0, 0, "pool.ntp.org", "time.nist.gov");
        if (registerDevice()) {
            saveCredentials();
        }
        stopAccessPoint();
        delay(1000);
        ESP.restart();
        return;
    }

    if (millis() - connectionStartedAt >= WIFI_TIMEOUT_MS) {
        connectionInProgress = false;
        WiFi.disconnect();
        debugLog("Tiempo de espera agotado. No se pudo conectar a " + pendingSsid);
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
    delay(500);
    pinMode(LED_BUILTIN, OUTPUT);
    debugLog("\n--- SmartFeeder Iniciando ---");

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

    if (WiFi.status() == WL_CONNECTED &&
        (lastTelemetryAt == 0 || millis() - lastTelemetryAt >= TELEMETRY_INTERVAL_MS)) {
        sendTelemetry();
        lastTelemetryAt = millis();
    }

    digitalWrite(LED_BUILTIN, WiFi.status() == WL_CONNECTED ? HIGH : LOW);
    delay(2);
}