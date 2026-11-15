#include <Arduino.h>
#include <ArduinoJson.h>
#include <DNSServer.h>
#include <ESP32Servo.h>
#include <HX711.h>
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
constexpr uint32_t TELEMETRY_INTERVAL_MS = 240000;
constexpr uint32_t COMMAND_POLL_INTERVAL_MS = 60000;
constexpr char DEFAULT_BACKEND_URL[] = "http://192.168.1.65:3000";
constexpr char DEVICE_CODE[] = "SF-ESP32-001";
constexpr char TEST_CODE[] = "SMART-FEEDER-TEST";
constexpr char FIRMWARE_VERSION[] = "0.1.0-debug";
constexpr uint8_t SERVO_PIN = 3;
constexpr uint32_t SERVO_TEST_STEP_MS = 2000;
constexpr uint8_t HX711_DOUT_PIN = 7;
constexpr uint8_t HX711_SCK_PIN = 10;
constexpr float HX711_CALIBRATION_FACTOR = 1.0f;
constexpr char HX711_PREFERENCES_NAMESPACE[] = "scale";
constexpr uint32_t HX711_DEBUG_INTERVAL_MS = 1000;
constexpr uint32_t HX711_RETRY_INTERVAL_MS = 3000;
constexpr uint32_t DISPENSE_STEP_INTERVAL_MS = 1200;
constexpr uint32_t DISPENSE_TIMEOUT_MS = 60000;
constexpr uint8_t DISPENSE_SERVO_PIN_MIN = 0;
constexpr uint8_t DISPENSE_SERVO_PIN_MAX = 180;

WebServer server(80);
DNSServer dnsServer;
Preferences preferences;
Servo dispenserServo;
HX711 loadCell;

String pendingSsid;
String pendingPassword;
String pendingBackendUrl;
String feederId;
uint32_t connectionStartedAt = 0;
bool apActive = false;
bool connectionInProgress = false;
uint32_t lastTelemetryAt = 0;
uint32_t lastCommandPollAt = 0;
uint8_t servoTestStep = 0;
uint32_t servoTestChangedAt = 0;
bool servoTestFinished = false;
bool hx711Ready = false;
uint32_t lastHx711DebugAt = 0;
uint32_t lastHx711RetryAt = 0;
float hx711CalibrationFactor = HX711_CALIBRATION_FACTOR;
bool hx711CalibrationSaved = false;
bool dispenseActive = false;
String activeCommandId;
float dispenseTargetGrams = 0.0f;
float dispenseBaselineGrams = 0.0f;
uint32_t dispenseStartedAt = 0;
uint32_t dispenseNextStepAt = 0;
uint8_t dispenseAngle = DISPENSE_SERVO_PIN_MIN;
uint16_t dispenseTransitions = 0;

void debugLog(const String &message);
String normalizeBackendUrl(String url);
bool sendTelemetry();

void saveHx711Calibration() {
    if (!preferences.begin(HX711_PREFERENCES_NAMESPACE, false)) {
        debugLog("No se pudo guardar la calibracion del HX711");
        return;
    }
    preferences.putFloat("factor", hx711CalibrationFactor);
    preferences.putLong("offset", loadCell.get_offset());
    preferences.end();
    debugLog("Calibracion del HX711 guardada en memoria");
}

void loadHx711Calibration() {
    if (!preferences.begin(HX711_PREFERENCES_NAMESPACE, true)) {
        loadCell.set_scale(hx711CalibrationFactor);
        return;
    }
    hx711CalibrationSaved = preferences.isKey("factor") && preferences.isKey("offset");
    hx711CalibrationFactor = preferences.getFloat("factor", HX711_CALIBRATION_FACTOR);
    const long offset = preferences.getLong("offset", 0);
    preferences.end();

    if (hx711CalibrationFactor == 0.0f) {
        hx711CalibrationFactor = HX711_CALIBRATION_FACTOR;
    }
    loadCell.set_scale(hx711CalibrationFactor);
    loadCell.set_offset(offset);
    debugLog("Calibracion HX711 cargada. Factor: " +
             String(hx711CalibrationFactor, 3) + " | Offset: " + String(offset));
}

void resetHx711Calibration() {
    if (!preferences.begin(HX711_PREFERENCES_NAMESPACE, false)) {
        debugLog("No se pudo abrir la memoria para resetear el HX711");
        return;
    }
    preferences.clear();
    preferences.end();

    hx711CalibrationFactor = HX711_CALIBRATION_FACTOR;
    hx711CalibrationSaved = false;
    loadCell.set_scale(hx711CalibrationFactor);
    loadCell.set_offset(0);
    debugLog("Factor y offset del HX711 eliminados. Factor actual: 1.000");
}

