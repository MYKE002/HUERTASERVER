#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <DNSServer.h>
#include <SinricPro.h>
#include <SinricProSwitch.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>

// ===========================================
// CONFIGURACIÓN DE PINES
// ===========================================
#define RELAY1_PIN 27
#define RELAY2_PIN 26
#define RELAY3_PIN 25
#define RESET_BUTTON_PIN 0
#define WIFI_STATUS_LED_PIN 2

// ===========================================
// CONFIGURACIÓN DE TIEMPOS
// ===========================================
#define RESET_BUTTON_PRESS_TIME_MS 8000
#define BLINK_INTERVAL_MS 80
#define WIFI_CONNECT_TIMEOUT_MS 30000
#define CAPTIVE_PORTAL_BLINK_MS 100
#define CONNECTION_RETRY_DELAY_MS 5000
#define MQTT_RECONNECT_INTERVAL_MS 5000
#define MQTT_HEARTBEAT_INTERVAL_MS 30000

// ===========================================
// CONFIGURACIÓN SINRIC PRO
// ===========================================
#define APP_KEY     "8c906c74-fc4d-46b7-abf6-02ec6804364d"
#define APP_SECRET  "aad44289-d461-45f4-951a-6429424d0bd1-4c98431c-5045-472c-90e9-956aaa505ae0"
#define DEVICE_ID_RELAY1 "6873b2c9929fca430279f7fa"
#define DEVICE_ID_RELAY2 "6873b4d6f64d827f96b07460"
#define DEVICE_ID_RELAY3 "6873b500030990a558d68a28"

// ===========================================
// CONFIGURACIÓN MQTT
// ===========================================
const char* mqtt_broker   = "broker.emqx.io";
const int   mqtt_port     = 1883;
const char* mqtt_username = "emqx";
const char* mqtt_password = "public";

// Topics MQTT (sensores - originales que ya usabas)
const char* mqtt_display_topic  = "casakevin/display";
const char* mqtt_temp_topic     = "casakevin/sensor/temp";
const char* mqtt_hum_topic      = "casakevin/sensor/hum";
const char* mqtt_distance_topic = "casakevin/sensor/distancia";
const char* mqtt_soil_topic     = "casakevin/sensor/humeda_suelo";

// Topics MQTT (sistema)
const char* mqtt_system_status_topic = "casakevin/system/status";
const char* mqtt_heartbeat_topic     = "casakevin/system/heartbeat";
const char* mqtt_lwt_topic           = "casakevin/system/lwt";

// Topics MQTT (relés - comando y estado)
const char* mqtt_relay1_command_topic = "casakevin/relay1/command";
const char* mqtt_relay1_state_topic   = "casakevin/relay1/state";
const char* mqtt_relay2_command_topic = "casakevin/relay2/command";
const char* mqtt_relay2_state_topic   = "casakevin/relay2/state";
const char* mqtt_relay3_command_topic = "casakevin/relay3/command";
const char* mqtt_relay3_state_topic   = "casakevin/relay3/state";

// ===========================================
// VARIABLES GLOBALES
// ===========================================
WebServer webServer(80);
DNSServer dnsServer;
Preferences preferences;

WiFiClient   wifiClient;
PubSubClient mqttClient(wifiClient);

// WiFi
String storedSsid = "";
String storedPassword = "";
bool wifiConnected = false;
unsigned long lastWifiReconnectAttempt = 0;

// MQTT
bool mqttConnected = false;
unsigned long lastMqttReconnectAttempt = 0;
unsigned long lastMqttHeartbeat = 0;
String clientId = "";

// Reset button
unsigned long resetButtonPressStartTime = 0;
bool resetButtonCurrentlyPressed = false;

// LED status
unsigned long lastBlinkTime = 0;
bool ledState = LOW;

// Estado de relés
struct RelayState {
    bool state;
    String name;
    int pin;
    bool activeHigh;
    const char* commandTopic;
    const char* stateTopic;
};

RelayState relayStates[3] = {
    {false, "Relay 1", RELAY1_PIN, false, mqtt_relay1_command_topic, mqtt_relay1_state_topic},
    {false, "Relay 2", RELAY2_PIN, false, mqtt_relay2_command_topic, mqtt_relay2_state_topic},
    {false, "Relay 3", RELAY3_PIN, true,  mqtt_relay3_command_topic, mqtt_relay3_state_topic}
};

// ===========================================
// PROTOTIPOS DE FUNCIONES
// ===========================================
void checkResetButton();
void updateWifiStatusLed();
void saveWifiCredentials(const String& ssid, const String& password);
bool loadWifiCredentials();
void clearWifiCredentials();
void startCaptivePortal();
void setupWifi();
void setupSinricPro();
void setupPins();
void printSystemInfo();
String getMainHTML();
String getWifiNetworks();

void setupMqtt();
void mqttCallback(char* topic, byte* payload, unsigned int length);
void handleMqttConnection();
bool reconnectMqtt();
void publishAllRelayStates();
void publishSensorData(const char* topic, float value);
void sendHeartbeat();
void setRelayState(int relayIndex, bool state, bool publishMqtt);

void sendTemperatureData(float temperature);
void sendHumidityData(float humidity);
void sendDistanceData(int distance);
void sendSoilMoistureData(int soilMoisture);
void sendAllSensorData(float temperature, float humidity, int distance, int soilMoisture);
void controlRelay1(bool state);
void controlRelay2(bool state);
void controlRelay3(bool state);
void controlAllRelays(bool state);
void printMqttStatus();
void printNetworkStatus();
void printFullSystemStatus();
void handleSerialCommands();
void monitorSystem();
void handleWiFiReconnection();

// ===========================================
// FUNCIONES WIFI Y CREDENCIALES
// ===========================================

void saveWifiCredentials(const String& ssid, const String& password) {
    preferences.begin("wifi_creds", false);
    preferences.putString("ssid", ssid);
    preferences.putString("password", password);
    preferences.end();
    Serial.println("✅ Credenciales WiFi guardadas correctamente.");
}

