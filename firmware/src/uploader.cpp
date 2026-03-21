#include "uploader.h"

namespace
{
  unsigned long nextBackoffDelay(unsigned long currentDelay, unsigned long maxDelay)
  {
    if (currentDelay >= maxDelay / 2UL)
    {
      return maxDelay;
    }

    return currentDelay * 2UL;
  }

  void buildStationBasePath(String &path)
  {
    // Rebuild the shared station root in one place to avoid duplicated path logic.
    path.reserve(sizeof(config::FIREBASE_ROOT_PATH) + sizeof(STATION_ID));
    path = config::FIREBASE_ROOT_PATH;
    path += "/";
    path += STATION_ID;
  }

  bool ensureStationInfo(FirebaseData &fbdo)
  {
    static bool stationInfoInitialized = false;

    if (stationInfoInitialized)
    {
      return true;
    }

    // Build once from the shared base path so station metadata always lands under
    // /weather_stations/{STATION_ID}/info.
    String infoPath;
    // Reserve path capacity up front to reduce heap churn over long runtimes.
    infoPath.reserve(sizeof(config::FIREBASE_ROOT_PATH) + sizeof(STATION_ID) + sizeof(config::FIREBASE_INFO_SUFFIX));
    buildStationBasePath(infoPath);
    infoPath += config::FIREBASE_INFO_SUFFIX;

    if (Firebase.RTDB.pathExisted(&fbdo, infoPath.c_str()))
    {
      stationInfoInitialized = true;
      return true;
    }

    // Only create metadata when the node is genuinely absent.
    if (fbdo.httpCode() != FIREBASE_ERROR_PATH_NOT_EXIST)
    {
      Serial.printf("Firebase station info check failed: %s\n", fbdo.errorReason().c_str());
      return false;
    }

    FirebaseJson infoJson;
    infoJson.set("name", STATION_ID);
    infoJson.set("location", STATION_LOCATION);

    if (Firebase.RTDB.setJSON(&fbdo, infoPath.c_str(), &infoJson))
    {
      stationInfoInitialized = true;
      Serial.printf("Firebase station info initialized: %s\n", infoPath.c_str());
      return true;
    }

    Serial.printf("Firebase station info init failed: %s\n", fbdo.errorReason().c_str());
    return false;
  }
} // namespace

void Uploader::begin()
{
  initializeFirebaseConfig_();
  resetState_();

  if (uploadTaskHandle_ == nullptr)
  {
    xTaskCreatePinnedToCore(
        Uploader::uploadTaskEntry_,
        "firebase_uploader",
        kUploadTaskStackBytes,
        this,
        1,
        &uploadTaskHandle_,
        1);
  }
}

void Uploader::initializeFirebaseConfig_()
{
  firebaseConfig_.api_key = API_KEY;
  firebaseConfig_.database_url = DATABASE_URL;
  firebaseConfig_.max_token_generation_retry = 5;
  firebaseConfig_.timeout.socketConnection = 10000;
  firebaseConfig_.timeout.serverResponse = 10000;
  firebaseConfig_.timeout.networkReconnect = 10000;
}

void Uploader::resetState_()
{
  head_ = 0;
  tail_ = 0;
  count_ = 0;
  droppedCount_ = 0;
  firebaseStarted_ = false;
  wifiConnected_ = false;
  timeReady_ = false;
  firebaseReady_ = false;
  authPending_ = false;
  lastFirebaseAttemptMs_ = 0;
  lastUploadAttemptMs_ = 0;
  lastSuccessfulUploadMs_ = 0;
  lastQueueProgressMs_ = 0;
  pendingSinceMs_ = 0;
  stallSinceMs_ = 0;
  lastQueueOverflowLogMs_ = 0;
  nextFirebaseAttemptMs_ = 0;
  nextUploadAttemptMs_ = 0;
  firebaseRetryIntervalMs_ = config::FIREBASE_RETRY_INTERVAL_MS;
  uploadRetryIntervalMs_ = config::FIREBASE_UPLOAD_RETRY_INITIAL_MS;
  lastAuthLogMs_ = 0;
  recoveryRequested_ = false;
}