void debugLog(const String &message) {
#if SMARTFEEDER_DEBUG
    Serial.println("[DEBUG] " + message);
#else
    (void)message;
#endif
}

void setServoAngle(const int angle) {
    const int safeAngle = constrain(angle, 0, 180);
    dispenserServo.write(safeAngle);
    debugLog("Servo: " + String(safeAngle) + " grados");
}

bool updateCommandStatus(const String &commandId, const char *status,
                         const char *failureReason = nullptr) {
    if (WiFi.status() != WL_CONNECTED || feederId.isEmpty() ||
        pendingBackendUrl.isEmpty() || commandId.isEmpty()) {
        debugLog("No se pudo actualizar el comando: conexión incompleta");
        return false;
    }

    HTTPClient http;
    const String endpoint = normalizeBackendUrl(pendingBackendUrl) +
                            "/device/commands/" + commandId;
    if (!http.begin(endpoint)) {
        debugLog("No se pudo abrir la URL de estado: " + endpoint);
        return false;
    }

    http.setTimeout(HTTP_TIMEOUT_MS);
    http.useHTTP10(true);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Connection", "close");
    JsonDocument request;
    request["status"] = status;
    if (failureReason != nullptr) {
        request["failureReason"] = failureReason;
    }

    String body;
    serializeJson(request, body);
    const int statusCode = http.PATCH(body);
    const String response = http.getString();
    http.end();
    if (statusCode < 200 || statusCode >= 300) {
        debugLog("No se pudo actualizar el comando. HTTP " + String(statusCode) +
                 ": " + response);
        return false;
    }
    debugLog("Comando " + commandId + " actualizado a " + status);
    return true;
}

bool startDispense(const String &commandId, const float requestedGrams) {
    if (dispenseActive) {
        debugLog("Dispensado rechazado: ya hay otro comando en ejecución");
        return false;
    }
    if (!hx711Ready) {
        updateCommandStatus(commandId, "failed", "HX711 no disponible");
        return false;
    }
    if (requestedGrams <= 0.0f) {
        updateCommandStatus(commandId, "failed", "Gramos solicitados inválidos");
        return false;
    }
    if (!updateCommandStatus(commandId, "processing")) {
        return false;
    }

    activeCommandId = commandId;
    dispenseTargetGrams = requestedGrams;
    dispenseBaselineGrams = loadCell.get_units(5);
    dispenseStartedAt = millis();
    dispenseNextStepAt = dispenseStartedAt + DISPENSE_STEP_INTERVAL_MS;
    dispenseAngle = DISPENSE_SERVO_PIN_MIN;
    dispenseTransitions = 0;
    dispenseActive = true;
    setServoAngle(dispenseAngle);
    debugLog("Dispensado iniciado. Objetivo: " + String(requestedGrams, 1) +
             " g | Base plato: " + String(dispenseBaselineGrams, 1) + " g");
    return true;
}

void finishDispense(const bool completed, const char *failureReason = nullptr) {
    if (!dispenseActive) return;

    setServoAngle(DISPENSE_SERVO_PIN_MIN);
    if (completed) {
        updateCommandStatus(activeCommandId, "completed");
        debugLog("Dispensado completado tras " + String(dispenseTransitions) +
                 " movimientos");
    } else {
        updateCommandStatus(activeCommandId, "failed", failureReason);
        debugLog("Dispensado fallido: " + String(failureReason));
    }
    dispenseActive = false;
    activeCommandId = "";
    dispenseTargetGrams = 0.0f;
    sendTelemetry();
}

void processDispense() {
    if (!dispenseActive || millis() < dispenseNextStepAt) return;

    const float currentGrams = loadCell.get_units(3);
    const float dispensedGrams = currentGrams - dispenseBaselineGrams;
    debugLog("Dispensando | plato: " + String(currentGrams, 1) +
             " g | agregado: " + String(dispensedGrams, 1) +
             " / " + String(dispenseTargetGrams, 1) + " g");

    if (dispensedGrams >= dispenseTargetGrams) {
        finishDispense(true);
        return;
    }
    if (millis() - dispenseStartedAt >= DISPENSE_TIMEOUT_MS) {
        finishDispense(false, "Tiempo máximo de dispensado excedido");
        return;
    }

    dispenseAngle = dispenseAngle == DISPENSE_SERVO_PIN_MIN
                        ? DISPENSE_SERVO_PIN_MAX
                        : DISPENSE_SERVO_PIN_MIN;
    setServoAngle(dispenseAngle);
    ++dispenseTransitions;
    dispenseNextStepAt = millis() + DISPENSE_STEP_INTERVAL_MS;
}