bool loadWifiCredentials() {
    preferences.begin("wifi_creds", true);
    storedSsid = preferences.getString("ssid", "");
    storedPassword = preferences.getString("password", "");
    preferences.end();

    bool hasCredentials = storedSsid.length() > 0 && storedPassword.length() > 0;
    if (hasCredentials) {
        Serial.println("📡 Credenciales WiFi cargadas desde memoria");
        Serial.printf("SSID: %s\n", storedSsid.c_str());
    }
    return hasCredentials;
}

void clearWifiCredentials() {
    preferences.begin("wifi_creds", false);
    preferences.clear();
    preferences.end();
    Serial.println("🗑️ Credenciales WiFi borradas.");
}

String getWifiNetworks() {
    WiFi.scanDelete();
    int n = WiFi.scanNetworks();
    String networks = "[";

    for (int i = 0; i < n; i++) {
        if (i > 0) networks += ",";
        networks += "{";
        networks += "\"ssid\":\"" + WiFi.SSID(i) + "\",";
        networks += "\"rssi\":" + String(WiFi.RSSI(i)) + ",";
        networks += "\"secure\":" + String(WiFi.encryptionType(i) != WIFI_AUTH_OPEN ? "true" : "false");
        networks += "}";
    }
    networks += "]";

    WiFi.scanDelete();
    return networks;
}

// ===========================================
// PORTAL CAUTIVO
// ===========================================