bool Uploader::enqueue(const SensorReading &reading)
{
  if (reading.timestamp == 0ULL)
  {
    Serial.println("Uploader enqueue skipped: clock is not synchronized yet");
    return false;
  }

  bool queued = false;
  bool replacedOldest = false;
  size_t pendingCount = 0;
  const unsigned long nowMs = millis();
  portENTER_CRITICAL(&queueMux_);
  if (count_ == 0U)
  {
    pendingSinceMs_ = nowMs;
  }
  if (count_ < kQueueSize)
  {
    queue_[tail_] = reading;
    tail_ = (tail_ + 1U) % kQueueSize;
    count_++;
    queued = true;
  }
  else
  {
    head_ = (head_ + 1U) % kQueueSize;
    queue_[tail_] = reading;
    tail_ = (tail_ + 1U) % kQueueSize;
    droppedCount_++;
    replacedOldest = true;
    queued = true;
  }
  pendingCount = count_;
  portEXIT_CRITICAL(&queueMux_);

  if (!queued)
  {
    Serial.printf("Uploader queue full: pending=%u dropped=%u\n", static_cast<unsigned>(pendingCount), static_cast<unsigned>(droppedCount()));
    return false;
  }

  if (replacedOldest && (lastQueueOverflowLogMs_ == 0 || nowMs - lastQueueOverflowLogMs_ >= config::QUEUE_OVERFLOW_LOG_INTERVAL_MS))
  {
    Serial.printf(
        "Uploader queue saturated: overwriting oldest sample pending=%u dropped=%u\n",
        static_cast<unsigned>(pendingCount),
        static_cast<unsigned>(droppedCount()));
    lastQueueOverflowLogMs_ = nowMs;
  }

  Serial.printf(
      "Uploader enqueue ok: ts=%llu pending=%u\n",
      static_cast<unsigned long long>(reading.timestamp),
      static_cast<unsigned>(pendingCount));
  return true;
}

void Uploader::update(unsigned long nowMs, bool wifiConnected, bool timeReady)
{
  (void)nowMs;
  portENTER_CRITICAL(&queueMux_);
  wifiConnected_ = wifiConnected;
  timeReady_ = timeReady;
  portEXIT_CRITICAL(&queueMux_);
}

size_t Uploader::pendingCount() const
{
  size_t countSnapshot = 0;
  portENTER_CRITICAL(const_cast<portMUX_TYPE *>(&queueMux_));
  countSnapshot = count_;
  portEXIT_CRITICAL(const_cast<portMUX_TYPE *>(&queueMux_));
  return countSnapshot;
}

size_t Uploader::droppedCount() const
{
  size_t droppedSnapshot = 0;
  portENTER_CRITICAL(const_cast<portMUX_TYPE *>(&queueMux_));
  droppedSnapshot = droppedCount_;
  portEXIT_CRITICAL(const_cast<portMUX_TYPE *>(&queueMux_));
  return droppedSnapshot;
}

unsigned long Uploader::lastSuccessfulUploadMs() const
{
  unsigned long snapshot = 0;
  portENTER_CRITICAL(const_cast<portMUX_TYPE *>(&queueMux_));
  snapshot = lastSuccessfulUploadMs_;
  portEXIT_CRITICAL(const_cast<portMUX_TYPE *>(&queueMux_));
  return snapshot;
}

unsigned long Uploader::lastQueueProgressMs() const
{
  unsigned long snapshot = 0;
  portENTER_CRITICAL(const_cast<portMUX_TYPE *>(&queueMux_));
  snapshot = pendingSinceMs_;
  portEXIT_CRITICAL(const_cast<portMUX_TYPE *>(&queueMux_));
  return snapshot;
}

unsigned long Uploader::stalledDurationMs() const
{
  unsigned long stalledMs = 0;
  portENTER_CRITICAL(const_cast<portMUX_TYPE *>(&queueMux_));
  if (count_ > 0U && stallSinceMs_ != 0 && firebaseReady_ && !authPending_)
  {
    const unsigned long currentMs = millis();
    stalledMs = currentMs - stallSinceMs_;
  }
  portEXIT_CRITICAL(const_cast<portMUX_TYPE *>(&queueMux_));
  return stalledMs;
}

bool Uploader::hasUploadStalled() const
{
  return stalledDurationMs() >= config::UPLOAD_STALL_RESTART_MS;
}

bool Uploader::waitingForAuth() const
{
  bool snapshot = false;
  portENTER_CRITICAL(const_cast<portMUX_TYPE *>(&queueMux_));
  snapshot = authPending_;
  portEXIT_CRITICAL(const_cast<portMUX_TYPE *>(&queueMux_));
  return snapshot;
}

