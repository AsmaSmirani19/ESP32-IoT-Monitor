#include <OneWire.h>
#include <DallasTemperature.h>
#include <WiFi.h>
#include <WebServer.h>

// ================= CONFIG =================
#define ONE_WIRE_BUS 14
#define MAX_SENSORS  3
#define MAX_HISTORY  100
#define RELAY_PIN    27

const char* ssid     = "";
const char* password = "";

OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);
WebServer server(80);

// ================= STRUCTS =================
typedef struct {
  int intervalle_sec;
  int nbSamples;
} ConfigData;

typedef struct {
  int sensorId;
  float temperature;
} SensorData;

typedef struct {
  int count;
  SensorData data[MAX_SENSORS];
} ResultPacket;

// ================= QUEUES =================
QueueHandle_t configQueue;
QueueHandle_t resultQueue;
SemaphoreHandle_t dataMutex;

// ================= HISTORY =================
ResultPacket history[MAX_HISTORY];
int historyIndex = 0;
int historyCount = 0;
bool newDataAvailable = false;

// ================= HTML =================
const char* HTML_FORM = R"rawliteral(
<!DOCTYPE html>
<html lang="fr">
<head>
<meta charset="UTF-8">
<title>ESP32 Config</title>
</head>
<body>

<h2>Configuration</h2>

<form action="/start" method="POST">
Intervalle (sec):
<input name="intervalle" type="number" value="5">
<br><br>

Nb Samples:
<input name="nbSamples" type="number" value="10">
<br><br>

<button type="submit">Start</button>
</form>

</body>
</html>
)rawliteral";

const char* HTML_RESULTS = R"rawliteral(
<!DOCTYPE html>
<html lang="fr">
<head>
<meta charset="UTF-8">
<title>Résultats</title>
</head>
<body>

<h2>Données temps réel</h2>

<table border="1">
<thead>
<tr>
  <th>Mesure</th>
  <th>Capteur 0</th>
  <th>Capteur 1</th>
  <th>Capteur 2</th>
</tr>
</thead>

<tbody id="data-body"></tbody>
</table>

<br><br>

<button onclick="fetch('/relay/on')">
RELAY ON
</button>

<button onclick="fetch('/relay/off')">
RELAY OFF
</button>

<script>

let pollingInterval = null;
let mesureIndex = 1;

function fetchData() {

  fetch('/data')
    .then(r => r.json())
    .then(d => {

      if (!d.available) return;

      let row = '<tr>';
      row += `<td>${mesureIndex}</td>`;

      let temps = ["—", "—", "—"];

      d.sensors.forEach(s => {

        if (s.id < 3) {

          let ok = s.temp > -50 && s.temp < 125;

          temps[s.id] = ok
            ? s.temp.toFixed(2) + " °C"
            : "—";
        }
      });

      for (let i = 0; i < 3; i++) {
        row += `<td>${temps[i]}</td>`;
      }

      row += '</tr>';

      document.getElementById('data-body').innerHTML += row;

      mesureIndex++;
    });
}

if (pollingInterval)
  clearInterval(pollingInterval);

pollingInterval = setInterval(fetchData, 2000);

fetchData();

</script>

</body>
</html>
)rawliteral";

// ================= API =================
void handleData() {

  String json;

  if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {

    if (!newDataAvailable || historyCount == 0) {

      xSemaphoreGive(dataMutex);

      server.send(
        200,
        "application/json",
        "{\"available\":false}"
      );

      return;
    }

    int last =
      (historyIndex - 1 + MAX_HISTORY) % MAX_HISTORY;

    ResultPacket &p = history[last];

    json = "{\"available\":true,\"sensors\":[";

    for (int i = 0; i < p.count; i++) {

      if (i)
        json += ",";

      json +=
        "{\"id\":" +
        String(p.data[i].sensorId) +
        ",\"temp\":" +
        String(p.data[i].temperature, 2) +
        "}";
    }

    json += "]}";

    newDataAvailable = false;

    xSemaphoreGive(dataMutex);

    server.send(200, "application/json", json);

  } else {

    server.send(
      500,
      "application/json",
      "{\"error\":\"mutex\"}"
    );
  }
}

