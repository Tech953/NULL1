"use strict";
const YouTube = (() => {
  function queryFor(track) {
    return [track.title, track.artist, track.album]
      .filter(value => typeof value === "string" && value.trim())
      .map(value => value.trim())
      .join(" ")
      .slice(0, 700);
  }

  async function search(track, signal) {
    if (!track || typeof track.title !== "string" || !track.title.trim()) {
      throw Error("Look up a track before searching YouTube.");
    }
    const query = queryFor(track);
    if (!query) throw Error("The catalog did not provide a track title.");
    const response = await Transfer.webFetch("/api/youtube-search", query, signal);
    const text = await response.text();
    const results = [];
    for (const line of text.split(/\r?\n/)) {
      if (!line.trim()) continue;
      let result;
      try {
        result = JSON.parse(line);
      } catch {
        throw Error("The YouTube search returned an unreadable result.");
      }
      if (!/^[A-Za-z0-9_-]{11}$/.test(result.id) ||
          typeof result.title !== "string" || !result.title.trim()) continue;
      results.push({
        id: result.id,
        title: result.title,
        channel: typeof result.channel === "string" && result.channel.trim() ?
          result.channel : "Unknown channel",
        duration: Number.isFinite(Number(result.duration)) && Number(result.duration) > 0 ?
          Number(result.duration) : null
      });
      if (results.length === 5) break;
    }
    return results;
  }

  return { search };
})();