bool Uploader::firebaseReady() const
{
  bool snapshot = false;
  portENTER_CRITICAL(const_cast<portMUX_TYPE *>(&queueMux_));
  snapshot = firebaseReady_;
  portEXIT_CRITICAL(const_cast<portMUX_TYPE *>(&queueMux_));
  return snapshot;
}

void Uploader::requestRecovery()
{
  portENTER_CRITICAL(&queueMux_);
  recoveryRequested_ = true;
  firebaseStarted_ = false;
  firebaseReady_ = false;
  authPending_ = false;
  lastFirebaseAttemptMs_ = 0;
  lastUploadAttemptMs_ = 0;
  nextFirebaseAttemptMs_ = 0;
  nextUploadAttemptMs_ = 0;
  firebaseRetryIntervalMs_ = config::FIREBASE_RETRY_INTERVAL_MS;
  uploadRetryIntervalMs_ = config::FIREBASE_UPLOAD_RETRY_INITIAL_MS;
  stallSinceMs_ = 0;
  portEXIT_CRITICAL(&queueMux_);
}

void Uploader::uploadTaskEntry_(void *context)
{
  static_cast<Uploader *>(context)->uploadTask_();
}

void Uploader::snapshotTaskInputs_(bool &wifiConnected, bool &timeReady, bool &recoveryRequested)
{
  portENTER_CRITICAL(&queueMux_);
  wifiConnected = wifiConnected_;
  timeReady = timeReady_;
  recoveryRequested = recoveryRequested_;
  if (recoveryRequested_)
  {
    recoveryRequested_ = false;
  }
  portEXIT_CRITICAL(&queueMux_);
}

bool Uploader::waitForConnectivity_(unsigned long nowMs, bool wifiConnected, bool timeReady)
{
  if (wifiConnected && timeReady)
  {
    return false;
  }

  updateStallState_(false, nowMs);
  return true;
}

bool Uploader::fetchQueuedReading_(SensorReading &reading, unsigned long nowMs)
{
  if (peekNextReading_(reading))
  {
    return true;
  }

  updateStallState_(false, nowMs);
  return false;
}

bool Uploader::waitForUploadBackoff_(unsigned long nowMs) const
{
  return nextUploadAttemptMs_ != 0 && nowMs < nextUploadAttemptMs_;
}

void Uploader::handleAuthPendingUpload_()
{
  portENTER_CRITICAL(&queueMux_);
  firebaseReady_ = false;
  authPending_ = true;
  stallSinceMs_ = 0;
  portEXIT_CRITICAL(&queueMux_);
  nextUploadAttemptMs_ = 0;
  uploadRetryIntervalMs_ = config::FIREBASE_UPLOAD_RETRY_INITIAL_MS;
  Serial.println("Firebase upload deferred: token is not ready");
}

void Uploader::handleUploadBackoff_(unsigned long nowMs)
{
  nextUploadAttemptMs_ = nowMs + uploadRetryIntervalMs_;
  uploadRetryIntervalMs_ = nextBackoffDelay(uploadRetryIntervalMs_, config::FIREBASE_UPLOAD_RETRY_MAX_MS);
}

void Uploader::handleSuccessfulUpload_(const SensorReading &reading, unsigned long nowMs)
{
  popUploadedReading_(reading, nowMs);
  lastUploadAttemptMs_ = 0;
  nextUploadAttemptMs_ = 0;
  uploadRetryIntervalMs_ = config::FIREBASE_UPLOAD_RETRY_INITIAL_MS;
  Serial.printf("Firebase queue advanced: pending=%u\n", static_cast<unsigned>(pendingCount()));
}