void processSerialCommands() {
    while (Serial.available() > 0) {
        const String command = Serial.readStringUntil('\n');
        String normalized = command;
        normalized.trim();
        normalized.toLowerCase();
        if (normalized.length() == 0) continue;

        if (normalized == "0" || normalized == "90" || normalized == "180") {
            setServoAngle(normalized.toInt());
            servoTestFinished = true;
        } else if (normalized == "test") {
            servoTestStep = 0;
            servoTestChangedAt = millis() - SERVO_TEST_STEP_MS;
            servoTestFinished = false;
            debugLog("Iniciando prueba del servo: 0, 90 y 180 grados");
        } else if (normalized == "hx711 tare") {
            if (!hx711Ready) {
                debugLog("HX711 TARE rechazado: HX711 no esta listo");
            } else {
                debugLog("Retire todo el peso. Ejecutando tara...");
                loadCell.tare(10);
                saveHx711Calibration();
                debugLog("Tara completada");
            }
        } else if (normalized == "hx711 reset") {
            resetHx711Calibration();
        } else if (normalized.startsWith("hx711 calibrate ")) {
            if (!hx711Ready) {
                debugLog("HX711 CALIBRATE rechazado: HX711 no esta listo");
            } else {
                const float knownGrams = normalized.substring(16).toFloat();
                if (knownGrams <= 0.0f) {
                    debugLog("Uso: hx711 calibrate <gramos>, ejemplo: hx711 calibrate 100");
                } else {
                    const long rawWithWeight = loadCell.read_average(20);
                    const long offset = loadCell.get_offset();
                    const long rawDifference = rawWithWeight - offset;
                    if (rawDifference == 0) {
                        debugLog("Calibracion fallida: no hubo cambio de lectura. Revise la celda.");
                    } else {
                        hx711CalibrationFactor =
                            static_cast<float>(rawDifference) / knownGrams;
                        loadCell.set_scale(hx711CalibrationFactor);
                        saveHx711Calibration();
                        debugLog("Calibracion completada. Raw: " + String(rawWithWeight) +
                                 " | Factor: " + String(hx711CalibrationFactor, 3));
                        debugLog("Envie 'hx711 read' para verificar el peso.");
                    }
                }
            }
        } else if (normalized == "hx711 raw") {
            if (!hx711Ready) {
                debugLog("HX711 no esta listo");
            } else {
                debugLog("HX711 raw: " + String(loadCell.read_average(10)));
            }
        } else if (normalized == "hx711 read") {
            if (!hx711Ready) {
                debugLog("HX711 no esta listo");
            } else {
                debugLog("HX711 peso: " + String(loadCell.get_units(10), 3));
            }
        } else if (normalized == "hx711 status") {
            debugLog("HX711 status: " + String(hx711Ready ? "LISTO" : "NO DETECTADO") +
                     " | DT GPIO" + String(HX711_DOUT_PIN) +
                     " | SCK GPIO" + String(HX711_SCK_PIN) +
                     " | factor " + String(hx711CalibrationFactor, 3) +
                     " | offset " + String(loadCell.get_offset()));
        } else if (normalized == "hx711 help" || normalized == "help") {
            debugLog("HX711: hx711 status | hx711 reset | hx711 tare | hx711 raw | hx711 read | hx711 calibrate <gramos>");
        } else {
            debugLog("Comando no reconocido: " + normalized);
        }
    }
}

void processServoTest() {
    if (!dispenserServo.attached()) return;

    if (servoTestFinished || millis() - servoTestChangedAt < SERVO_TEST_STEP_MS) {
        return;
    }

    const uint8_t testAngles[] = {0, 90, 180};
    setServoAngle(testAngles[servoTestStep]);
    servoTestChangedAt = millis();
    ++servoTestStep;
    if (servoTestStep >= 3) {
        servoTestFinished = true;
        debugLog("Prueba terminada. Envie 0, 90, 180 o test por el monitor serie.");
    }
}