String getMainHTML() {
    return R"rawliteral(
<!DOCTYPE html>
<html lang="es">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>ESP32 Smart Home - Configuración</title>
    <style>
        * { margin: 0; padding: 0; box-sizing: border-box; }
        :root {
            --primary-color: #00d4ff;
            --secondary-color: #0099cc;
            --accent-color: #ff6b6b;
            --bg-dark: #0a0a0a;
            --bg-card: #1a1a1a;
            --bg-input: #2a2a2a;
            --text-light: #ffffff;
            --text-muted: #888888;
            --border-color: #333333;
            --success-color: #4caf50;
            --warning-color: #ff9800;
            --error-color: #f44336;
        }
        body {
            font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;
            background: linear-gradient(135deg, #0a0a0a 0%, #1a1a1a 100%);
            color: var(--text-light);
            min-height: 100vh;
            position: relative;
            overflow-x: hidden;
        }
        body::before {
            content: '';
            position: fixed;
            top: 0; left: 0; width: 100%; height: 100%;
            background-image:
                radial-gradient(circle at 20% 50%, rgba(0, 212, 255, 0.1) 0%, transparent 50%),
                radial-gradient(circle at 80% 20%, rgba(0, 153, 204, 0.1) 0%, transparent 50%),
                radial-gradient(circle at 40% 80%, rgba(255, 107, 107, 0.1) 0%, transparent 50%);
            animation: backgroundPulse 10s ease-in-out infinite alternate;
            z-index: -1;
        }
        @keyframes backgroundPulse { 0% { opacity: 0.3; } 100% { opacity: 0.7; } }
        .container { max-width: 450px; margin: 0 auto; padding: 20px; min-height: 100vh; display: flex; flex-direction: column; justify-content: center; }
        .card {
            background: var(--bg-card); border-radius: 20px; padding: 30px; margin-bottom: 20px;
            border: 1px solid var(--border-color); box-shadow: 0 10px 40px rgba(0, 0, 0, 0.3);
            backdrop-filter: blur(10px); position: relative; overflow: hidden; animation: slideUp 0.6s ease-out;
        }
        @keyframes slideUp { from { transform: translateY(30px); opacity: 0; } to { transform: translateY(0); opacity: 1; } }
        .header { text-align: center; margin-bottom: 30px; }
        .logo { font-size: 3em; margin-bottom: 10px; }
        .title { font-size: 1.8em; font-weight: 300; margin-bottom: 10px; }
        .subtitle { color: var(--text-muted); font-size: 0.9em; }
        .form-group { margin-bottom: 25px; position: relative; }
        .form-label { display: block; margin-bottom: 8px; font-weight: 500; color: var(--text-light); font-size: 0.9em; }
        .form-input {
            width: 100%; padding: 15px 20px; background: var(--bg-input); border: 1px solid var(--border-color);
            border-radius: 12px; color: var(--text-light); font-size: 16px; transition: all 0.3s ease; outline: none;
        }
        .form-input:focus { border-color: var(--primary-color); box-shadow: 0 0 0 3px rgba(0, 212, 255, 0.1); background: var(--bg-card); }
        .form-input::placeholder { color: var(--text-muted); }
        .wifi-networks { max-height: 200px; overflow-y: auto; margin-bottom: 15px; border: 1px solid var(--border-color); border-radius: 12px; background: var(--bg-input); }
        .wifi-network { padding: 12px 15px; border-bottom: 1px solid var(--border-color); cursor: pointer; transition: all 0.2s ease; display: flex; justify-content: space-between; align-items: center; }
        .wifi-network:last-child { border-bottom: none; }
        .wifi-network:hover { background: var(--bg-card); color: var(--primary-color); }
        .wifi-network.selected { background: var(--primary-color); color: var(--bg-dark); }
        .wifi-signal { font-size: 0.8em; opacity: 0.7; }
        .btn { width: 100%; padding: 15px; border: none; border-radius: 12px; font-size: 16px; font-weight: 600; cursor: pointer; transition: all 0.3s ease; text-transform: uppercase; letter-spacing: 1px; }
        .btn-primary { background: linear-gradient(45deg, var(--primary-color), var(--secondary-color)); color: var(--bg-dark); }
        .btn-primary:hover { transform: translateY(-2px); box-shadow: 0 10px 25px rgba(0, 212, 255, 0.3); }
        .btn-secondary { background: var(--bg-input); color: var(--text-light); border: 1px solid var(--border-color); margin-top: 10px; }
        .btn-secondary:hover { background: var(--border-color); }
        .btn:disabled { opacity: 0.6; cursor: not-allowed; transform: none !important; }
        .loading { display: none; text-align: center; padding: 20px; color: var(--text-muted); }
        .loading.active { display: block; }
        .spinner { width: 30px; height: 30px; border: 3px solid var(--border-color); border-top: 3px solid var(--primary-color); border-radius: 50%; animation: spin 1s linear infinite; margin: 0 auto 15px; }
        @keyframes spin { 0% { transform: rotate(0deg); } 100% { transform: rotate(360deg); } }
        .status { padding: 15px; border-radius: 12px; margin-bottom: 20px; text-align: center; font-weight: 500; animation: slideUp 0.3s ease-out; }
        .status.success { background: rgba(76, 175, 80, 0.1); color: var(--success-color); border: 1px solid rgba(76, 175, 80, 0.3); }
        .status.error { background: rgba(244, 67, 54, 0.1); color: var(--error-color); border: 1px solid rgba(244, 67, 54, 0.3); }
        .hidden { display: none !important; }
        .fade-in { animation: fadeIn 0.5s ease-out; }
        @keyframes fadeIn { from { opacity: 0; } to { opacity: 1; } }
    </style>
</head>
<body>
    <div class="container">
        <div class="card">
            <div class="header">
                <div class="logo">🏠</div>
                <h1 class="title">ESP32 Smart Home</h1>
                <p class="subtitle">Configuración de Red WiFi</p>
            </div>

            <div id="statusMessage"></div>

            <form id="wifiForm">
                <div class="form-group">
                    <label class="form-label">Seleccionar Red WiFi</label>
                    <div id="wifiNetworks" class="wifi-networks">
                        <div class="loading active"><div class="spinner"></div>Escaneando redes WiFi...</div>
                    </div>
                    <button type="button" id="refreshBtn" class="btn btn-secondary">🔄 Actualizar Lista</button>
                </div>

                <div class="form-group">
                    <label class="form-label" for="ssidInput">Nombre de Red (SSID)</label>
                    <input type="text" id="ssidInput" class="form-input" placeholder="Ingresa el SSID manualmente" maxlength="32">
                </div>

                <div class="form-group">
                    <label class="form-label" for="passwordInput">Contraseña WiFi</label>
                    <input type="password" id="passwordInput" class="form-input" placeholder="Contraseña de la red WiFi" required>
                </div>

                <button type="submit" id="connectBtn" class="btn btn-primary">🔗 Conectar y Guardar</button>
            </form>

            <div class="loading" id="connectionLoading">
                <div class="spinner"></div>
                Conectando a la red WiFi...<br>
                <small>El dispositivo se reiniciará automáticamente y luego se conectará a MQTT</small>
            </div>
        </div>
    </div>

    <script>
        class WiFiConfigurator {
            constructor() {
                this.selectedNetwork = null;
                this.initializeEventListeners();
                this.loadWifiNetworks();
            }
            initializeEventListeners() {
                document.getElementById('wifiForm').addEventListener('submit', (e) => this.handleFormSubmit(e));
                document.getElementById('refreshBtn').addEventListener('click', () => this.loadWifiNetworks());
                document.getElementById('ssidInput').addEventListener('input', () => this.clearNetworkSelection());
            }
            async loadWifiNetworks() {
                const networksContainer = document.getElementById('wifiNetworks');
                const refreshBtn = document.getElementById('refreshBtn');
                networksContainer.innerHTML = '<div class="loading active"><div class="spinner"></div>Escaneando redes WiFi...</div>';
                refreshBtn.disabled = true;
                refreshBtn.textContent = '🔄 Escaneando...';
                try {
                    const response = await fetch('/api/scan');
                    const networks = await response.json();
                    if (networks.length === 0) {
                        networksContainer.innerHTML = '<div style="padding: 20px; text-align: center; color: var(--text-muted);">No se encontraron redes WiFi</div>';
                        return;
                    }
                    networksContainer.innerHTML = '';
                    networks.forEach((network, index) => {
                        const networkElement = document.createElement('div');
                        networkElement.className = 'wifi-network';
                        networkElement.innerHTML = `<span>${network.ssid}</span><span class="wifi-signal">${network.secure ? '🔒' : '🔓'}</span>`;
                        networkElement.addEventListener('click', () => this.selectNetwork(network, networkElement));
                        networksContainer.appendChild(networkElement);
                        setTimeout(() => { networkElement.classList.add('fade-in'); }, index * 50);
                    });
                } catch (error) {
                    networksContainer.innerHTML = '<div style="padding: 20px; text-align: center; color: var(--error-color);">Error al escanear redes</div>';
                } finally {
                    refreshBtn.disabled = false;
                    refreshBtn.textContent = '🔄 Actualizar Lista';
                }
            }
            selectNetwork(network, element) {
                document.querySelectorAll('.wifi-network').forEach(el => el.classList.remove('selected'));
                element.classList.add('selected');
                this.selectedNetwork = network;
                document.getElementById('ssidInput').value = network.ssid;
            }
            clearNetworkSelection() {
                document.querySelectorAll('.wifi-network').forEach(el => el.classList.remove('selected'));
                this.selectedNetwork = null;
            }
            showStatus(message, type = 'info') {
                const statusDiv = document.getElementById('statusMessage');
                statusDiv.innerHTML = `<div class="status ${type}">${message}</div>`;
                if (type === 'success' || type === 'error') {
                    setTimeout(() => { statusDiv.innerHTML = ''; }, 5000);
                }
            }
            async handleFormSubmit(e) {
                e.preventDefault();
                const ssid = document.getElementById('ssidInput').value.trim();
                const password = document.getElementById('passwordInput').value;
                if (!ssid) { this.showStatus('Por favor selecciona o ingresa un SSID', 'error'); return; }
                if (ssid.length > 32) { this.showStatus('El SSID no puede tener más de 32 caracteres', 'error'); return; }
                document.getElementById('wifiForm').classList.add('hidden');
                document.getElementById('connectionLoading').classList.add('active');
                try {
                    const response = await fetch('/save', {
                        method: 'POST',
                        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
                        body: `ssid=${encodeURIComponent(ssid)}&password=${encodeURIComponent(password)}`
                    });
                    if (response.ok) {
                        this.showStatus('✅ Configuración guardada exitosamente. Reiniciando...', 'success');
                        let countdown = 5;
                        const countdownInterval = setInterval(() => {
                            document.getElementById('connectionLoading').innerHTML = `<div class="spinner"></div>Reiniciando en ${countdown} segundos...<br><small>El dispositivo se conectará a WiFi y MQTT</small>`;
                            countdown--;
                            if (countdown < 0) { clearInterval(countdownInterval); window.location.reload(); }
                        }, 1000);
                    } else { throw new Error('Error en la respuesta del servidor'); }
                } catch (error) {
                    this.showStatus('❌ Error al guardar la configuración', 'error');
                    document.getElementById('wifiForm').classList.remove('hidden');
                    document.getElementById('connectionLoading').classList.remove('active');
                }
            }
        }
        document.addEventListener('DOMContentLoaded', () => { new WiFiConfigurator(); });
    </script>
</body>
</html>)rawliteral";
}

