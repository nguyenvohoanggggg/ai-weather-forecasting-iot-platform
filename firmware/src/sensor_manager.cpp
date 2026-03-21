#include "sensor_manager.h"

#include <DHT.h>
#include <Adafruit_BMP280.h>
#include <Wire.h>

#include "config.h"

namespace
{
  // Common rain sensor modules drive the digital pin low when water is detected.
  constexpr int RAIN_DETECTED_STATE = LOW;
  constexpr uint8_t RAIN_SAMPLE_COUNT = 5;
  constexpr uint8_t RAIN_SAMPLE_THRESHOLD = 3;

  DHT g_dht(config::DHT_PIN, config::DHT_TYPE);
  Adafruit_BMP280 g_bmp;
} // namespace

bool SensorManager::begin()
{
  g_dht.begin();
  bmpReady_ = g_bmp.begin(0x76);
  if (!bmpReady_)
  {
    bmpReady_ = g_bmp.begin(0x77);
  }

  // GPIO34 is input-only on ESP32 and has no internal pull-up/down support.
  // Use an external resistor on the rain sensor signal line.
  pinMode(config::RAIN_SENSOR_PIN, INPUT);

  if (!bmpReady_)
  {
    Serial.println("Warning: BMP280 not detected, pressure will be reported as 0.0 hPa");
  }

  return true;
}

bool SensorManager::pressureAvailable() const
{
  return bmpReady_;
}

void SensorManager::setTimestampProvider(TimestampProvider provider)
{
  timestampProvider_ = provider;
}

bool SensorManager::sample(SensorReading &outReading)
{
  const float humidity = g_dht.readHumidity();
  const float temperature = g_dht.readTemperature();

  if (isnan(humidity) || isnan(temperature))
  {
    return false;
  }

  if (humidity < 0.0f || humidity > 100.0f)
  {
    return false;
  }

  float pressureHpa = 0.0f;

  if (bmpReady_)
  {
    const float pressurePa = g_bmp.readPressure();
    if (pressurePa > 0.0f)
    {
      pressureHpa = pressurePa / 100.0f;
    }
  }

  uint8_t rainHits = 0;
  for (uint8_t i = 0; i < RAIN_SAMPLE_COUNT; ++i)
  {
    if (digitalRead(config::RAIN_SENSOR_PIN) == RAIN_DETECTED_STATE)
    {
      rainHits++;
    }
  }
  const bool isRaining = rainHits >= RAIN_SAMPLE_THRESHOLD;

  outReading.timestamp = timestampProvider_ != nullptr ? timestampProvider_() : 0ULL;
  outReading.temperatureC = temperature;
  outReading.humidityPct = humidity;
  outReading.pressureHpa = pressureHpa;
  outReading.isRaining = isRaining;

  return true;
}
