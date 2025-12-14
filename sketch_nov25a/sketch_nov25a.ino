#include <SoftwareSerial.h>

// SDS011 TX -> D5 (GPIO14)
// SDS011 RX -> D6 (GPIO12)

SoftwareSerial sds(14, 12); // RX = 14 (D5), TX = 12 (D6)

void setup() {
  Serial.begin(115200);
  sds.begin(9600);
  Serial.println("Reading SDS011...");
}

void loop() {
  if (sds.available() >= 10) {
    uint8_t buf[10];
    sds.readBytes(buf, 10);

    if (buf[0] == 0xAA && buf[1] == 0xC0 && buf[9] == 0xAB) {
      float pm25 = (buf[2] + buf[3] * 256) / 10.0;
      float pm10 = (buf[4] + buf[5] * 256) / 10.0;

      Serial.print("PM2.5: ");
      Serial.print(pm25);
      Serial.print(" ug/m3, PM10: ");
      Serial.println(pm10);
    }
  }
}