void startCaptivePortal() {
    Serial.println("🌐 Iniciando portal cautivo...");

    WiFi.mode(WIFI_AP);
    WiFi.softAP("ESP32_SmartHome_Setup", "", 1, 0, 4);

    IPAddress apIP(192, 168, 4, 1);
    IPAddress gateway(192, 168, 4, 1);
    IPAddress subnet(255, 255, 255, 0);
    WiFi.softAPConfig(apIP, gateway, subnet);

    dnsServer.start(53, "*", apIP);

    webServer.on("/", []() {
        webServer.send(200, "text/html", getMainHTML());
    });

    webServer.on("/api/scan", []() {
        webServer.sendHeader("Access-Control-Allow-Origin", "*");
        webServer.send(200, "application/json", getWifiNetworks());
    });

    webServer.on("/api/status", []() {
        DynamicJsonDocument doc(512);
        doc["wifi_connected"] = wifiConnected;
        doc["ssid"] = storedSsid;
        doc["ip"] = wifiConnected ? WiFi.localIP().toString() : "";
        doc["rssi"] = wifiConnected ? WiFi.RSSI() : 0;
        doc["free_heap"] = ESP.getFreeHeap();
        doc["uptime"] = millis();

        String response;
        serializeJson(doc, response);
        webServer.sendHeader("Access-Control-Allow-Origin", "*");
        webServer.send(200, "application/json", response);
    });

    webServer.on("/save", HTTP_POST, []() {
        if (webServer.hasArg("ssid") && webServer.hasArg("password")) {
            String ssid = webServer.arg("ssid");
            String password = webServer.arg("password");

            if (ssid.length() > 0 && ssid.length() <= 32) {
                saveWifiCredentials(ssid, password);
                webServer.send(200, "text/plain", "OK");
                delay(2000);
                ESP.restart();
            } else {
                webServer.send(400, "text/plain", "SSID inválido");
            }
        } else {
            webServer.send(400, "text/plain", "Parámetros faltantes");
        }
    });

    webServer.onNotFound([]() {
        webServer.sendHeader("Location", String("http://") + WiFi.softAPIP().toString());
        webServer.send(302, "text/plain", "");
    });

    webServer.begin();
    Serial.printf("🚀 Portal cautivo iniciado: http://%s\n", WiFi.softAPIP().toString().c_str());

    unsigned long portalBlinkTime = 0;
    bool portalLedState = LOW;

    while (true) {
        dnsServer.processNextRequest();
        webServer.handleClient();
        checkResetButton();

        if (millis() - portalBlinkTime >= CAPTIVE_PORTAL_BLINK_MS) {
            portalBlinkTime = millis();
            portalLedState = !portalLedState;
            digitalWrite(WIFI_STATUS_LED_PIN, portalLedState);
        }

        delay(10);
    }
}

// ===========================================
// CONFIGURACIÓN WIFI
// ===========================================

void setupWifi() {
    Serial.println("🔌 Configurando WiFi...");
    digitalWrite(WIFI_STATUS_LED_PIN, LOW);

    if (loadWifiCredentials()) {
        WiFi.mode(WIFI_STA);
        WiFi.setHostname("ESP32-SmartHome");
        WiFi.begin(storedSsid.c_str(), storedPassword.c_str());

        Serial.printf("Conectando a: %s", storedSsid.c_str());

        unsigned long startTime = millis();
        int attempts = 0;

        while (WiFi.status() != WL_CONNECTED && (millis() - startTime) < WIFI_CONNECT_TIMEOUT_MS) {
            updateWifiStatusLed();
            checkResetButton();

            if (millis() - startTime > (attempts + 1) * 5000) {
                attempts++;
                Serial.printf("\n🔄 Intento %d de conectar a WiFi...", attempts);
            }

            Serial.print(".");
            delay(500);
        }

        if (WiFi.status() == WL_CONNECTED) {
            Serial.println();
            Serial.println("✅ WiFi conectado exitosamente!");
            Serial.printf("📍 IP: %s\n", WiFi.localIP().toString().c_str());
            Serial.printf("📡 RSSI: %d dBm\n", WiFi.RSSI());

            wifiConnected = true;
            digitalWrite(WIFI_STATUS_LED_PIN, HIGH);
            lastWifiReconnectAttempt = 0;
        } else {
            Serial.println();
            Serial.println("❌ Error al conectar WiFi. Iniciando portal cautivo...");
            wifiConnected = false;
            startCaptivePortal();
        }
    } else {
        Serial.println("📋 No hay credenciales guardadas. Iniciando portal cautivo...");
        startCaptivePortal();
    }
}

// ===========================================
// FUNCIONES MQTT
// ===========================================

