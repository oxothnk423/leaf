#include "comms/leaf_log_credentials.h"

#include <Preferences.h>

namespace leaf_log_credentials {
  namespace {
    constexpr const char* NAMESPACE = "leafLog";
    constexpr const char* TOKEN_KEY = "token";
    constexpr const char* HANDLE_KEY = "handle";
    constexpr const char* DISPLAY_KEY = "display";
    constexpr const char* RECONNECT_KEY = "reconnect";

    constexpr const char* ACTIVE_SLOT_KEY = "active";
    constexpr uint8_t SLOT_A = 0;
    constexpr uint8_t SLOT_B = 1;
    constexpr uint8_t NO_ACTIVE_SLOT = 0xff;

    struct SlotKeys {
      const char* token;
      const char* handle;
      const char* display;
      const char* reconnect;
    };

    constexpr SlotKeys SLOT_KEYS[] = {{"tokenA", "handleA", "displayA", "reconnA"},
                                      {"tokenB", "handleB", "displayB", "reconnB"}};

    Snapshot loadSnapshot(Preferences& prefs, const SlotKeys& keys) {
      Snapshot result;
      result.token = prefs.getString(keys.token);
      result.handle = prefs.getString(keys.handle);
      result.displayName = prefs.getString(keys.display);
      result.reconnectRequired = prefs.getBool(keys.reconnect, false);
      return result;
    }

    bool snapshotMatches(const Snapshot& snapshot, const String& token, const String& handle,
                         const String& displayName) {
      return snapshot.token == token && snapshot.handle == handle &&
             snapshot.displayName == displayName && !snapshot.reconnectRequired;
    }

    void clearLegacyKeys(Preferences& prefs) {
      prefs.remove(TOKEN_KEY);
      prefs.remove(HANDLE_KEY);
      prefs.remove(DISPLAY_KEY);
      prefs.remove(RECONNECT_KEY);
    }
  }  // namespace

  Snapshot load() {
    Preferences prefs;
    if (!prefs.begin(NAMESPACE, true)) return Snapshot();

    const uint8_t activeSlot = prefs.getUChar(ACTIVE_SLOT_KEY, NO_ACTIVE_SLOT);
    Snapshot result;
    if (activeSlot == SLOT_A || activeSlot == SLOT_B) {
      result = loadSnapshot(prefs, SLOT_KEYS[activeSlot]);
    } else {
      // Backward compatibility for credentials stored before the two-slot format.
      result.token = prefs.getString(TOKEN_KEY);
      result.handle = prefs.getString(HANDLE_KEY);
      result.displayName = prefs.getString(DISPLAY_KEY);
      result.reconnectRequired = prefs.getBool(RECONNECT_KEY, false);
    }
    prefs.end();
    return result;
  }

  bool store(const String& token, const String& handle, const String& displayName) {
    if (!token.startsWith("llk_") || token.length() < 5 || handle.isEmpty() ||
        displayName.isEmpty()) {
      return false;
    }

    Preferences prefs;
    if (!prefs.begin(NAMESPACE, false)) return false;

    const uint8_t activeSlot = prefs.getUChar(ACTIVE_SLOT_KEY, NO_ACTIVE_SLOT);
    const uint8_t stagingSlot = activeSlot == SLOT_A ? SLOT_B : SLOT_A;
    const SlotKeys& keys = SLOT_KEYS[stagingSlot];

    // Invalidate the staging slot first. The currently active slot (or legacy keys) remains
    // selected until every new value has been written and read back successfully.
    prefs.remove(keys.token);
    const bool written = prefs.putString(keys.handle, handle) == handle.length() &&
                         prefs.putString(keys.display, displayName) == displayName.length() &&
                         prefs.putBool(keys.reconnect, false) == 1 &&
                         prefs.putString(keys.token, token) == token.length();
    const bool verified =
        written && snapshotMatches(loadSnapshot(prefs, keys), token, handle, displayName);
    const bool committed =
        verified && prefs.putUChar(ACTIVE_SLOT_KEY, stagingSlot) == sizeof(stagingSlot);
    if (committed) clearLegacyKeys(prefs);
    prefs.end();
    return committed;
  }

  bool updateAccount(const String& handle, const String& displayName) {
    const Snapshot current = load();
    if (!current.linked()) return false;
    if (current.handle == handle && current.displayName == displayName) return true;
    return store(current.token, handle, displayName);
  }

  void markReconnectRequired() {
    Preferences prefs;
    if (!prefs.begin(NAMESPACE, false)) return;
    const uint8_t activeSlot = prefs.getUChar(ACTIVE_SLOT_KEY, NO_ACTIVE_SLOT);
    if (activeSlot == SLOT_A || activeSlot == SLOT_B) {
      const SlotKeys& keys = SLOT_KEYS[activeSlot];
      // Removing the token first guarantees that a power loss cannot leave this credential linked.
      prefs.remove(keys.token);
      prefs.putBool(keys.reconnect, true);
    } else {
      prefs.remove(TOKEN_KEY);
      prefs.putBool(RECONNECT_KEY, true);
    }
    prefs.end();
  }

  void clear() {
    Preferences prefs;
    if (!prefs.begin(NAMESPACE, false)) return;
    prefs.clear();
    prefs.end();
  }
}  // namespace leaf_log_credentials
