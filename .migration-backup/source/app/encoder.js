"use strict";
importScripts("vendor/lame.all.js");
let encoder, channels;
const quantize = a => {
  const b = new Int16Array(a.length);
  for (let i = 0; i < a.length; i++) {
    const s = Math.max(-1, Math.min(1, a[i]));
    b[i] = Math.round(s * (s < 0 ? 32768 : 32767));
  }
  return b;
};
self.onmessage = ({
  data
}) => {
  try {
    if (data.kind === "start") {
      channels = data.channels;
      encoder = new lamejs.Mp3Encoder(channels, 44100, data.bitrate);
      self.postMessage({
        kind: "ready"
      });
    } else if (data.kind === "chunk") {
      const left = quantize(data.left),
        right = channels === 2 ? quantize(data.right) : null;
      const out = channels === 2 ? encoder.encodeBuffer(left, right) : encoder.encodeBuffer(left);
      self.postMessage({
        kind: "encoded",
        out
      }, [out.buffer]);
    } else if (data.kind === "end") {
      const out = encoder.flush();
      self.postMessage({
        kind: "ended",
        out
      }, [out.buffer]);
      self.close();
    }
  } catch (e) {
    self.postMessage({
      kind: "error",
      message: e.message || "The encoder failed."
    });
    self.close();
  }
};
