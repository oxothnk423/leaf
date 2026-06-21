#include "games/envelope_expansion.h"

#include <ArduinoJson.h>
#include <SD_MMC.h>
#include <TinyGPSPlus.h>
#include <U8g2lib.h>

#include "hardware/configuration.h"
#include "instruments/gps.h"
#include "logging/log.h"
#include "storage/sd_card.h"
#include "ui/display/display_fields.h"
#include "ui/display/fonts.h"
#include "ui/settings/settings.h"

#ifndef WO256X128
extern U8G2_ST75256_JLX19296_F_4W_HW_SPI u8g2;
#else
extern U8G2_ST75256_WO256X128_F_4W_HW_SPI u8g2;
#endif

namespace {
constexpr const char* kResultsFile = "/games/envelope_expansion.json";

float altitudeToUserUnits(float meters) {
  return settings.units_alt ? meters * 3.28084f : meters;
}
}  // namespace

EnvelopeExpansion envelopeExpansion;

void EnvelopeExpansion::beginFlight() {
  current_ = EnvelopeExpansionResults();
  playing_ = true;
  loadBest();
}

void EnvelopeExpansion::update() {
  if (!playing_ || !gps.location.isValid() || !gps.altitude.isValid()) return;

  capturePoint(gps.location.lat(), gps.location.lng(), gps.altitude.meters());
}

void EnvelopeExpansion::endFlight() {
  if (!playing_) return;

  playing_ = false;
  if (bestNeedsUpdate()) {
    updateBestResults();
    saveBest();
  }
}

bool EnvelopeExpansion::loadBest() {
  if (bestLoaded_) return best_.valid;

  if (!sdcard.isMounted()) return false;
  bestLoaded_ = true;
  if (!SD_MMC.exists(kResultsFile)) return false;

  File file = SD_MMC.open(kResultsFile, "r");
  if (!file) return false;

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, file);
  file.close();
  if (error) return false;

  EnvelopeExpansionResults loaded;
  loaded.valid = doc["valid"] | false;
  loaded.northLat = doc["northLat"] | 0.0;
  loaded.southLat = doc["southLat"] | 0.0;
  loaded.eastLon = doc["eastLon"] | 0.0;
  loaded.westLon = doc["westLon"] | 0.0;
  loaded.referenceLat = doc["referenceLat"] | 0.0;
  loaded.referenceLon = doc["referenceLon"] | 0.0;
  loaded.maxAlt = doc["maxAlt"] | 0.0f;
  loaded.minAlt = doc["minAlt"] | 0.0f;
  loaded.northSouthSpreadMeters = doc["northSouthSpreadMeters"] | 0.0f;
  loaded.eastWestSpreadMeters = doc["eastWestSpreadMeters"] | 0.0f;
  loaded.altitudeSpreadMeters = doc["altitudeSpreadMeters"] | 0.0f;

  if (!loaded.valid) return false;

  best_ = loaded;
  return true;
}

bool EnvelopeExpansion::saveBest() {
  if (!best_.valid || !sdcard.isMounted()) return false;

  SD_MMC.mkdir("/games");

  File file = SD_MMC.open(kResultsFile, "w", true);
  if (!file) return false;

  JsonDocument doc;
  doc["valid"] = best_.valid;
  doc["northLat"] = best_.northLat;
  doc["southLat"] = best_.southLat;
  doc["eastLon"] = best_.eastLon;
  doc["westLon"] = best_.westLon;
  doc["referenceLat"] = best_.referenceLat;
  doc["referenceLon"] = best_.referenceLon;
  doc["maxAlt"] = best_.maxAlt;
  doc["minAlt"] = best_.minAlt;
  doc["northSouthSpreadMeters"] = best_.northSouthSpreadMeters;
  doc["eastWestSpreadMeters"] = best_.eastWestSpreadMeters;
  doc["altitudeSpreadMeters"] = best_.altitudeSpreadMeters;

  bool success = serializeJson(doc, file) > 0;
  file.close();
  return success;
}