void setupMqtt() {
    clientId = "ESP32SmartHome-" + WiFi.macAddress();
    clientId.replace(":", "");

    mqttClient.setServer(mqtt_broker, mqtt_port);
    mqttClient.setCallback(mqttCallback);
    mqttClient.setBufferSize(512);

    Serial.println("🟦 MQTT configurado");
    Serial.printf("  Broker: %s:%d\n", mqtt_broker, mqtt_port);
    Serial.printf("  Client ID: %s\n", clientId.c_str());

    reconnectMqtt();
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
    String message;
    for (unsigned int i = 0; i < length; i++) {
        message += (char)payload[i];
    }
    message.trim();

    String topicStr = String(topic);
    Serial.printf("📥 MQTT recibido - Topic: %s, Mensaje: %s\n", topic, message.c_str());

    bool state = (message == "ON" || message == "1" || message == "true" || message.equalsIgnoreCase("on"));

    if (topicStr == mqtt_relay1_command_topic) {
        setRelayState(0, state, true);
    } else if (topicStr == mqtt_relay2_command_topic) {
        setRelayState(1, state, true);
    } else if (topicStr == mqtt_relay3_command_topic) {
        setRelayState(2, state, true);
    }
}

bool reconnectMqtt() {
    if (!wifiConnected) return false;

    Serial.println("🔗 Conectando a MQTT...");

    bool connected = mqttClient.connect(
        clientId.c_str(),
        mqtt_username,
        mqtt_password,
        mqtt_lwt_topic,   // Last Will topic
        1,                // QoS
        true,             // retain
        "offline"         // Last Will message
    );

    if (connected) {
        Serial.println("✅ MQTT conectado exitosamente!");
        mqttConnected = true;

        // Suscribirse a topics de comando de los relés
        mqttClient.subscribe(mqtt_relay1_command_topic);
        mqttClient.subscribe(mqtt_relay2_command_topic);
        mqttClient.subscribe(mqtt_relay3_command_topic);

        // Publicar estado online (retenido)
        mqttClient.publish(mqtt_lwt_topic, "online", true);
        mqttClient.publish(mqtt_system_status_topic, "connected", true);

        // Publicar estados actuales de los relés
        publishAllRelayStates();

        Serial.println("📡 Suscrito a topics de comando de relés");
    } else {
        Serial.printf("❌ Fallo conexión MQTT, rc=%d\n", mqttClient.state());
        mqttConnected = false;
    }

    return connected;
}

void handleMqttConnection() {
    if (!wifiConnected) return;

    if (!mqttClient.connected()) {
        mqttConnected = false;
        unsigned long currentTime = millis();

        if (currentTime - lastMqttReconnectAttempt >= MQTT_RECONNECT_INTERVAL_MS) {
            lastMqttReconnectAttempt = currentTime;
            reconnectMqtt();
        }
    } else {
        mqttClient.loop();

        // Heartbeat periódico
        if (millis() - lastMqttHeartbeat >= MQTT_HEARTBEAT_INTERVAL_MS) {
            lastMqttHeartbeat = millis();
            sendHeartbeat();
        }
    }
}

void publishAllRelayStates() {
    if (!mqttConnected || !mqttClient.connected()) return;

    for (int i = 0; i < 3; i++) {
        mqttClient.publish(relayStates[i].stateTopic,
                            relayStates[i].state ? "ON" : "OFF", true);
    }
}

void publishSensorData(const char* topic, float value) {
    if (!mqttConnected || !mqttClient.connected()) return;

    char valueStr[16];
    dtostrf(value, 4, 2, valueStr);

    if (mqttClient.publish(topic, valueStr)) {
        Serial.printf("📤 Publicado - Topic: %s, Valor: %s\n", topic, valueStr);
    }
}

void sendHeartbeat() {
    if (!mqttConnected || !mqttClient.connected()) return;

    DynamicJsonDocument doc(256);
    doc["device_id"] = clientId;
    doc["uptime"] = millis();
    doc["free_heap"] = ESP.getFreeHeap();
    doc["wifi_rssi"] = WiFi.RSSI();
    doc["ip"] = WiFi.localIP().toString();

    JsonArray relays = doc.createNestedArray("relays");
    for (int i = 0; i < 3; i++) {
        JsonObject relay = relays.createNestedObject();
        relay["name"] = relayStates[i].name;
        relay["state"] = relayStates[i].state;
    }

    String jsonString;
    serializeJson(doc, jsonString);
    mqttClient.publish(mqtt_heartbeat_topic, jsonString.c_str());
}

// ===========================================
// CONTROL UNIFICADO DE RELÉS
// ===========================================

void setRelayState(int relayIndex, bool state, bool publishMqtt) {
    if (relayIndex < 0 || relayIndex > 2) return;

    relayStates[relayIndex].state = state;

    bool pinState = relayStates[relayIndex].activeHigh ? state : !state;
    digitalWrite(relayStates[relayIndex].pin, pinState);

    Serial.printf("🔌 %s (Pin %d): %s\n",
                  relayStates[relayIndex].name.c_str(),
                  relayStates[relayIndex].pin,
                  state ? "ACTIVADO" : "DESACTIVADO");

    if (publishMqtt && mqttConnected && mqttClient.connected()) {
        mqttClient.publish(relayStates[relayIndex].stateTopic, state ? "ON" : "OFF", true);
    }

    // Mantener sincronizado con Sinric Pro
    const char* deviceIds[3] = {DEVICE_ID_RELAY1, DEVICE_ID_RELAY2, DEVICE_ID_RELAY3};
    SinricProSwitch &device = SinricPro[deviceIds[relayIndex]];
    device.sendPowerStateEvent(state);
}

// ===========================================
// FUNCIONES DE ESTADO
// ===========================================

void updateWifiStatusLed() {
    if (WiFi.status() == WL_CONNECTED) {
        digitalWrite(WIFI_STATUS_LED_PIN, HIGH);
    } else {
        if (millis() - lastBlinkTime >= BLINK_INTERVAL_MS) {
            lastBlinkTime = millis();
            ledState = !ledState;
            digitalWrite(WIFI_STATUS_LED_PIN, ledState);
        }
    }
}

