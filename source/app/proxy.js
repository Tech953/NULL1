"use strict";
/* Proxy settings for Trackport.
 *
 * All external traffic in Trackport flows through Transfer.webFetch() to the
 * local C server, which fetches with system curl. The proxy settings below
 * are attached to every local-server request as X-Proxy-* headers, so the
 * server can apply them to every curl invocation (catalog lookups, audio
 * downloads and the proxy test). A grep of this app confirms webFetch is
 * the only network call in the app layer, so there is no request that can
 * bypass the proxy while it is enabled.
 *
 * Settings persist in localStorage under proxyEnabled, proxyUrl,
 * proxyUsername and proxyPassword. The proxy is off by default.
 */
const Proxy = (() => {
  const KEYS = {
    enabled: "proxyEnabled",
    url: "proxyUrl",
    username: "proxyUsername",
    password: "proxyPassword"
  };
  const SCHEMES = ["http:", "https:", "socks5:", "socks5h:"];

  function load() {
    const out = { enabled: false, url: "", username: "", password: "" };
    try {
      out.enabled = localStorage.getItem(KEYS.enabled) === "1";
      out.url = localStorage.getItem(KEYS.url) || "";
      out.username = localStorage.getItem(KEYS.username) || "";
      out.password = localStorage.getItem(KEYS.password) || "";
    } catch {
      /* Storage unavailable (private mode, blocked cookies): settings live
         only for this session and stay at their defaults. */
    }
    return out;
  }

  let current = load();

  function save(patch) {
    current = { ...current, ...patch };
    try {
      localStorage.setItem(KEYS.enabled, current.enabled ? "1" : "0");
      localStorage.setItem(KEYS.url, current.url);
      localStorage.setItem(KEYS.username, current.username);
      localStorage.setItem(KEYS.password, current.password);
    } catch {
      /* See load(): keep the in-memory values if storage is unavailable. */
    }
    return current;
  }

  /* A proxy URL is a bare scheme + authority: no embedded credentials, no
     path/query/fragment. Credentials go in the Username/Password fields. */
  function validUrl(u) {
    if (!u || typeof u !== "string") return false;
    let parsed;
    try {
      parsed = new URL(u);
    } catch {
      return false;
    }
    if (!SCHEMES.includes(parsed.protocol)) return false;
    if (parsed.username || parsed.password || parsed.search || parsed.hash || !parsed.hostname) return false;
    /* Non-special schemes (socks5:) parse with an empty pathname; http(s)
       parse with "/". Anything else is a path, which is not allowed. */
    if (parsed.pathname !== "" && parsed.pathname !== "/") return false;
    return true;
  }

  /* Credentials travel in HTTP headers to the loopback server and in a curl
     command line on Windows, so they must not contain whitespace, quotes,
     backslashes or control characters. */
  function validCredentials(user, pass) {
    return !/[^\x21-\x7e]/.test(user || "") && !/[^\x21-\x7e]/.test(pass || "") &&
      !/["\\]/.test(user || "") && !/["\\]/.test(pass || "");
  }

  /* Headers for the local server. Empty credentials are omitted. The server
     rejects the request when the proxy is enabled but the URL is invalid,
     so an enabled-but-broken setting fails closed instead of fetching
     directly. */
  function headers() {
    if (!current.enabled || !validUrl(current.url)) return {};
    const h = { "X-Proxy-Enabled": "1", "X-Proxy-Url": current.url };
    if (current.username) h["X-Proxy-User"] = current.username;
    if (current.password) h["X-Proxy-Pass"] = current.password;
    return h;
  }

  /* Fetches a small test URL through the configured proxy via the local
     server and reports latency. The server performs the fetch, so this
     exercises the same curl path as a real download. */
  async function test(signal) {
    if (!current.enabled) throw Error("Enable the proxy first.");
    if (!validUrl(current.url)) throw Error(
      "Enter a valid proxy URL (http://, https:// or socks5://).");
    if (!validCredentials(current.username, current.password)) throw Error(
      "Proxy username and password cannot contain spaces, quotes or control characters.");
    const started = performance.now();
    const r = await Transfer.webFetch("/api/proxy-test", "https://example.com", signal);
    const ms = Math.round(performance.now() - started);
    const detail = (await r.text()).slice(0, 120);
    return { ms, detail };
  }

  return {
    load,
    save,
    validUrl,
    validCredentials,
    headers,
    test,
    get current() {
      return current;
    }
  };
})();
