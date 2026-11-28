"use strict";
if (window.TrackportAndroid) {
  Transfer.save = async (blob, name) => {
    if (!(blob instanceof Blob)) {
      window.alert("The export is not available to save.");
      return;
    }
    try {
      const response = await fetch("/api/export", {
        method: "POST",
        headers: {
          "Content-Type": "application/octet-stream",
          "X-Trackport": "1"
        },
        body: blob,
        cache: "no-store"
      });
      if (!response.ok) {
        const message = (await response.text()).slice(0, 200);
        throw new Error(message || "The local export could not be prepared.");
      }
      const exportId = (await response.text()).trim();
      window.TrackportAndroid.saveExport(exportId, name);
    } catch (error) {
      window.alert(error?.message || "The export could not be saved.");
    }
  };
}