void checkResetButton() {
    int buttonState = digitalRead(RESET_BUTTON_PIN);

    if (buttonState == LOW && !resetButtonCurrentlyPressed) {
        resetButtonPressStartTime = millis();
        resetButtonCurrentlyPressed = true;
        Serial.println("🔄 Botón RESET presionado...");
    }
    else if (buttonState == LOW && resetButtonCurrentlyPressed) {
        unsigned long pressTime = millis() - resetButtonPressStartTime;
        if (pressTime >= RESET_BUTTON_PRESS_TIME_MS) {
            Serial.println("🔄 Reset iniciado - Borrando credenciales...");
            clearWifiCredentials();

            for (int i = 0; i < 10; i++) {
                digitalWrite(WIFI_STATUS_LED_PIN, HIGH);
                delay(100);
                digitalWrite(WIFI_STATUS_LED_PIN, LOW);
                delay(100);
            }

            delay(1000);
            ESP.restart();
        }
    }
    else if (buttonState == HIGH && resetButtonCurrentlyPressed) {
        resetButtonCurrentlyPressed = false;
        unsigned long pressTime = millis() - resetButtonPressStartTime;
        Serial.printf("🔄 Botón RESET liberado después de %lu ms\n", pressTime);
    }
}

// ===========================================
// SINRIC PRO
// ===========================================

bool onPowerState(String deviceId, bool &state) {
    Serial.printf("🎛️ Comando Sinric Pro - Device: %s, Estado: %s\n",
                  deviceId.c_str(), state ? "ON" : "OFF");

    int relayIndex = -1;

    if (deviceId == DEVICE_ID_RELAY1) relayIndex = 0;
    else if (deviceId == DEVICE_ID_RELAY2) relayIndex = 1;
    else if (deviceId == DEVICE_ID_RELAY3) relayIndex = 2;
    else {
        Serial.printf("⚠️ ID de dispositivo desconocido: %s\n", deviceId.c_str());
        return false;
    }

    // No re-publicamos a Sinric Pro (evita loop), pero sí a MQTT
    relayStates[relayIndex].state = state;
    bool pinState = relayStates[relayIndex].activeHigh ? state : !state;
    digitalWrite(relayStates[relayIndex].pin, pinState);

    if (mqttConnected && mqttClient.connected()) {
        mqttClient.publish(relayStates[relayIndex].stateTopic, state ? "ON" : "OFF", true);
    }

    Serial.printf("🔌 %s (Pin %d): %s\n",
                  relayStates[relayIndex].name.c_str(),
                  relayStates[relayIndex].pin,
                  state ? "ACTIVADO" : "DESACTIVADO");

    return true;
}

void setupSinricPro() {
    Serial.println("🔗 Configurando Sinric Pro...");

    SinricProSwitch &relay1 = SinricPro[DEVICE_ID_RELAY1];
    SinricProSwitch &relay2 = SinricPro[DEVICE_ID_RELAY2];
    SinricProSwitch &relay3 = SinricPro[DEVICE_ID_RELAY3];

    relay1.onPowerState(onPowerState);
    relay2.onPowerState(onPowerState);
    relay3.onPowerState(onPowerState);

    SinricPro.begin(APP_KEY, APP_SECRET);
    SinricPro.restoreDeviceStates(true);

    Serial.println("✅ Sinric Pro configurado correctamente");
}

// ===========================================
// CONFIGURACIÓN DE PINES
// ===========================================

void setupPins() {
    Serial.println("📌 Configurando pines...");

    pinMode(RELAY1_PIN, OUTPUT);
    pinMode(RELAY2_PIN, OUTPUT);
    pinMode(RELAY3_PIN, OUTPUT);
    pinMode(WIFI_STATUS_LED_PIN, OUTPUT);
    pinMode(RESET_BUTTON_PIN, INPUT_PULLUP);

    for (int i = 0; i < 3; i++) {
        bool initialState = relayStates[i].activeHigh ? false : true;
        digitalWrite(relayStates[i].pin, initialState);
        Serial.printf("📌 %s (Pin %d) configurado como %s\n",
                      relayStates[i].name.c_str(),
                      relayStates[i].pin,
                      relayStates[i].activeHigh ? "ACTIVE HIGH" : "ACTIVE LOW");
    }

    digitalWrite(WIFI_STATUS_LED_PIN, LOW);

    Serial.println("✅ Pines configurados correctamente");
}

// ===========================================
// INFORMACIÓN DEL SISTEMA
// ===========================================

void printSystemInfo() {
    Serial.println("\n==================================================");
    Serial.println("🏠 ESP32 SMART HOME CONTROLLER v2.0 + MQTT");
    Serial.println("==================================================");
    Serial.printf("📱 Chip: %s\n", ESP.getChipModel());
    Serial.printf("💾 RAM libre: %d bytes\n", ESP.getFreeHeap());
    Serial.printf("📡 WiFi MAC: %s\n", WiFi.macAddress().c_str());
    Serial.println("--------------------------------------------------");
    Serial.println("📋 CONFIGURACIÓN DE HARDWARE:");
    Serial.printf("  🔌 Relay 1: Pin %d (ACTIVE LOW)\n", RELAY1_PIN);
    Serial.printf("  🔌 Relay 2: Pin %d (ACTIVE LOW)\n", RELAY2_PIN);
    Serial.printf("  🔌 Relay 3: Pin %d (ACTIVE HIGH)\n", RELAY3_PIN);
    Serial.println("--------------------------------------------------");
    Serial.println("🟦 CONFIGURACIÓN MQTT:");
    Serial.printf("  Broker: %s:%d\n", mqtt_broker, mqtt_port);
    Serial.printf("  Usuario: %s\n", mqtt_username);
    Serial.println("==================================================");
}

// ===========================================
// FUNCIONES DE MONITOREO
// ===========================================

