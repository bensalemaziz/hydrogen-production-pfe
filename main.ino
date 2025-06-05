#include <WiFi.h>
#include <PubSubClient.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <ESP_Mail_Client.h>
#include <ESP32Time.h>

// === CONFIG WIFI / MQTT ===
const char* ssid = "Flybox_4D2E";
const char* password = "f7xcpxcbg6q9";
const char* mqtt_server = "broker.hivemq.com";

// === BROCHES ===
#define ONE_WIRE_BUS     13
#define PH_PIN           34
#define NIVEAU_PIN       32
#define POMPE_PH_PIN     26
#define POMPE_RESV_PIN   27

// === SMTP / EMAIL ===
#define SMTP_HOST "smtp.gmail.com"
#define SMTP_PORT 587
#define AUTHOR_EMAIL    "benghorbelamine000@gmail.com"
#define AUTHOR_PASSWORD "issi dich efhi kyuo"
#define RECIPIENT_EMAIL "jax1919@outlook.com"

// === OBJETS ===
WiFiClient espClient;
PubSubClient client(espClient);
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);
SMTPSession smtp;

ESP32Time rtc(0);  // fuseau horaire UTC
unsigned long lastMsg = 0;

// === SEUILS ===
const float PH_SEUIL = 7.9;
const float TEMP_ALERTE = 30.0;

void setup_wifi() {
  delay(10);
  Serial.println();
  Serial.print("Connexion à ");
  Serial.println(ssid);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connecté !");
  Serial.print("IP : ");
  Serial.println(WiFi.localIP());

  // Synchronisation du temps NTP (manuelle car ESP32Time ne fait pas de sync auto)
  configTime(0, 0, "pool.ntp.org");
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    rtc.setTimeStruct(timeinfo);
    Serial.println("Heure synchronisée !");
  } else {
    Serial.println("Échec de synchronisation du temps !");
  }
}

void reconnect() {
  while (!client.connected()) {
    Serial.print("Connexion MQTT...");
    String clientId = "ESP32Client-" + String(random(0xffff), HEX);
    if (client.connect(clientId.c_str())) {
      Serial.println("Connecté !");
      client.subscribe("/ThinkIOT/Subscribe");
    } else {
      Serial.print("Échec, rc=");
      Serial.print(client.state());
      Serial.println(" nouvelle tentative dans 5 sec");
      delay(5000);
    }
  }
}

void setup() {
  Serial.begin(115200);
  setup_wifi();
  client.setServer(mqtt_server, 1883);
  sensors.begin();

  pinMode(NIVEAU_PIN, INPUT_PULLUP);
  pinMode(POMPE_PH_PIN, OUTPUT);
  pinMode(POMPE_RESV_PIN, OUTPUT);
  digitalWrite(POMPE_PH_PIN, LOW);
  digitalWrite(POMPE_RESV_PIN, LOW);

  smtp.debug(1);
}
/* Callback function to get the Email sending status */
void smtpCallback(SMTP_Status status);
void sendEmailAlert(float temp, float ph, bool niveauBas) {
  ESP_Mail_Session session;
  session.server.host_name = SMTP_HOST;
  session.server.port = SMTP_PORT;
  session.login.email = AUTHOR_EMAIL;
  session.login.password = AUTHOR_PASSWORD;

  SMTP_Message message;
  message.sender.name = "Système Hydrogène";
  message.sender.email = AUTHOR_EMAIL;
  message.subject = "🚨 Alerte : Température Critique";
  message.addRecipient("Responsable", RECIPIENT_EMAIL);

  struct tm now = rtc.getTimeStruct();
  char dateStr[30];
  strftime(dateStr, sizeof(dateStr), "%Y-%m-%d %H:%M:%S", &now);

  String textMsg = "Alerte système de production d'hydrogène !\n\n";
  textMsg += "Paramètres actuels :\n";
  textMsg += "🌡️ Température : " + String(temp, 2) + " °C\n";
  textMsg += "🧪 pH : " + String(ph, 2) + "\n";
  textMsg += "💧 Niveau d'eau : " + String(niveauBas ? "BAS" : "NORMAL") + "\n\n";
  textMsg += "Date/heure : " + String(dateStr) + "\n";
  textMsg += "Veuillez intervenir immédiatement.";

  message.text.content = textMsg.c_str();
  message.text.charSet = "utf-8";
  message.text.transfer_encoding = Content_Transfer_Encoding::enc_7bit;
  message.priority = esp_mail_smtp_priority::esp_mail_smtp_priority_high;

  if (!smtp.connect(&session)) {
    Serial.println("Erreur connexion SMTP: " + smtp.errorReason());
    return;
  }

  if (!MailClient.sendMail(&smtp, &message)) {
    Serial.println("Erreur envoi email: " + smtp.errorReason());
  }
}

void loop() {
  if (!client.connected()) {
    reconnect();
  }
  client.loop();

  unsigned long now = millis();
  if (now - lastMsg > 2000) {
    lastMsg = now;

    sensors.requestTemperatures();
    float temperature = sensors.getTempCByIndex(0);

    int adc = analogRead(PH_PIN);
    float voltage = adc * (3.3 / 4095.0);
    float pH = 7 + ((2.5 - voltage) / 0.18);

    int niveau = digitalRead(NIVEAU_PIN);
    bool niveauBas = (niveau == HIGH);

    digitalWrite(POMPE_PH_PIN, (pH < PH_SEUIL || niveauBas) ? HIGH : LOW);
    digitalWrite(POMPE_RESV_PIN, niveauBas ? HIGH : LOW);

    char tempStr[8], phStr[8], nivStr[8];
    dtostrf(temperature, 4, 2, tempStr);
    dtostrf(pH, 4, 2, phStr);
    snprintf(nivStr, sizeof(nivStr), "%s", niveauBas ? "LOW" : "HIGH");

    client.publish("/ThinkIOT/temp", tempStr);
    client.publish("/ThinkIOT/ph", phStr);
    client.publish("/ThinkIOT/niveau", nivStr);
    client.publish("/ThinkIOT/pompe_ph", (pH < PH_SEUIL) ? "ON" : "OFF");
    client.publish("/ThinkIOT/pompe_reservoir", niveauBas ? "ON" : "OFF");

    if (temperature > TEMP_ALERTE) {
      sendEmailAlert(temperature, pH, niveauBas);
    }

    Serial.println("=========================================");
    Serial.printf("🌡️  Température : %.2f °C\n", temperature);
    Serial.printf("🧪 pH : %.2f\n", pH);
    Serial.printf("💧 Niveau : %s\n", nivStr);
    Serial.printf("🚰 Pompe pH : %s\n", (pH < PH_SEUIL) ? "ON" : "OFF");
    Serial.printf("🚿 Pompe rempl. : %s\n", niveauBas ? "ON" : "OFF");
    Serial.println("=========================================\n");
  }
}
