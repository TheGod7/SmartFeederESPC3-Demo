#include <Arduino.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_wifi.h> // Necesario para configurar el protocolo PHY de la antena

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

WebServer server(80);
DNSServer dnsServer;
Preferences preferences;

String pendingSsid;
String pendingPassword;
uint32_t connectionStartedAt = 0;
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

String page(const String &message = "") {
    String content = F(
        "<!doctype html><html lang='es'><head>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<meta charset='utf-8'><meta name='theme-color' content='#101827'>"
        "<title>SmartFeeder | Configuración</title>"
        "<style>"
        ":root{color-scheme:dark;--bg:#0b1120;--card:#111b2e;--card2:#17243b;"
        "--line:#263957;--text:#eef4ff;--muted:#9aabc5;--accent:#57d6a0;--blue:#6ea8fe}"
        "*{box-sizing:border-box}body{margin:0;min-height:100vh;background:"
        "radial-gradient(circle at 15% 0,#1d385b 0,transparent 36%),var(--bg);"
        "color:var(--text);font:16px/1.5 system-ui,-apple-system,Segoe UI,sans-serif}"
        ".shell{width:min(100% - 28px,620px);margin:0 auto;padding:28px 0 42px}"
        ".brand{display:flex;align-items:center;gap:14px;margin:8px 0 24px}"
        ".logo{display:grid;place-items:center;width:48px;height:48px;border-radius:15px;"
        "background:linear-gradient(135deg,var(--accent),#2c9bd6);color:#07141a;"
        "font-size:25px;font-weight:800;box-shadow:0 9px 24px #0005}"
        "h1{font-size:1.55rem;letter-spacing:-.03em;margin:0}.subtitle{color:var(--muted);"
        "font-size:.9rem}.card{background:linear-gradient(145deg,#15243b,#10192b);"
        "border:1px solid var(--line);border-radius:22px;padding:22px;margin:14px 0;"
        "box-shadow:0 14px 35px #0003}.eyebrow{color:var(--accent);font-size:.76rem;"
        "font-weight:750;letter-spacing:.1em;text-transform:uppercase}.card h2{font-size:1.1rem;"
        "margin:5px 0 3px}.hint{color:var(--muted);font-size:.9rem;margin:0 0 16px}"
        ".status{display:flex;align-items:center;gap:9px;color:#c9d6e9;font-size:.88rem;"
        "margin:0 0 17px}.dot{width:9px;height:9px;border-radius:50%;background:var(--accent);"
        "box-shadow:0 0 12px var(--accent)}.notice{background:#193a3a;border:1px solid #286a62;"
        "border-radius:13px;color:#b9f4d9;padding:12px 14px;margin-bottom:16px}"
        "label{display:block;color:#cad7e9;font-size:.87rem;font-weight:650;margin:14px 0 6px}"
        "input,button{font:inherit;width:100%;border-radius:12px;padding:12px 14px}"
        "input{background:#0b1425;border:1px solid var(--line);color:var(--text);outline:none}"
        "input:focus{border-color:var(--blue);box-shadow:0 0 0 3px #6ea8fe22}"
        "button{border:0;background:linear-gradient(135deg,#62dbaa,#36a8cb);color:#07151b;"
        "font-weight:750;cursor:pointer;transition:transform .15s,filter .15s;margin-top:12px}"
        "button:hover{filter:brightness(1.1);transform:translateY(-1px)}button.secondary{"
        "background:var(--card2);border:1px solid var(--line);color:var(--text);margin-top:0}"
        ".password{position:relative}.password button{position:absolute;right:5px;top:3px;"
        "width:auto;padding:8px;background:transparent;color:var(--muted);margin:0;box-shadow:none}"
        "#networks{display:grid;gap:8px;margin-top:14px}.network{display:flex;align-items:center;"
        "justify-content:space-between;gap:12px;padding:12px 14px;background:#0d1728;"
        "border:1px solid var(--line);border-radius:13px;cursor:pointer}.network:hover{"
        "border-color:var(--blue);background:#14233b}.network-name{overflow:hidden;text-overflow:"
        "ellipsis;white-space:nowrap}.signal{color:var(--muted);font-size:.78rem;white-space:nowrap}"
        ".empty{color:var(--muted);font-size:.9rem;padding:8px 0}.footer{text-align:center;"
        "color:#687c9b;font-size:.78rem;margin-top:22px}"
        "</style></head><body><main class='shell'><header class='brand'>"
        "<div class='logo'>S</div><div><h1>SmartFeeder</h1>"
        "<div class='subtitle'>Configuración de tu alimentador</div></div></header>");
    if (message.length() > 0) {
        content += F("<div class='notice'>");
        content += htmlEscape(message);
        content += F("</div>");
    }
    content += F(
        "<section class='card'><div class='eyebrow'>Paso 1</div><h2>Busca tu red</h2>"
        "<p class='hint'>Selecciona una red cercana para continuar.</p>"
        "<div class='status'><span class='dot'></span>Punto de configuración activo</div>"
        "<button class='secondary' type='button' onclick='scan()' id='scanButton'>"
        "Escanear redes disponibles</button><div id='networks'>"
        "<div class='empty'>Pulsa el botón para buscar redes Wi-Fi.</div></div></section>"
        "<section class='card'><div class='eyebrow'>Paso 2</div><h2>Conecta SmartFeeder</h2>"
        "<p class='hint'>Los datos se guardarán en la memoria del dispositivo.</p>"
        "<form method='post' action='/connect'><label for='ssid'>Nombre de red</label>"
        "<input id='ssid' name='ssid' placeholder='Selecciona una red' autocomplete='username' required>"
        "<label for='password'>Contraseña</label><div class='password'><input id='password' "
        "name='password' type='password' placeholder='Contraseña de Wi-Fi' autocomplete='current-password'>"
        "<button type='button' onclick='togglePassword()' aria-label='Mostrar contraseña'>Ver</button></div>"
        "<button type='submit'>Conectar a esta red</button></form></section>"
        "<div class='footer'>SmartFeeder · ESP32-C3</div></main>"
        "<script>"
        "const list=document.getElementById('networks'),scanButton=document.getElementById('scanButton');"
        "async function scan(){scanButton.disabled=true;scanButton.textContent='Buscando redes...';"
        "list.innerHTML='<div class=\"empty\">Escaneando el entorno...</div>';"
        "try{const response=await fetch('/scan'),networks=await response.json();list.innerHTML='';"
        "if(!networks.length){list.innerHTML='<div class=\"empty\">No se encontraron redes.</div>';return;}"
        "networks.forEach(n=>{const item=document.createElement('div');item.className='network';"
        "const name=document.createElement('span');name.className='network-name';"
        "name.textContent=n.ssid||'(red oculta)';const signal=document.createElement('span');"
        "signal.className='signal';signal.textContent=n.rssi+' dBm';item.append(name,signal);"
        "item.onclick=()=>{document.getElementById('ssid').value=n.ssid;};list.append(item);});}"
        "catch(e){list.innerHTML='<div class=\"empty\">No se pudo completar el escaneo.</div>';}"
        "finally{scanButton.disabled=false;scanButton.textContent='Escanear redes disponibles';}}"
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
    preferences.end();
    debugLog("Credenciales guardadas correctamente.");
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

    digitalUpdate:
    digitalWrite(LED_BUILTIN, WiFi.status() == WL_CONNECTED ? HIGH : LOW);
    delay(2);
}