void Uploader::uploadTask_()
{
  // Cloud I/O stays off the Arduino loop task so auth and TLS work cannot stall
  // sensor sampling or UI refresh.
  for (;;)
  {
    bool wifiConnected = false;
    bool timeReady = false;
    bool recoveryRequested = false;
    snapshotTaskInputs_(wifiConnected, timeReady, recoveryRequested);

    const unsigned long nowMs = millis();

    if (recoveryRequested)
    {
      Firebase.reconnectWiFi(true);
      Serial.println("Uploader recovery requested");
    }

    if (waitForConnectivity_(nowMs, wifiConnected, timeReady))
    {
      vTaskDelay(pdMS_TO_TICKS(config::UPLOADER_TASK_INTERVAL_MS));
      continue;
    }

    if (!ensureFirebaseReady_(nowMs))
    {
      updateStallState_(false, nowMs);
      vTaskDelay(pdMS_TO_TICKS(config::UPLOADER_TASK_INTERVAL_MS));
      continue;
    }

    SensorReading reading{};
    if (!fetchQueuedReading_(reading, nowMs))
    {
      vTaskDelay(pdMS_TO_TICKS(config::UPLOADER_TASK_INTERVAL_MS));
      continue;
    }

    updateStallState_(true, nowMs);

    if (waitForUploadBackoff_(nowMs))
    {
      vTaskDelay(pdMS_TO_TICKS(config::UPLOADER_TASK_INTERVAL_MS));
      continue;
    }

    lastUploadAttemptMs_ = nowMs;
    Serial.printf(
        "Firebase upload attempt: ts=%llu pending=%u\n",
        static_cast<unsigned long long>(reading.timestamp),
        static_cast<unsigned>(pendingCount()));

    if (uploadReading_(reading, nowMs))
    {
      handleSuccessfulUpload_(reading, nowMs);
    }
    else if (isFirebaseAuthPending_())
    {
      handleAuthPendingUpload_();
    }
    else
    {
      handleUploadBackoff_(nowMs);
    }

    vTaskDelay(pdMS_TO_TICKS(config::UPLOADER_TASK_INTERVAL_MS));
  }
}

bool Uploader::ensureFirebaseReady_(unsigned long nowMs)
{
  // Firebase.ready() is the library's token engine. Keep calling it from the
  // worker task until the auth token reaches the ready state.
  if (!firebaseStarted_)
  {
    if (nextFirebaseAttemptMs_ != 0 && nowMs < nextFirebaseAttemptMs_)
    {
      return false;
    }

    lastFirebaseAttemptMs_ = nowMs;
    Serial.println("Firebase init attempt");

    if (!Firebase.signUp(&firebaseConfig_, &auth_, "", ""))
    {
      Serial.printf(
          "Firebase sign-up failed: %s. Enable Anonymous authentication in Firebase Console > Authentication > Sign-in method.\n",
          firebaseConfig_.signer.signupError.message.c_str());
      firebaseRetryIntervalMs_ = nextBackoffDelay(firebaseRetryIntervalMs_, config::FIREBASE_INIT_RETRY_MAX_MS);
      nextFirebaseAttemptMs_ = nowMs + firebaseRetryIntervalMs_;
      return false;
    }

    Firebase.begin(&firebaseConfig_, &auth_);
    Firebase.reconnectWiFi(true);
    firebaseStarted_ = true;
    firebaseReady_ = false;
    authPending_ = true;
    firebaseRetryIntervalMs_ = config::FIREBASE_RETRY_INTERVAL_MS;
    nextFirebaseAttemptMs_ = 0;
    lastAuthLogMs_ = 0;
    Serial.println("Firebase initialized, waiting for token ready");
  }

  const bool ready = Firebase.ready();
  portENTER_CRITICAL(&queueMux_);
  firebaseReady_ = ready;
  authPending_ = !ready;
  if (!ready)
  {
    stallSinceMs_ = 0;
  }
  portEXIT_CRITICAL(&queueMux_);

  if (ready)
  {
    nextUploadAttemptMs_ = 0;
    uploadRetryIntervalMs_ = config::FIREBASE_UPLOAD_RETRY_INITIAL_MS;
    return true;
  }

  if (lastAuthLogMs_ == 0 || nowMs - lastAuthLogMs_ >= config::FIREBASE_AUTH_LOG_INTERVAL_MS)
  {
    const TokenInfo tokenInfo = Firebase.authTokenInfo();
    Serial.printf(
        "Firebase waiting for token: status=%d code=%d message=%s\n",
        static_cast<int>(tokenInfo.status),
        static_cast<int>(tokenInfo.error.code),
        tokenInfo.error.message.c_str());
    lastAuthLogMs_ = nowMs;
  }

  return false;
}