void monitorSystem() {
    static unsigned long lastMonitorTime = 0;
    const unsigned long monitorInterval = 60000;

    if (millis() - lastMonitorTime >= monitorInterval) {
        lastMonitorTime = millis();

        Serial.println("\n📊 ESTADO DEL SISTEMA:");
        Serial.printf("  ⏱️ Uptime: %lu ms\n", millis());
        Serial.printf("  💾 RAM libre: %d bytes\n", ESP.getFreeHeap());
        Serial.printf("  📡 WiFi: %s\n", WiFi.isConnected() ? "CONECTADO" : "DESCONECTADO");
        Serial.printf("  🟦 MQTT: %s\n", mqttConnected ? "CONECTADO" : "DESCONECTADO");

        if (WiFi.isConnected()) {
            Serial.printf("  📶 RSSI: %d dBm\n", WiFi.RSSI());
            Serial.printf("  📍 IP: %s\n", WiFi.localIP().toString().c_str());
        }

        Serial.println("  🔌 Estados de Relés:");
        for (int i = 0; i < 3; i++) {
            Serial.printf("    %s: %s\n", relayStates[i].name.c_str(),
                          relayStates[i].state ? "ON" : "OFF");
        }
    }
}

void handleWiFiReconnection() {
    if (!wifiConnected && WiFi.status() != WL_CONNECTED) {
        unsigned long currentTime = millis();

        if (currentTime - lastWifiReconnectAttempt >= CONNECTION_RETRY_DELAY_MS) {
            lastWifiReconnectAttempt = currentTime;

            Serial.println("🔄 Intentando reconectar WiFi...");
            WiFi.disconnect();
            delay(1000);
            WiFi.begin(storedSsid.c_str(), storedPassword.c_str());

            unsigned long reconnectStart = millis();
            while (WiFi.status() != WL_CONNECTED && (millis() - reconnectStart) < 10000) {
                updateWifiStatusLed();
                delay(500);
            }

            if (WiFi.status() == WL_CONNECTED) {
                Serial.println("✅ WiFi reconectado exitosamente!");
                wifiConnected = true;
                digitalWrite(WIFI_STATUS_LED_PIN, HIGH);

                if (!SinricPro.isConnected()) {
                    Serial.println("🔁 Reconectando con SinricPro...");
                    SinricPro.begin(APP_KEY, APP_SECRET);
                    SinricPro.restoreDeviceStates(true);
                }

                lastMqttReconnectAttempt = 0; // forzar reconexión MQTT
            } else {
                Serial.println("❌ Fallo al reconectar WiFi");
            }
        }
    }
}

// ===========================================
// FUNCIONES AUXILIARES PARA SENSORES
// ===========================================

void sendTemperatureData(float temperature) {
    publishSensorData(mqtt_temp_topic, temperature);
}

void sendHumidityData(float humidity) {
    publishSensorData(mqtt_hum_topic, humidity);
}

void sendDistanceData(int distance) {
    publishSensorData(mqtt_distance_topic, distance);
}

void sendSoilMoistureData(int soilMoisture) {
    publishSensorData(mqtt_soil_topic, soilMoisture);
}

void sendAllSensorData(float temperature, float humidity, int distance, int soilMoisture) {
    if (!mqttConnected || !mqttClient.connected()) return;

    DynamicJsonDocument doc(256);
    doc["device_id"] = clientId;
    doc["timestamp"] = millis();
    doc["temperature"] = temperature;
    doc["humidity"] = humidity;
    doc["distance"] = distance;
    doc["soil_moisture"] = soilMoisture;

    String jsonString;
    serializeJson(doc, jsonString);

    const char* combined_topic = "casakevin/sensors/all";
    mqttClient.publish(combined_topic, jsonString.c_str());

    publishSensorData(mqtt_temp_topic, temperature);
    publishSensorData(mqtt_hum_topic, humidity);
    publishSensorData(mqtt_distance_topic, distance);
    publishSensorData(mqtt_soil_topic, soilMoisture);
}

// ===========================================
// FUNCIONES DE CONTROL REMOTO
// ===========================================

void controlRelay1(bool state) { setRelayState(0, state, true); }
void controlRelay2(bool state) { setRelayState(1, state, true); }
void controlRelay3(bool state) { setRelayState(2, state, true); }

void controlAllRelays(bool state) {
    for (int i = 0; i < 3; i++) {
        setRelayState(i, state, true);
        delay(100);
    }
    Serial.printf("🔌 Todos los relés: %s\n", state ? "ACTIVADOS" : "DESACTIVADOS");
}

// ===========================================
// DIAGNÓSTICO Y DEBUG
// ===========================================

void printMqttStatus() {
    Serial.println("\n🟦 ESTADO MQTT DETALLADO:");
    Serial.printf("  🔗 Cliente conectado: %s\n", mqttClient.connected() ? "SI" : "NO");
    Serial.printf("  📡 Broker: %s:%d\n", mqtt_broker, mqtt_port);
    Serial.printf("  🆔 Client ID: %s\n", clientId.c_str());
    Serial.printf("  📊 Estado cliente: %d\n", mqttClient.state());

    Serial.println("  📡 Topics de relés:");
    for (int i = 0; i < 3; i++) {
        Serial.printf("    %s -> cmd: %s | state: %s\n",
                      relayStates[i].name.c_str(),
                      relayStates[i].commandTopic,
                      relayStates[i].stateTopic);
    }
}

void printNetworkStatus() {
    Serial.println("\n📡 ESTADO DE RED:");
    Serial.printf("  WiFi Connected: %s\n", wifiConnected ? "SI" : "NO");
    Serial.printf("  IP Address: %s\n", WiFi.localIP().toString().c_str());
    Serial.printf("  RSSI: %d dBm\n", WiFi.RSSI());
    Serial.printf("  MAC: %s\n", WiFi.macAddress().c_str());
}

