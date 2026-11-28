"use strict";
const Transfer = (() => {
  const cap = 256 * 1024 * 1024,
    chunkSize = 46080;

  function safeName(s) {
    let base = Array.from(String(s || "Track").normalize("NFC").replace(
        /[<>:"/\\|?*\u0000-\u001f\u007f]/g, " ").replace(/\s+/g, " ").trim()).slice(0, 120).join("")
      .replace(/[. ]+$/, "") || "Track";
    if (/^(con|prn|aux|nul|com[1-9¹²³]|lpt[1-9¹²³])(?:\.|$)/i.test(base)) base = "_" + base;
    return base;
  }

  function unique(base, used) {
    base = safeName(base);
    let name = base + ".mp3",
      n = 2;
    while (used.has(name.toLocaleLowerCase("en-US"))) name = base + " (" + (n++) + ").mp3";
    return name;
  }

  function save(blobURL, name) {
    const a = document.createElement("a");
    a.href = blobURL;
    a.download = name;
    document.body.append(a);
    a.click();
    a.remove();
  }
  async function webFetch(path, url, signal) {
    const r = await fetch(path, {
      method: "POST",
      headers: {
        "Content-Type": "text/plain;charset=UTF-8",
        "X-Trackport": "1",
        /* Proxy settings for the local server: the server applies them to
           every curl fetch it performs for this request, so all external
           traffic (catalog lookups, audio downloads, proxy test) routes
           through the configured proxy when it is enabled. */
        ...Proxy.headers()
      },
      body: url,
      signal,
      cache: "no-store"
    });
    if (!r.ok) {
      const t = await r.text();
      throw Error((t || "Download failed. Check the original link.").slice(0, 220));
    }
    return r;
  }
  async function loadSource(j, signal) {
    if (j.file) {
      if (j.file.size > cap) throw Error("This file exceeds the 256 MiB import limit.");
      return j.file.arrayBuffer();
    }
    const r = await webFetch("/api/audio", j.url, signal);
    let b;
    try {
      b = await r.arrayBuffer();
    } catch (e) {
      if (signal.aborted) throw e;
      throw Error("The full transfer did not finish. Check the download link and retry.");
    }
    if (b.byteLength > cap) throw Error("This file exceeds the 256 MiB import limit.");
    return b;
  }

  function encode(audio, bitrate, signal, progress) {
    return new Promise((resolve, reject) => {
      const worker = new Worker("encoder.js"),
        chunks = [];
      let p = 0,
        finished = false;

      function finish(error) {
        if (finished) return;
        finished = true;
        signal.removeEventListener("abort", abort);
        worker.terminate();
        if (error) reject(error);
        else resolve(chunks);
      }

      function abort() {
        finish(new DOMException("Stopped", "AbortError"));
      }

      function send() {
        if (p >= audio.length) {
          worker.postMessage({
            kind: "end"
          });
          return;
        }
        const end = Math.min(p + chunkSize, audio.length),
          left = audio.getChannelData(0).slice(p, end),
          list = [left.buffer];
        const message = {
          kind: "chunk",
          left
        };
        if (audio.numberOfChannels > 1) {
          message.right = audio.getChannelData(1).slice(p, end);
          list.push(message.right.buffer);
        }
        p = end;
        worker.postMessage(message, list);
      }
      worker.onerror = () => finish(Error("The local MP3 encoder could not finish."));
      worker.onmessage = ({
        data
      }) => {
        if (data.kind === "error") {
          finish(Error(data.message));
          return;
        }
        if (data.out && data.out.byteLength) chunks.push(data.out);
        if (data.kind === "ended") {
          finish();
          return;
        }
        if (data.kind === "encoded") progress(Math.round(100 * p / audio.length));
        send();
      };
      signal.addEventListener("abort", abort, {
        once: true
      });
      if (signal.aborted) {
        abort();
        return;
      }
      worker.postMessage({
        kind: "start",
        channels: audio.numberOfChannels,
        bitrate
      });
    });
  }
  const crcTable = Array.from({
    length: 256
  }, (_, a) => {
    for (let k = 0; k < 8; k++) a = (a & 1) ? 0xedb88320 ^ (a >>> 1) : a >>> 1;
    return a >>> 0;
  });
  async function crc(blob) {
    let c = -1;
    for (let p = 0; p < blob.size; p += 1024 * 1024) {
      const bytes = new Uint8Array(await blob.slice(p, p + 1024 * 1024).arrayBuffer());
      for (const x of bytes) c = crcTable[(c ^ x) & 255] ^ (c >>> 8);
    }
    return (c ^ -1) >>> 0;
  }

  function words(size) {
    const a = new Uint8Array(size),
      v = new DataView(a.buffer);
    return {
      a,
      w: (i, n) => v.setUint16(i, n, true),
      d: (i, n) => v.setUint32(i, n, true)
    };
  }
  async function zip(jobs) {
    const contents = [],
      directory = [];
    let offset = 0,
      cdSize = 0;
    if (jobs.length > 65535) throw Error("Use smaller ZIP batches or save each MP3 individually.");
    const now = new Date(),
      time = (now.getHours() << 11) | (now.getMinutes() << 5) | (now.getSeconds() >> 1);
    const date = ((Math.max(1980, now.getFullYear()) - 1980) << 9) | ((now.getMonth() + 1) << 5) |
      now.getDate();
    for (const j of jobs) {
      const name = new TextEncoder().encode(j.outName),
        n = j.blob.size;
      if (offset + 30 + name.length + n + cdSize + 46 + name.length + 22 > 0xffffffff) throw Error(
        "This ZIP would exceed 4 GiB. Save the tracks individually.");
      const digest = await crc(j.blob),
        a = words(30);
      a.d(0, 0x04034b50);
      a.w(4, 20);
      a.w(6, 0x800);
      a.w(10, time);
      a.w(12, date);
      a.d(14, digest);
      a.d(18, n);
      a.d(22, n);
      a.w(26, name.length);
      const b = words(46);
      b.d(0, 0x02014b50);
      b.w(4, 20);
      b.w(6, 20);
      b.w(8, 0x800);
      b.w(12, time);
      b.w(14, date);
      b.d(16, digest);
      b.d(20, n);
      b.d(24, n);
      b.w(28, name.length);
      b.d(42, offset);
      contents.push(a.a, name, j.blob);
      directory.push(b.a, name);
      offset += 30 + name.length + n;
      cdSize += 46 + name.length;
    }
    const end = words(22);
    end.d(0, 0x06054b50);
    end.w(8, jobs.length);
    end.w(10, jobs.length);
    end.d(12, cdSize);
    end.d(16, offset);
    return new Blob([...contents, ...directory, end.a], {
      type: "application/zip"
    });
  }
  return {
    cap,
    safeName,
    unique,
    save,
    webFetch,
    loadSource,
    encode,
    zip
  };
})();
