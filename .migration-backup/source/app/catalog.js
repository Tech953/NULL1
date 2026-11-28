"use strict";
const Catalog = (() => {
  const spotifyID = /^[A-Za-z0-9]{22}$/;

  function ref(raw) {
    raw = raw.trim();
    if (raw.startsWith("spotify:")) {
      const m = /^spotify:track:([A-Za-z0-9]{22})$/.exec(raw);
      if (!m) throw Error("Use a Spotify song link or spotify:track URI.");
      raw = "https://open.spotify.com/track/" + m[1];
    }
    let u;
    try {
      u = new URL(raw);
    } catch {
      throw Error("Paste a full Apple Music or Spotify song link.");
    }
    if (u.protocol !== "https:" || u.username || u.password || u.port) throw Error(
      "Use a public HTTPS song link.");
    const p = u.pathname.split("/").filter(Boolean);
    if (u.hostname === "open.spotify.com") {
      if (/^intl-[a-z-]+$/i.test(p[0])) p.shift();
      if (p.length !== 2 || p[0] !== "track" || !spotifyID.test(p[1])) throw Error(
        "Choose a single song. In Spotify, open its Share menu and copy the song link.");
      const url = "https://open.spotify.com/track/" + p[1];
      return {
        provider: "Spotify",
        url,
        lookup: "https://open.spotify.com/oembed?url=" + encodeURIComponent(url)
      };
    }
    if (u.hostname === "music.apple.com" || u.hostname === "itunes.apple.com") {
      const store = /^[a-z]{2}$/i.test(p[0]) ? p.shift().toLowerCase() : "us";
      const kind = p[0],
        id = kind === "album" ? u.searchParams.get("i") : kind === "song" ? p[p.length - 1] : "";
      if (!/^\d{1,20}$/.test(id || "")) throw Error(
        "Choose a song in Apple Music and copy its link. An album link needs the song’s i= identifier."
        );
      const url = "https://music.apple.com/" + store + "/song/" + id;
      return {
        provider: "Apple Music",
        url,
        lookup: "https://itunes.apple.com/lookup?id=" + id + "&country=" + store,
        id
      };
    }
    if (["spotify.link", "spotify.app.link", "apple.co"].includes(u.hostname)) throw Error(
      "Open this shortened link, then copy the full song URL from the address bar.");
    throw Error("Use an Apple Music or Spotify song link here.");
  }

  function direct(raw) {
    let url;
    try {
      url = new URL(raw.trim());
    } catch {
      throw Error("Paste a complete HTTPS audio download URL.");
    }
    if (url.protocol !== "https:" || url.username || url.password) throw Error(
      "Use an HTTPS download URL without embedded sign-in details.");
    if (["open.spotify.com", "spotify.link", "spotify.app.link", "music.apple.com",
        "itunes.apple.com", "apple.co"
      ].includes(url.hostname)) throw Error(
      "Use the Track link tab for this music-service page, then attach its full audio.");
    if ((url.hostname === "p.scdn.co" && /mp3-preview/.test(url.pathname)) || (
        /(?:^|\.)mzstatic\.com$/.test(url.hostname) && /AudioPreview/i.test(url.pathname)))
    throw Error("This is a catalog preview. Add a link to the full recording.");
    url.hash = "";
    return url.href;
  }
  async function lookup(raw, signal) {
    const track = ref(raw);
    let data;
    try {
      const response = await Transfer.webFetch("/api/catalog", track.lookup, signal);
      data = await response.json();
      if (track.provider === "Spotify") {
        if (typeof data.title !== "string" || !data.title.trim()) throw Error();
        track.title = AudioFiles.clean(data.title);
      } else {
        const item = Array.isArray(data.results) && data.results.find(t => String(t.trackId) ===
          track.id && typeof t.trackName === "string");
        if (!item) throw Error();
        track.title = AudioFiles.clean(item.trackName);
        track.artist = AudioFiles.clean(item.artistName);
        track.album = AudioFiles.clean(item.collectionName);
        if (Number(item.trackTimeMillis) > 0) track.duration = Number(item.trackTimeMillis) / 1000;
      }
    } catch (e) {
      if (signal?.aborted) throw e;
      track.unavailable = true;
    }
    return track;
  }
  return {
    ref,
    direct,
    lookup
  };
})();