bool Uploader::uploadReading_(const SensorReading &reading, unsigned long nowMs)
{
  char timestampBuffer[24];
  snprintf(timestampBuffer, sizeof(timestampBuffer), "%llu", static_cast<unsigned long long>(reading.timestamp));

  // Route writes into a per-station namespace so multiple ESP32 nodes can
  // share one Firebase database without overwriting each other.
  String basePath;
  buildStationBasePath(basePath);

  // Reuse the shared base path and append suffixes incrementally to avoid
  // ambiguous operator+ chains and extra temporary Strings.
  String latestPath = basePath;
  // Reserve once to avoid extra reallocations in the long-running uploader task.
  latestPath.reserve(basePath.length() + sizeof(config::FIREBASE_LATEST_SUFFIX));
  latestPath += config::FIREBASE_LATEST_SUFFIX;

  String historyPath = basePath;
  historyPath.reserve(basePath.length() + sizeof(config::FIREBASE_HISTORY_SUFFIX) + sizeof(timestampBuffer));
  historyPath += config::FIREBASE_HISTORY_SUFFIX;
  historyPath += timestampBuffer;

  FirebaseJson json;
  json.set("deviceId", DEVICE_ID);
  json.set("timestamp", static_cast<double>(reading.timestamp));
  json.set("temperature", reading.temperatureC);
  json.set("humidity", reading.humidityPct);
  json.set("rain", reading.isRaining ? 1 : 0);
  json.set("pressure", reading.pressureHpa);

  // Optional station metadata is initialized once and then left untouched.
  (void)ensureStationInfo(fbdo_);

  // Keep the existing payload unchanged while writing both the current
  // snapshot and the timestamped history entry.
  if (!Firebase.RTDB.setJSON(&fbdo_, latestPath.c_str(), &json))
  {
    Serial.printf(
        "Firebase latest upload failed: %s (ts=%llu, at=%lu ms)\n",
        fbdo_.errorReason().c_str(),
        static_cast<unsigned long long>(reading.timestamp),
        static_cast<unsigned long>(nowMs));
    return false;
  }

  if (!Firebase.RTDB.setJSON(&fbdo_, historyPath.c_str(), &json))
  {
    Serial.printf(
        "Firebase history upload failed: %s (ts=%llu, at=%lu ms)\n",
        fbdo_.errorReason().c_str(),
        static_cast<unsigned long long>(reading.timestamp),
        static_cast<unsigned long>(nowMs));
    return false;
  }

  Serial.printf(
      "Firebase upload ok ts=%llu T=%.2fC H=%.2f%% P=%.2fhPa Rain=%s\n",
      static_cast<unsigned long long>(reading.timestamp),
      reading.temperatureC,
      reading.humidityPct,
      reading.pressureHpa,
      reading.isRaining ? "YES" : "NO");
  return true;
}

bool Uploader::isFirebaseAuthPending_() const
{
  const TokenInfo tokenInfo = Firebase.authTokenInfo();
  return tokenInfo.status != token_status_ready ||
         const_cast<FirebaseData &>(fbdo_).httpCode() == FIREBASE_ERROR_TOKEN_NOT_READY;
}

void Uploader::updateStallState_(bool canAttemptUpload, unsigned long nowMs)
{
  portENTER_CRITICAL(&queueMux_);
  if (canAttemptUpload && count_ > 0U)
  {
    if (stallSinceMs_ == 0)
    {
      stallSinceMs_ = nowMs;
    }
  }
  else
  {
    stallSinceMs_ = 0;
  }
  portEXIT_CRITICAL(&queueMux_);
}

bool Uploader::peekNextReading_(SensorReading &outReading) const
{
  bool hasReading = false;
  portENTER_CRITICAL(const_cast<portMUX_TYPE *>(&queueMux_));
  if (count_ > 0U)
  {
    outReading = queue_[head_];
    hasReading = true;
  }
  portEXIT_CRITICAL(const_cast<portMUX_TYPE *>(&queueMux_));
  return hasReading;
}

void Uploader::popUploadedReading_(const SensorReading &uploadedReading, unsigned long nowMs)
{
  portENTER_CRITICAL(&queueMux_);
  if (count_ > 0U && queue_[head_].timestamp == uploadedReading.timestamp)
  {
    head_ = (head_ + 1U) % kQueueSize;
    count_--;
    lastSuccessfulUploadMs_ = nowMs;
    lastQueueProgressMs_ = nowMs;
    pendingSinceMs_ = count_ > 0U ? nowMs : 0;
    stallSinceMs_ = count_ > 0U ? nowMs : 0;
  }
  portEXIT_CRITICAL(&queueMux_);
}
