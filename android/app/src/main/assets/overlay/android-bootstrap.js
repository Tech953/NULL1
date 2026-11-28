"use strict";
(() => {
  const bridge = window.TrackportAndroid;
  if (!bridge) return;
  const allowed = new Set([
    "proxyEnabled",
    "proxyUrl",
    "proxyUsername",
    "proxyPassword"
  ]);
  const storage = {
    getItem(key) {
      return allowed.has(String(key)) ? bridge.getPreference(String(key)) : null;
    },
    setItem(key, value) {
      if (allowed.has(String(key))) bridge.setPreference(String(key), String(value));
    },
    removeItem(key) {
      if (allowed.has(String(key))) bridge.setPreference(String(key), "");
    },
    key(index) {
      return [...allowed][index] || null;
    },
    get length() {
      return allowed.size;
    }
  };
  Object.defineProperty(window, "localStorage", {
    configurable: false,
    enumerable: true,
    value: storage,
    writable: false
  });
})();