void EnvelopeExpansion::draw() {
  const EnvelopeExpansionResults& results = displayedResults();

  u8g2.setFont(leaf_6x12);
  u8g2.setCursor(3, 34);
  u8g2.print("ENVELOPE");
  u8g2.setCursor(3, 47);
  u8g2.print("EXPANSION");

  u8g2.setFont(leaf_5h);
  u8g2.setCursor(3, 65);
  u8g2.print(playing_ ? "CURRENT FLIGHT" : "BEST SAVED");

  if (!results.valid) {
    u8g2.setFont(leaf_6x12);
    u8g2.setCursor(3, 97);
    u8g2.print("No score yet");
    u8g2.setCursor(3, 112);
    u8g2.print("Fly a log!");
    return;
  }

  u8g2.setFont(leaf_6x12);
  u8g2.setCursor(3, 91);
  u8g2.print("N/S ");
  u8g2.print(formatDistance(results.northSouthSpreadMeters));

  u8g2.setCursor(3, 113);
  u8g2.print("E/W ");
  u8g2.print(formatDistance(results.eastWestSpreadMeters));

  u8g2.setCursor(3, 135);
  u8g2.print("ALT ");
  u8g2.print(formatAltitude(results.altitudeSpreadMeters));
}

void EnvelopeExpansion::capturePoint(double lat, double lon, float altMeters) {
  if (!current_.valid) {
    current_.valid = true;
    current_.northLat = lat;
    current_.southLat = lat;
    current_.eastLon = lon;
    current_.westLon = lon;
    current_.referenceLat = lat;
    current_.referenceLon = lon;
    current_.maxAlt = altMeters;
    current_.minAlt = altMeters;
  } else {
    if (lat > current_.northLat) current_.northLat = lat;
    if (lat < current_.southLat) current_.southLat = lat;
    if (lon > current_.eastLon) current_.eastLon = lon;
    if (lon < current_.westLon) current_.westLon = lon;
    if (altMeters > current_.maxAlt) current_.maxAlt = altMeters;
    if (altMeters < current_.minAlt) current_.minAlt = altMeters;

    current_.referenceLat = (current_.northLat + current_.southLat) / 2.0;
    current_.referenceLon = (current_.eastLon + current_.westLon) / 2.0;
  }

  recomputeSpreads(current_);
}

void EnvelopeExpansion::recomputeSpreads(EnvelopeExpansionResults& results) {
  if (!results.valid) return;

  results.northSouthSpreadMeters =
      TinyGPSPlus::distanceBetween(results.southLat, results.referenceLon, results.northLat,
                                   results.referenceLon);
  results.eastWestSpreadMeters =
      TinyGPSPlus::distanceBetween(results.referenceLat, results.westLon, results.referenceLat,
                                   results.eastLon);
  results.altitudeSpreadMeters = results.maxAlt - results.minAlt;
}

bool EnvelopeExpansion::bestNeedsUpdate() const {
  if (!current_.valid) return false;
  if (!best_.valid) return true;

  return current_.northSouthSpreadMeters > best_.northSouthSpreadMeters ||
         current_.eastWestSpreadMeters > best_.eastWestSpreadMeters ||
         current_.altitudeSpreadMeters > best_.altitudeSpreadMeters;
}

void EnvelopeExpansion::updateBestResults() {
  if (!current_.valid) return;

  if (!best_.valid) {
    best_ = current_;
    return;
  }

  best_.valid = true;
  if (current_.northSouthSpreadMeters > best_.northSouthSpreadMeters) {
    best_.northLat = current_.northLat;
    best_.southLat = current_.southLat;
    best_.referenceLon = current_.referenceLon;
    best_.northSouthSpreadMeters = current_.northSouthSpreadMeters;
  }

  if (current_.eastWestSpreadMeters > best_.eastWestSpreadMeters) {
    best_.eastLon = current_.eastLon;
    best_.westLon = current_.westLon;
    best_.referenceLat = current_.referenceLat;
    best_.eastWestSpreadMeters = current_.eastWestSpreadMeters;
  }

  if (current_.altitudeSpreadMeters > best_.altitudeSpreadMeters) {
    best_.maxAlt = current_.maxAlt;
    best_.minAlt = current_.minAlt;
    best_.altitudeSpreadMeters = current_.altitudeSpreadMeters;
  }
}

String EnvelopeExpansion::formatDistance(float meters) const {
  if (settings.units_distance) {
    float feet = meters * 3.28084f;
    if (feet >= 1000.0f) return String(feet / 5280.0f, 2) + "mi";
    return String(feet, 0) + "ft";
  }

  if (meters >= 1000.0f) return String(meters / 1000.0f, 2) + "km";
  return String(meters, 0) + "m";
}

String EnvelopeExpansion::formatAltitude(float meters) const {
  return String(altitudeToUserUnits(meters), 0) + (settings.units_alt ? "ft" : "m");
}

const EnvelopeExpansionResults& EnvelopeExpansion::displayedResults() const {
  if (playing_) return current_;
  return best_;
}