void processHx711Debug() {
    const uint32_t now = millis();
    if (!hx711Ready) {
        if (now - lastHx711RetryAt < HX711_RETRY_INTERVAL_MS) return;
        lastHx711RetryAt = now;
        hx711Ready = loadCell.wait_ready_timeout(250);
        if (hx711Ready) {
            debugLog("HX711 detectado despues de reintentar. Envie 'hx711 tare'.");
        } else {
            debugLog("HX711 sin respuesta en DT GPIO" + String(HX711_DOUT_PIN) +
                     ". Revise VCC, GND, DT/DOUT y SCK.");
        }
        return;
    }

    if (now - lastHx711DebugAt < HX711_DEBUG_INTERVAL_MS) return;
    lastHx711DebugAt = now;
    debugLog("HX711 | raw: " + String(loadCell.read_average(3)) +
             " | peso: " + String(loadCell.get_units(3), 3));
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
    request["sensorStatus"] = hx711Ready ? "ready" : "error";
    request["wifiSignal"] = WiFi.RSSI();
    request["firmwareVersion"] = FIRMWARE_VERSION;
    if (hx711Ready) {
        request["plateFoodGrams"] =
            max(0, static_cast<int>(lroundf(loadCell.get_units(3))));
    }

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

bool pollPendingCommands() {
    if (dispenseActive) return true;
    if (WiFi.status() != WL_CONNECTED || feederId.isEmpty() ||
        pendingBackendUrl.isEmpty()) {
        debugLog("Comandos omitidos: falta Wi-Fi, feederId o backend");
        return false;
    }

    HTTPClient http;
    const String endpoint = normalizeBackendUrl(pendingBackendUrl) +
                            "/device/" + feederId + "/commands/pending";
    if (!http.begin(endpoint)) {
        debugLog("No se pudo abrir la URL de comandos: " + endpoint);
        return false;
    }

    http.setTimeout(HTTP_TIMEOUT_MS);
    http.useHTTP10(true);
    http.addHeader("Connection", "close");
    const int statusCode = http.GET();
    const String response = http.getString();
    http.end();

    if (statusCode < 200 || statusCode >= 300) {
        debugLog("Consulta de comandos rechazada. HTTP " + String(statusCode));
        return false;
    }

    JsonDocument result;
    const DeserializationError error = deserializeJson(result, response);
    if (error || !result["data"].is<JsonArray>()) {
        debugLog("Respuesta de comandos invalida");
        return false;
    }

    const JsonArray commands = result["data"].as<JsonArray>();
    debugLog("Comandos pendientes recibidos: " + String(commands.size()));
    for (JsonObject command : commands) {
        const char *commandId = command["id"] | "";
        const char *commandType = command["type"] | "";
        const int requestedGrams = command["requestedGrams"] | 0;
        debugLog("Comando " + String(commandId) + " (" + String(commandType) +
                 ", " + String(requestedGrams) + " g) recibido");
        if (String(commandType) == "dispense") {
            return startDispense(commandId, requestedGrams);
        }
    }
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
            lastTelemetryAt = 0;
            lastCommandPollAt = 0;
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
            lastTelemetryAt = 0;
            lastCommandPollAt = 0;
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
    loadCell.begin(HX711_DOUT_PIN, HX711_SCK_PIN);
    loadHx711Calibration();
    hx711Ready = loadCell.wait_ready_timeout(1000);
    if (hx711Ready) {
        if (!hx711CalibrationSaved) {
            loadCell.tare(10);
            saveHx711Calibration();
            hx711CalibrationSaved = true;
            debugLog("HX711 listo. Tara inicial completada.");
        } else {
            debugLog("HX711 listo. Tara cargada desde memoria.");
        }
    } else {
        debugLog("HX711 no detectado en el arranque. Revise GPIO7=DT, GPIO10=SCK, VCC y GND.");
    }
    dispenserServo.setPeriodHertz(50);
    dispenserServo.attach(SERVO_PIN, 500, 2400);
    debugLog("Prueba del servo: 0, 90 y 180 grados; pausa de 2 segundos.");
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
    processSerialCommands();
    processServoTest();
    processHx711Debug();
    processDispense();

    if (WiFi.status() == WL_CONNECTED && !feederId.isEmpty()) {
        const uint32_t now = millis();
        if (lastTelemetryAt == 0 || now - lastTelemetryAt >= TELEMETRY_INTERVAL_MS) {
            if (sendTelemetry()) {
                lastTelemetryAt = now;
            }
        }
        if (lastCommandPollAt == 0 ||
            now - lastCommandPollAt >= COMMAND_POLL_INTERVAL_MS) {
            if (pollPendingCommands()) {
                lastCommandPollAt = now;
            }
        }
    }

    digitalWrite(LED_BUILTIN, WiFi.status() == WL_CONNECTED ? HIGH : LOW);
    delay(2);
}