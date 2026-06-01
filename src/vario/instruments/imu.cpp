#include "instruments/imu.h"

#include "dispatch/message_types.h"
#include "hardware/Leaf_I2C.h"
#include "logging/log.h"
#include "logging/telemetry.h"
#include "math/kalman.h"
#include "storage/sd_card.h"
#include "ui/display/pages/dialogs/page_imu_diagnostic_alert.h"
#include "utils/const_math.h"

// Singleton IMU instance for device
IMU imu;

namespace {
  // === What to display on the serial port
  // #define ALIGN_TEXT 3  // When set, puts values in equal-width columns with this many digits
  // #define SHOW_QUATERNION  // When set, print the components of the orientation quaternion
  // #define SHOW_DEVICE_ACCEL    // When set, print the components of the device-frame acceleration
  // #define SHOW_WORLD_ACCEL     // When set, print the components of the world-frame acceleration
  // #define SHOW_VERTICAL_ACCEL  // When set, print the vertical acceleration (with gravity
  // removed) #define SHOW_FIXED_BOUNDS 0.2  // When set, print fixed values to keep the Arduino
  // serial plot in a consistent range

  // == Estimation of constant gravity acceleration ==
  constexpr double NEW_MEASUREMENT_WEIGHT = 0.9;  // Weight given to new measurements...
  constexpr double AFTER_SECONDS = 5.0;           // ...after this number of seconds
  constexpr double K_UPDATE = constexpr_log(1 - NEW_MEASUREMENT_WEIGHT) / AFTER_SECONDS;

  constexpr uint8_t IMU_SAMPLE_RATE = 40;  // Hz
  constexpr double GRAVITY_INIT_S = 3.0;   // Time to take to estimate the initial gravity magnitude
  // samples to bypass at startup while gravity is estimated
  constexpr uint32_t GRAVITY_INIT_SAMPLES = IMU_SAMPLE_RATE * GRAVITY_INIT_S;

  constexpr double GRAVITY_UPDATE_ACCEL_TOLERANCE_G = 0.12;
  constexpr double GRAVITY_UPDATE_VERTICAL_TOLERANCE_G = 0.25;
  constexpr double GRAVITY_UNSTABLE_DRIFT_ALERT_G = 0.5;
  constexpr double QUATERNION_ROTATION_DELTA_ALERT_G = 0.05;

  double unstableGravityDriftG = 0;

  bool normalizedQuaternion(double* qw, double* qx, double* qy, double* qz) {
    double qVecMagnitude2 = (*qx * *qx) + (*qy * *qy) + (*qz * *qz);
    if (isnan(qVecMagnitude2) || isinf(qVecMagnitude2) || qVecMagnitude2 > 1.05) {
      return false;
    }

    if (qVecMagnitude2 > 1.0) {
      qVecMagnitude2 = 1.0;
    }

    *qw = sqrt(1.0 - qVecMagnitude2);

    double qMagnitude = sqrt((*qw * *qw) + (*qx * *qx) + (*qy * *qy) + (*qz * *qz));
    if (isnan(qMagnitude) || isinf(qMagnitude) || qMagnitude < 0.5) {
      return false;
    }

    *qw /= qMagnitude;
    *qx /= qMagnitude;
    *qy /= qMagnitude;
    *qz /= qMagnitude;
    return true;
  }

  void reportIMUDiagnostic(etl::imessage_bus* bus, const char* trigger, const String& detail) {
    String msg = String("IMU diagnostic: ") + trigger + "\n" + detail;
    Serial.println(msg);
    if (bus) {
      bus->receive(CommentMessage(msg));
    }
    PageIMUDiagnosticAlert::show(trigger, detail);
  }
}  // namespace

void quaternionMult(double qw, double qx, double qy, double qz, double rw, double rx, double ry,
                    double rz, double* pw, double* px, double* py, double* pz) {
  *pw = rw * qw - rx * qx - ry * qy - rz * qz;
  *px = rw * qx + rx * qw - ry * qz + rz * qy;
  *py = rw * qy + rx * qz + ry * qw - rz * qx;
  *pz = rw * qz - rx * qy + ry * qx + rz * qw;
}

