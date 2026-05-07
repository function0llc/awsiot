Use this path:

1. **Create AWS IoT resources (cloud side)**
2. **Flash firmware with certs + MQTT code (device side)**
3. **Connect over TLS to AWS IoT Core endpoint**
4. **Publish/subscribe test topics**

---

## 1) AWS IoT Core setup

In AWS Console:

### A. Create a Thing
- Go to **AWS IoT Core → Manage → All devices → Things**
- Create a thing (e.g. `esp32c3-01`)

### B. Create certificate + keys
- During thing creation, generate:
  - Device certificate (`certificate.pem.crt`)
  - Private key (`private.pem.key`)
- Download them securely.

### C. Attach policy
Create an IoT policy like:

```json
{
  "Version": "2012-10-17",
  "Statement": [
    {
      "Effect": "Allow",
      "Action": ["iot:Connect"],
      "Resource": ["arn:aws:iot:REGION:ACCOUNT_ID:client/esp32c3-01"]
    },
    {
      "Effect": "Allow",
      "Action": ["iot:Publish","iot:Subscribe","iot:Receive"],
      "Resource": [
        "arn:aws:iot:REGION:ACCOUNT_ID:topic/esp32c3/test",
        "arn:aws:iot:REGION:ACCOUNT_ID:topicfilter/esp32c3/test"
      ]
    }
  ]
}
```

Attach this policy to the certificate, and attach certificate to the Thing.

### D. Get your AWS IoT endpoint
- **AWS IoT Core → Settings**
- Copy endpoint like:
  `xxxxxxxxxxxxx-ats.iot.us-west-2.amazonaws.com`

### E. Root CA
Use Amazon Root CA 1:
- `AmazonRootCA1.pem`

---

## 2) ESP32-C3 firmware approach

Two common options:

- **Arduino framework** (fastest to get working)
- **ESP-IDF + AWS IoT Embedded C SDK** (more production-grade control)

For first success, Arduino is quickest.

---

## 3) Arduino example (MQTT over TLS 8883)

Libraries:
- `WiFi.h`
- `WiFiClientSecure.h`
- `PubSubClient.h`

You embed:
- Root CA (`AmazonRootCA1.pem`)
- Device cert
- Device private key

```cpp
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>

const char* ssid = "YOUR_WIFI";
const char* password = "YOUR_WIFI_PASS";

const char* awsEndpoint = "xxxxxxxxxxxxx-ats.iot.us-west-2.amazonaws.com";
const int awsPort = 8883;
const char* clientId = "esp32c3-01";
const char* pubTopic = "esp32c3/test";
const char* subTopic = "esp32c3/test";

static const char* rootCA = R"EOF(
-----BEGIN CERTIFICATE-----
...AmazonRootCA1...
-----END CERTIFICATE-----
)EOF";

static const char* deviceCert = R"KEY(
-----BEGIN CERTIFICATE-----
...device cert...
-----END CERTIFICATE-----
)KEY";

static const char* privateKey = R"KEY(
-----BEGIN RSA PRIVATE KEY-----
...private key...
-----END RSA PRIVATE KEY-----
)KEY";

WiFiClientSecure net;
PubSubClient mqtt(net);

void messageHandler(char* topic, byte* payload, unsigned int len) {
  Serial.print("Message on ");
  Serial.print(topic);
  Serial.print(": ");
  for (unsigned int i=0; i<len; i++) Serial.print((char)payload[i]);
  Serial.println();
}

void connectWiFi() {
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected");
}

void connectAWS() {
  net.setCACert(rootCA);
  net.setCertificate(deviceCert);
  net.setPrivateKey(privateKey);

  mqtt.setServer(awsEndpoint, awsPort);
  mqtt.setCallback(messageHandler);

  while (!mqtt.connected()) {
    Serial.print("Connecting MQTT...");
    if (mqtt.connect(clientId)) {
      Serial.println("connected");
      mqtt.subscribe(subTopic);
    } else {
      Serial.print("failed, rc=");
      Serial.println(mqtt.state());
      delay(2000);
    }
  }
}

void setup() {
  Serial.begin(115200);
  connectWiFi();

  // Optional but recommended: set time via NTP before TLS
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  delay(2000);

  connectAWS();
}

void loop() {
  if (!mqtt.connected()) connectAWS();
  mqtt.loop();

  static unsigned long last = 0;
  if (millis() - last > 5000) {
    last = millis();
    mqtt.publish(pubTopic, "{\"msg\":\"hello from esp32-c3\"}");
  }
}
```

---

## 4) Test in AWS IoT MQTT test client

- Open **AWS IoT Core → MQTT test client**
- Subscribe to: `esp32c3/test`
- You should see periodic messages from the board.

---

## 5) Common failure points

- **Wrong endpoint** (must be your exact `-ats.iot.<region>.amazonaws.com`)
- **Policy mismatch** (`clientId` in policy must match connect client id if restricted)
- **Bad cert/key pairing**
- **System time not set** (TLS validation fails if RTC time is wrong)
- **Using wrong root CA**
- **Topic ARN mismatch** in policy

---

## 6) Production recommendations

- Use **AWS IoT Fleet Provisioning** instead of hardcoding certs at scale
- Store keys in secure flash / secure element when possible
- Implement reconnect + exponential backoff
- Use **Device Shadow** topics for state sync
- Restrict policy to least privilege

---

If you want, the next step is to move this into a clean **ESP-IDF project structure** with separate files for Wi-Fi, TLS credentials, MQTT, and shadow updates.