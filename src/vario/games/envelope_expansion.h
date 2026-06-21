#pragma once

#include <Arduino.h>

struct EnvelopeExpansionResults {
  bool valid = false;
  double northLat = 0;
  double southLat = 0;
  double eastLon = 0;
  double westLon = 0;
  double referenceLat = 0;
  double referenceLon = 0;
  float maxAlt = 0;
  float minAlt = 0;
  float northSouthSpreadMeters = 0;
  float eastWestSpreadMeters = 0;
  float altitudeSpreadMeters = 0;
};

class EnvelopeExpansion {
 public:
  void beginFlight();
  void update();
  void endFlight();

  bool loadBest();
  bool saveBest();

  void draw();

  const EnvelopeExpansionResults& current() const { return current_; }
  const EnvelopeExpansionResults& best() const { return best_; }
  bool playing() const { return playing_; }

 private:
  void capturePoint(double lat, double lon, float altMeters);
  void recomputeSpreads(EnvelopeExpansionResults& results);
  bool bestNeedsUpdate() const;
  void updateBestResults();
  String formatDistance(float meters) const;
  String formatAltitude(float meters) const;
  const EnvelopeExpansionResults& displayedResults() const;

  EnvelopeExpansionResults current_;
  EnvelopeExpansionResults best_;
  bool playing_ = false;
  bool bestLoaded_ = false;
};

extern EnvelopeExpansion envelopeExpansion;
