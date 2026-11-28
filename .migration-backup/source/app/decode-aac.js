"use strict";
importScripts("vendor/aurora.js", "vendor/aac-decoder.js");
self.onmessage = ({
  data
}) => {
  try {
    const asset = AV.Asset.fromBuffer(new Uint8Array(data));
    asset.on("error", () => {
      self.postMessage({
        error: "The AAC decoder could not read this recording. Choose a complete, DRM-free AAC-LC source."
      });
      self.close();
    });
    asset.decodeToBuffer(samples => {
      const {
        channelsPerFrame: channels,
        sampleRate
      } = asset.format;
      if (!channels || !sampleRate || samples.length < channels) {
        self.postMessage({
          error: "The AAC recording has no decodable samples."
        });
      } else {
        self.postMessage({
          samples,
          channels,
          sampleRate
        }, [samples.buffer]);
      }
      self.close();
    });
  } catch (error) {
    self.postMessage({
      error: error.message || "The AAC decoder could not read this recording."
    });
    self.close();
  }
};