void rotateByQuaternion(double px, double py, double pz, double qw, double qx, double qy, double qz,
                        double* p1x, double* p1y, double* p1z) {
  double qrw, qrx, qry, qrz, qcw;
  quaternionMult(qw, qx, qy, qz, 0, px, py, pz, &qrw, &qrx, &qry, &qrz);
  quaternionMult(qrw, qrx, qry, qrz, qw, -qx, -qy, -qz, &qcw, p1x, p1y, p1z);
}

inline void printFloat(double v) {
#ifdef ALIGN_TEXT
  if (v >= 0) {
    Serial.print(' ');
  }
  const int digits = ALIGN_TEXT;
#else
  const int digits = 4;
#endif
  Serial.print(v, digits);
}

IMU::IMU()
    : kalmanvert_(pow(POSITION_MEASURE_STANDARD_DEVIATION, 2),
                  pow(ACCELERATION_MEASURE_STANDARD_DEVIATION, 2)),
      gravityInitCount_(GRAVITY_INIT_SAMPLES) {}

void IMU::processMotion(const MotionUpdate& m) {
  // Scale to +/- 1
  double magnitude = ((m.qx * m.qx) + (m.qy * m.qy) + (m.qz * m.qz));
  if (magnitude >= 1.0) magnitude = 1.0;
  double qw = sqrt(1.0 - magnitude);

  bool needComma = false;
  bool needNewline = false;

#ifdef SHOW_QUATERNION
  Serial.print(F("Qw:"));
  printFloat(qw);
  Serial.print(F(",Qx:"));
  printFloat(m.qx);
  Serial.print(F(",Qy:"));
  printFloat(m.qy);
  Serial.print(F(",Qz:"));
  printFloat(m.qz);
  needComma = true;
  needNewline = true;
#endif

  accelTot_ = sqrt(m.ax * m.ax + m.ay * m.ay + m.az * m.az);
  validAccelTot_ = true;

#ifdef SHOW_DEVICE_ACCEL
  if (needComma) {
    Serial.print(',');
  }
  Serial.print(F("Ax:"));
  printFloat(m.ax);
  Serial.print(F(",Ay:"));
  printFloat(m.ay);
  Serial.print(F(",Az:"));
  printFloat(m.az);
  needComma = true;
  needNewline = true;
#endif

  double awx, awy, awz;
  rotateByQuaternion(m.ax, m.ay, m.az, qw, m.qx, m.qy, m.qz, &awx, &awy, &awz);
  double qwn = 0;
  double qxn = m.qx;
  double qyn = m.qy;
  double qzn = m.qz;
  if (!normalizedQuaternion(&qwn, &qxn, &qyn, &qzn)) {
    reportIMUDiagnostic(bus_, "Quaternion invalid",
                        String("q=(") + String(m.qx, 5) + "," + String(m.qy, 5) + "," +
                            String(m.qz, 5) + ")\nqvec2=" +
                            String((m.qx * m.qx) + (m.qy * m.qy) + (m.qz * m.qz), 5));
  } else {
    double awxn, awyn, awzn;
    rotateByQuaternion(m.ax, m.ay, m.az, qwn, qxn, qyn, qzn, &awxn, &awyn, &awzn);
    if (!isnan(awzn) && !isinf(awzn) && fabs(awzn - awz) > QUATERNION_ROTATION_DELTA_ALERT_G) {
      reportIMUDiagnostic(bus_, "Quaternion normalization",
                          String("rawWz=") + String(awz, 5) + "\nnormWz=" +
                              String(awzn, 5) + "\ndelta=" + String(awzn - awz, 5));
    }
  }
  if (isinf(awz) || isnan(awz)) {
    fatalErrorInfo(
        "m.ax=%g, m.ay=%g, m.az=%g, qw=%g, m.qx=%g, m.qy=%g, m.qz=%g, awx=%g, awy=%g, awz=%g", m.ax,
        m.ay, m.az, qw, m.qx, m.qy, m.qz, awx, awy, awz);
    fatalError("IMU awz was invalid");
  }

#ifdef SHOW_WORLD_ACCEL
  if (needComma) {
    Serial.print(',');
  }
  Serial.print("Wx:");
  printFloat(awx);
  Serial.print(",Wy:");
  printFloat(awy);
  Serial.print(",Wz:");
  printFloat(awz);
  needComma = true;
  needNewline = true;
#endif

#ifdef SHOW_FIXED_BOUNDS
  if (needComma) {
    Serial.print(',');
  }
  Serial.print("min:");
  printFloat(SHOW_FIXED_BOUNDS);
  Serial.print(",max:");
  printFloat(-SHOW_FIXED_BOUNDS);
  needNewline = true;
#endif

  if (gravityInitCount_ > 0) {
    validAccelVert_ = false;
    gravity_ += awz;
    if (--gravityInitCount_ == 0) {
      gravity_ /= GRAVITY_INIT_SAMPLES;
    }
  }
  if (gravityInitCount_ == 0) {
    // In steady-state (normally), actual vertical acceleration is the difference between measured
    // vertical acceleration and gravity
    accelVert_ = awz - gravity_;
    validAccelVert_ = true;

    // Slowly update estimate of gravity
    double dt = ((double)m.t - tLastGravityUpdate_) * 0.001;
    double f = exp(K_UPDATE * dt);
    double nextGravity = gravity_ * f + awz * (1 - f);

    double accelMagnitudeDelta = fabs(accelTot_ - fabs(gravity_));
    if (accelMagnitudeDelta > GRAVITY_UPDATE_ACCEL_TOLERANCE_G ||
        fabs(accelVert_) > GRAVITY_UPDATE_VERTICAL_TOLERANCE_G) {
      unstableGravityDriftG += fabs(nextGravity - gravity_);
      if (unstableGravityDriftG > GRAVITY_UNSTABLE_DRIFT_ALERT_G) {
        reportIMUDiagnostic(bus_, "Gravity updated while moving",
                            String("unstable drift=") + String(unstableGravityDriftG, 5) +
                                "g\n|accel|-|g|=" + String(accelMagnitudeDelta, 5) +
                                "\nvertAccelG=" + String(accelVert_, 5) + "\ng=" +
                                String(gravity_, 5) + " awz=" + String(awz, 5));
      }
    } else {
      unstableGravityDriftG *= 0.95;
    }

    gravity_ = nextGravity;
  }

#ifdef SHOW_VERTICAL_ACCEL
  if (needComma) {
    Serial.print(',');
  }
  Serial.print("dAz:");
  printFloat(accelVert_);
  needComma = true;
  needNewline = true;
#endif

  tLastGravityUpdate_ = m.t;

  if (needNewline) {
    Serial.println();
  }
}

