"use strict";
const AudioFiles = (() => {
  const ascii = (v, p, n) => String.fromCharCode(...v.subarray(p, p + n));
  const view = v => new DataView(v.buffer, v.byteOffset, v.byteLength);
  const u32 = (v, p, little = false) => view(v).getUint32(p, little);
  const clean = s => String(s || "").replace(/\0/g, " ").trim().slice(0, 512);

  function string(v, encoding) {
    try {
      return clean(new TextDecoder(encoding).decode(v));
    } catch {
      return "";
    }
  }
  const syncSize = (v, p) => (v[p] << 21) + (v[p + 1] << 14) + (v[p + 2] << 7) + v[p + 3];

  function id3(v) {
    const tags = {};
    let offset = 0;
    while (offset + 10 <= v.length && ascii(v, offset, 3) === "ID3") {
      const pos = offset,
        version = v[pos + 3],
        size = syncSize(v, pos + 6),
        end = pos + 10 + size;
      if (end > v.length || size < 0) throw Error(
        "The ID3 tag is incomplete. Download the source again.");
      offset = end + (version === 4 && (v[pos + 5] & 16) ? 10 : 0);
      if (![3, 4].includes(version) || (v[pos + 5] & 0xc0)) continue;
      for (let p = pos + 10; p + 10 <= end;) {
        const key = ascii(v, p, 4),
          len = version === 4 ? syncSize(v, p + 4) : u32(v, p + 4);
        if (!len || p + 10 + len > end || !/^[A-Z0-9]{4}$/.test(key)) break;
        const name = {
          TIT2: "title",
          TPE1: "artist",
          TALB: "album",
          TRCK: "track"
        } [key];
        if (name && !(v[p + 9] & (version === 4 ? 0x4f : 0xe0))) {
          const enc = v[p + 10],
            b = v.subarray(p + 11, p + 10 + len);
          const codec = ["windows-1252", b[0] === 0xfe && b[1] === 0xff ? "utf-16be" : "utf-16le",
            "utf-16be", "utf-8"
          ][enc];
          if (codec) tags[name] = string(b, codec);
        }
        p += 10 + len;
      }
    }
    return {
      tags,
      offset
    };
  }

  function frame(v, p) {
    if (p + 4 > v.length || v[p] !== 255 || (v[p + 1] & 224) !== 224) return null;
    const ver = (v[p + 1] >> 3) & 3,
      layer = (v[p + 1] >> 1) & 3,
      rate = (v[p + 2] >> 2) & 3,
      idx = v[p + 2] >> 4;
    if (ver === 1 || layer !== 1 || rate === 3 || idx === 0 || idx === 15) return null;
    const sample = [44100, 48000, 32000][rate] / (ver === 3 ? 1 : ver === 2 ? 2 : 4);
    const kb = (ver === 3 ? [0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320] : [0,
      8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160
    ])[idx];
    return {
      size: Math.floor((ver === 3 ? 144000 : 72000) * kb / sample) + ((v[p + 2] >> 1) & 1),
      sample,
      seconds: (ver === 3 ? 1152 : 576) / sample
    };
  }

  function mp3(v, start) {
    for (let p = start; p < Math.min(start + 2048, v.length - 3); p++) {
      const first = frame(v, p);
      if (!first) continue;
      const second = frame(v, p + first.size);
      if (!second || first.sample !== second.sample) continue;
      let i = p,
        secs = 0;
      while (i + 4 <= v.length) {
        const f = frame(v, i);
        if (!f) break;
        if (i + f.size > v.length) throw Error(
          "The last MP3 frame is incomplete. Download the full file again.");
        secs += f.seconds;
        i += f.size;
      }
      if (i < v.length && v[i] === 255 && v.length - i < 4) throw Error(
        "The last MP3 frame is incomplete.");
      return {
        seconds: secs
      };
    }
    return null;
  }

  function flac(v) {
    const tags = {};
    let p = 4;
    while (p + 4 <= v.length) {
      const header = v[p],
        len = v[p + 1] * 65536 + v[p + 2] * 256 + v[p + 3],
        end = p + 4 + len;
      if (end > v.length) break;
      if ((header & 127) === 4 && len >= 8) {
        let q = p + 4;
        const vendor = u32(v, q, true);
        q += 4 + vendor;
        if (q + 4 > end) break;
        const count = u32(v, q, true);
        q += 4;
        for (let k = 0; k < count && q + 4 <= end; k++) {
          const n = u32(v, q, true);
          q += 4;
          if (q + n > end) break;
          const comment = string(v.subarray(q, q + n), "utf-8");
          q += n;
          const i = comment.indexOf("="),
            name = {
              TITLE: "title",
              ARTIST: "artist",
              ALBUM: "album",
              TRACKNUMBER: "track"
            } [comment.slice(0, i).toUpperCase()];
          if (i > 0 && name && !tags[name]) tags[name] = clean(comment.slice(i + 1));
        }
      }
      p = end;
      if (header & 128) break;
    }
    return tags;
  }

  function wav(v) {
    const tags = {};
    for (let p = 12; p + 8 <= v.length;) {
      const n = u32(v, p + 4, true),
        end = p + 8 + n;
      if (end > v.length) break;
      if (ascii(v, p, 4) === "LIST" && n >= 4 && ascii(v, p + 8, 4) === "INFO") {
        for (let q = p + 12; q + 8 <= end;) {
          const sz = u32(v, q + 4, true),
            stop = q + 8 + sz;
          if (stop > end) break;
          const name = {
            INAM: "title",
            IART: "artist",
            IPRD: "album",
            ITRK: "track"
          } [ascii(v, q, 4)];
          if (name) tags[name] = string(v.subarray(q + 8, stop), "utf-8");
          q = stop + (sz & 1);
        }
      }
      p = end + (n & 1);
    }
    return tags;
  }

  function mp4(v) {
    const tags = {};
    let protectedAudio = false;

    function walk(start, end, depth) {
      if (depth > 10) return;
      for (let p = start; p + 8 <= end;) {
        let size = u32(v, p),
          head = 8;
        const key = ascii(v, p + 4, 4);
        if (size === 1) {
          if (p + 16 > end || u32(v, p + 8) !== 0) return;
          size = u32(v, p + 12);
          head = 16;
        }
        if (size === 0) size = end - p;
        const q = p + head,
          stop = p + size;
        if (size < head || stop > end) break;
        if (["moov", "udta", "ilst", "trak", "mdia", "minf", "stbl"].includes(key)) walk(q, stop,
          depth + 1);
        else if (key === "meta" && q + 4 <= stop) walk(q + 4, stop, depth + 1);
        else if (key === "stsd" && q + 8 <= stop) walk(q + 8, stop, depth + 1);
        else if (["drms", "enca"].includes(key)) protectedAudio = true;
        else {
          const name = {
            "©nam": "title",
            "©ART": "artist",
            "aART": "albumArtist",
            "©alb": "album",
            "trkn": "track"
          } [key];
          if (name && q + 16 <= stop && ascii(v, q + 4, 4) === "data") {
            const dataEnd = q + u32(v, q);
            if (dataEnd >= q + 16 && dataEnd <= stop) {
              if (name === "track" && dataEnd >= q + 20) tags.track = String(view(v).getUint16(q +
                18));
              else tags[name] = string(v.subarray(q + 16, dataEnd), "utf-8");
            }
          }
        }
        p = stop;
      }
    }
    walk(0, v.length, 0);
    if (protectedAudio) throw Error(
      "This recording is protected. Attach a DRM-free purchase, original recording or artist download."
      );
    if (!tags.artist) tags.artist = tags.albumArtist;
    return tags;
  }

  function inspect(v, filename) {
    if (v.length < 8) throw Error("This source is empty or incomplete.");
    if (/\.m4p$/i.test(filename)) throw Error(
      "This is a protected Apple Music file. Choose a DRM-free copy.");
    const prefix = ascii(v, 0, Math.min(128, v.length)).trimStart().toLowerCase();
    if (prefix.startsWith("<!doctype") || prefix.startsWith("<html") || prefix.startsWith("<?xml") ||
      prefix.startsWith("#extm3u") || prefix.startsWith("<mpd")) throw Error(
      "This link returned a page or playlist. Use the direct link to the full audio file.");
    const info = id3(v);
    const stream = mp3(v, info.offset);
    if (stream) return {
      isMp3: true,
      tags: info.tags,
      duration: stream.seconds
    };
    const sig = ascii(v, 0, 4);
    if (sig === "fLaC") return {
      tags: flac(v)
    };
    if (["RIFF", "RF64"].includes(sig) && ascii(v, 8, 4) === "WAVE") return {
      tags: wav(v)
    };
    if (ascii(v, 4, 4) === "ftyp") return {
      tags: mp4(v)
    };
    if (/\.mp3$/i.test(filename)) throw Error(
      "No complete MPEG Layer III frame sequence was found in this MP3.");
    return {
      tags: info.tags
    };
  }

  function tagBytes(tags) {
    const frames = [];
    for (const [name, key] of Object.entries({
        title: "TIT2",
        artist: "TPE1",
        album: "TALB",
        track: "TRCK"
      })) {
      const t = clean(tags[name]);
      if (!t) continue;
      const body = new Uint8Array(3 + t.length * 2);
      body.set([1, 255, 254]);
      for (let i = 0; i < t.length; i++) view(body).setUint16(3 + 2 * i, t.charCodeAt(i), true);
      const frame = new Uint8Array(10 + body.length);
      frame.set(new TextEncoder().encode(key));
      view(frame).setUint32(4, body.length);
      frame.set(body, 10);
      frames.push(frame);
    }
    const length = frames.reduce((n, f) => n + f.length, 0),
      out = new Uint8Array(10 + length);
    out.set([73, 68, 51, 3, 0, 0, (length >>> 21) & 127, (length >>> 14) & 127, (length >>> 7) & 127,
      length & 127
    ]);
    let p = 10;
    for (const f of frames) {
      out.set(f, p);
      p += f.length;
    }
    return out;
  }

  function decodeAAC(bytes, signal) {
    return new Promise((resolve, reject) => {
      const worker = new Worker("decode-aac.js");

      function finish(error, result) {
        signal.removeEventListener("abort", abort);
        worker.terminate();
        if (error) reject(error);
        else resolve(result);
      }

      function abort() {
        finish(new DOMException("Stopped", "AbortError"));
      }
      if (signal.aborted) {
        abort();
        return;
      }
      signal.addEventListener("abort", abort, {
        once: true
      });
      worker.onerror = () => finish(Error("The offline AAC decoder could not read this file."));
      worker.onmessage = ({
        data
      }) => {
        if (data.error) {
          finish(Error(data.error));
          return;
        }
        try {
          const len = Math.floor(data.samples.length / data.channels);
          const audio = new OfflineAudioContext(1, 1, 44100).createBuffer(data.channels, len,
            data.sampleRate);
          for (let c = 0; c < data.channels; c++) {
            const dest = audio.getChannelData(c);
            for (let i = 0, p = c; i < len; i++, p += data.channels) dest[i] = data.samples[p];
          }
          finish(null, audio);
        } catch (e) {
          finish(e);
        }
      };
      worker.postMessage(bytes, [bytes]);
    });
  }
  async function decode(bytes, signal) {
    const ac = new OfflineAudioContext(2, 1, 44100);
    const magic = new Uint8Array(bytes, 0, Math.min(bytes.byteLength, 12));
    const aac = ascii(magic, 4, 4) === "ftyp" || (magic[0] === 255 && (magic[1] & 0xf6) === 0xf0);
    let src;
    try {
      src = await ac.decodeAudioData(aac ? bytes.slice(0) : bytes);
    } catch {
      if (!aac) throw Error(
        "The browser could not decode this recording. Try a full DRM-free WAV, FLAC, MP3 or M4A in a current browser."
        );
      src = await decodeAAC(bytes, signal);
    }
    if (!src.length) throw Error("The recording has no audio samples.");
    if (src.numberOfChannels <= 2 && src.sampleRate === 44100) return src;
    const count = Math.max(1, Math.round(src.duration * 44100));
    const mix = new OfflineAudioContext(Math.min(2, src.numberOfChannels), count, 44100),
      node = mix.createBufferSource();
    node.buffer = src;
    node.connect(mix.destination);
    node.start();
    return mix.startRendering();
  }
  return {
    inspect,
    tagBytes,
    decode,
    clean
  };
})();