void printFullSystemStatus() {
    Serial.println("\n============================================================");
    Serial.println("📊 ESTADO COMPLETO DEL SISTEMA");
    Serial.println("============================================================");

    printNetworkStatus();
    printMqttStatus();

    Serial.println("\n🔌 ESTADO DE RELÉS:");
    for (int i = 0; i < 3; i++) {
        Serial.printf("  %s (Pin %d): %s [%s]\n",
                      relayStates[i].name.c_str(),
                      relayStates[i].pin,
                      relayStates[i].state ? "ON" : "OFF",
                      relayStates[i].activeHigh ? "ACTIVE HIGH" : "ACTIVE LOW");
    }

    Serial.printf("\n💾 Heap libre: %d bytes | Uptime: %lu ms\n", ESP.getFreeHeap(), millis());
    Serial.println("============================================================");
}

void handleSerialCommands() {
    if (Serial.available()) {
        String command = Serial.readStringUntil('\n');
        command.trim();
        command.toLowerCase();

        if (command == "status") {
            printFullSystemStatus();
        }
        else if (command == "mqtt") {
            printMqttStatus();
        }
        else if (command == "network" || command == "wifi") {
            printNetworkStatus();
        }
        else if (command == "restart") {
            Serial.println("🔄 Reiniciando sistema...");
            ESP.restart();
        }
        else if (command == "reset") {
            Serial.println("🔄 Borrando credenciales WiFi...");
            clearWifiCredentials();
            ESP.restart();
        }
        else if (command.startsWith("relay")) {
            int spaceIndex = command.indexOf(' ');
            if (spaceIndex > 0) {
                String relayStr = command.substring(5, spaceIndex);
                String stateStr = command.substring(spaceIndex + 1);
                int relayIndex = relayStr.toInt() - 1;
                bool state = (stateStr == "on" || stateStr == "1");
                if (relayIndex >= 0 && relayIndex < 3) {
                    setRelayState(relayIndex, state, true);
                }
            }
        }
        else if (command == "test") {
            Serial.println("🧪 Iniciando test de relés...");
            for (int i = 0; i < 3; i++) {
                setRelayState(i, true, true);
                delay(1000);
                setRelayState(i, false, true);
                delay(500);
            }
            Serial.println("🧪 Test completado");
        }
        else if (command == "help") {
            Serial.println("\n📋 COMANDOS DISPONIBLES:");
            Serial.println("  status    - Estado completo del sistema");
            Serial.println("  mqtt      - Estado de MQTT");
            Serial.println("  network   - Estado de red WiFi");
            Serial.println("  restart   - Reiniciar sistema");
            Serial.println("  reset     - Borrar WiFi y reiniciar");
            Serial.println("  relay1 on - Activar relé 1");
            Serial.println("  relay2 off- Desactivar relé 2");
            Serial.println("  test      - Test de todos los relés");
            Serial.println("  help      - Mostrar esta ayuda");
        }
        else if (command.length() > 0) {
            Serial.println("❓ Comando no reconocido. Escribe 'help' para ver comandos disponibles.");
        }
    }
}

// ===========================================
// SETUP PRINCIPAL
// ===========================================

void setup() {
    Serial.begin(115200);
    delay(2000);

    printSystemInfo();

    setupPins();
    setupWifi();

    if (wifiConnected) {
        setupSinricPro();
        setupMqtt();

        webServer.on("/status", []() {
            DynamicJsonDocument doc(1024);
            doc["uptime"] = millis();
            doc["free_heap"] = ESP.getFreeHeap();
            doc["wifi_rssi"] = WiFi.RSSI();
            doc["wifi_connected"] = wifiConnected;
            doc["mqtt_connected"] = mqttConnected;

            JsonArray relays = doc.createNestedArray("relays");
            for (int i = 0; i < 3; i++) {
                JsonObject relay = relays.createNestedObject();
                relay["name"] = relayStates[i].name;
                relay["pin"] = relayStates[i].pin;
                relay["state"] = relayStates[i].state;
                relay["active_high"] = relayStates[i].activeHigh;
            }

            String response;
            serializeJson(doc, response);
            webServer.send(200, "application/json", response);
        });

        webServer.begin();
        Serial.printf("🌐 Servidor web iniciado en: http://%s/status\n", WiFi.localIP().toString().c_str());
    }

    Serial.println("\n🚀 Sistema iniciado correctamente!");
    Serial.println("📋 FUNCIONES DISPONIBLES:");
    Serial.println("  🔄 Mantener RESET 8s para borrar WiFi");
    Serial.println("  📱 Control por Sinric Pro activo");
    Serial.println("  🟦 Control por MQTT activo");
    Serial.println("  🌐 Portal web de configuración");
    Serial.println("  📊 Monitoreo del sistema cada 60s");
    Serial.println("  🔗 Reconexión automática de WiFi y MQTT");
    Serial.println("  ⌨️  Comandos serie disponibles (escribe 'help')");
}

// ===========================================
// LOOP PRINCIPAL
// ===========================================

void loop() {
    checkResetButton();
    updateWifiStatusLed();
    monitorSystem();
    handleWiFiReconnection();
    handleMqttConnection();
    handleSerialCommands();

    if (wifiConnected) {
        SinricPro.handle();
        webServer.handleClient();
    }

    bool currentWifiStatus = (WiFi.status() == WL_CONNECTED);
    if (currentWifiStatus != wifiConnected) {
        wifiConnected = currentWifiStatus;

        if (!wifiConnected) {
            Serial.println("❌ WiFi desconectado detectado");
            mqttConnected = false;
            lastWifiReconnectAttempt = 0;
        } else {
            Serial.println("✅ WiFi reconectado detectado");
            lastMqttReconnectAttempt = 0;
        }
    }

    bool currentMqttStatus = mqttClient.connected();
    if (currentMqttStatus != mqttConnected) {
        mqttConnected = currentMqttStatus;
        Serial.printf("%s MQTT %s\n", mqttConnected ? "✅" : "❌", mqttConnected ? "conectado" : "desconectado");
    }

    delay(10);
}