// ================= START =================
void handleStart() {

  if (configQueue == NULL) {

    server.send(
      500,
      "text/plain",
      "Queue non initialisée"
    );

    return;
  }

  ConfigData cfg;

  int intervalle =
    server.hasArg("intervalle")
    ? server.arg("intervalle").toInt()
    : 5;

  int samples =
    server.hasArg("nbSamples")
    ? server.arg("nbSamples").toInt()
    : 1;

  cfg.intervalle_sec = max(1, intervalle);
  cfg.nbSamples = max(1, samples);

  xQueueOverwrite(configQueue, &cfg);

  server.sendHeader("Location", "/results");
  server.send(302, "text/plain", "");
}

// ================= WIFI TASK =================
void taskServer(void *p) {

  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {

    Serial.print(".");

    vTaskDelay(500 / portTICK_PERIOD_MS);
  }

  Serial.println("\nWiFi connecté !");
  Serial.println(WiFi.localIP());

  // ROUTES
  server.on("/", []() {
    server.send(200, "text/html", HTML_FORM);
  });

  server.on("/start", HTTP_POST, handleStart);

  server.on("/results", []() {
    server.send(200, "text/html", HTML_RESULTS);
  });

  server.on("/data", handleData);

  // RELAIS ON
  server.on("/relay/on", []() {

    digitalWrite(RELAY_PIN, HIGH);

    server.send(
      200,
      "text/plain",
      "Relay ON"
    );
  });

  // RELAIS OFF
  server.on("/relay/off", []() {

    digitalWrite(RELAY_PIN, LOW);

    server.send(
      200,
      "text/plain",
      "Relay OFF"
    );
  });

  server.begin();

  ResultPacket packet;

  while (true) {

    server.handleClient();

    if (xQueueReceive(resultQueue, &packet, 0) == pdTRUE) {

      if (xSemaphoreTake(dataMutex, portMAX_DELAY) == pdTRUE) {

        history[historyIndex] = packet;

        historyIndex =
          (historyIndex + 1) % MAX_HISTORY;

        if (historyCount < MAX_HISTORY)
          historyCount++;

        newDataAvailable = true;

        xSemaphoreGive(dataMutex);
      }
    }

    vTaskDelay(5 / portTICK_PERIOD_MS);
  }
}

// ================= SENSOR TASK =================
void taskSensors(void *p) {

  ConfigData cfg;
  ResultPacket packet;

  while (true) {

    xQueueReceive(configQueue, &cfg, portMAX_DELAY);

    for (int s = 0; s < cfg.nbSamples; s++) {

      packet.count = 0;

      sensors.requestTemperatures();

      for (int i = 0; i < MAX_SENSORS; i++) {

        DeviceAddress addr;

        if (sensors.getAddress(addr, i)) {

          float temp = sensors.getTempC(addr);

          if (temp != DEVICE_DISCONNECTED_C) {

            packet.data[packet.count].sensorId = i;

            packet.data[packet.count].temperature = temp;

            packet.count++;
          }
        }
      }

      xQueueOverwrite(resultQueue, &packet);

      vTaskDelay(
        cfg.intervalle_sec * 1000 / portTICK_PERIOD_MS
      );
    }
  }
}

// ================= SETUP =================
void setup() {

  Serial.begin(115200);

  sensors.begin();

  // RELAIS
  pinMode(RELAY_PIN, OUTPUT);

  digitalWrite(RELAY_PIN, LOW);

  // QUEUES
  configQueue =
    xQueueCreate(1, sizeof(ConfigData));

  resultQueue =
    xQueueCreate(1, sizeof(ResultPacket));

  dataMutex =
    xSemaphoreCreateMutex();

  // TASKS
  xTaskCreate(
    taskServer,
    "wifi",
    8192,
    NULL,
    1,
    NULL
  );

  xTaskCreate(
    taskSensors,
    "sensors",
    4096,
    NULL,
    1,
    NULL
  );
}

// ================= LOOP =================
void loop() {

}