void IMU::on_receive(const MotionUpdate& msg) {
  if (baro.state() != Barometer::State::Ready) {
    // We can't do anything without simultaneous barometer-measured altitude
    return;
  }
  if (!msg.hasAcceleration || !msg.hasOrientation) {
    // We need to use both acceleration and orientation.
    // In the future, we could potentially collect them separately, but that seems unnecessary given
    // that they almost always occur together.
    return;
  }

  processMotion(msg);

  if (validAccelVert_) {
    // update kalman filter
    kalmanvert_.update(millis() / 1000.0, baro.altF(), accelVert_ * 9.80665f);

    if (LOG::KALMAN && bus_) {
      String kalmanName = "kalman,";
      String kalmanEntryString = kalmanName + String(kalmanvert_.getPosition(), 8) + ',' +
                                 String(kalmanvert_.getVelocity(), 8) + ',' +
                                 String(kalmanvert_.getAcceleration(), 8);
      bus_->receive(CommentMessage(kalmanEntryString));
    }
  }
}

void IMU::wake() { gravityInitCount_ = GRAVITY_INIT_SAMPLES; }

bool IMU::accelValid() { return validAccelTot_; }

float IMU::getAccel() {
  if (!accelValid()) {
    fatalError("IMU::getAccel when not valid");
  }
  return accelTot_;
}

bool IMU::velocityValid() { return gravityInitCount_ == 0; }

float IMU::getVelocity() {
  if (!velocityValid()) {
    fatalError("IMU::getVelocity when not valid");
  }
  return (float)kalmanvert_.getVelocity();
}
