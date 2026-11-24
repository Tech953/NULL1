"use strict";
(() => {
  const el = id => document.getElementById(id),
    jobs = [];
  let reference = null,
    pickerRef = null,
    lookupTask = null,
    controller = null,
    active = false,
    zipping = false,
    closed = false;
  const msg = (id, t) => {
    const node = el(id);
    node.textContent = t || "";
    node.hidden = !t;
  };
  const duration = t => {
    const s = Math.max(0, Math.round(t));
    return Math.floor(s / 60) + ":" + String(s % 60).padStart(2, "0");
  };
  const checkSignal = s => {
    if (s.aborted) throw new DOMException("Stopped", "AbortError");
  };
  const trimExt = s => s.replace(/\.[a-z0-9]{1,6}$/i, "") || "Track";

  function basename(j) {
    if (j.file) return j.file.name;
    try {
      return decodeURIComponent(new URL(j.url).pathname.split("/").pop() || "Track");
    } catch {
      return "Track";
    }
  }

  function element(tag, cls, text) {
    const n = document.createElement(tag);
    if (cls) n.className = cls;
    if (text !== undefined) n.textContent = text;
    return n;
  }

  function button(text, cls, fn) {
    const b = element("button", cls, text);
    b.type = "button";
    b.addEventListener("click", fn);
    return b;
  }

  function tab(id, focus = false) {
    for (const t of document.querySelectorAll("[role=tab]")) {
      const selected = t.dataset.tab === id;
      t.setAttribute("aria-selected", String(selected));
      t.tabIndex = selected ? 0 : -1;
      el(t.dataset.tab + "-pane").hidden = !selected;
      if (selected && focus) t.focus();
    }
    msg("intake-msg", "");
  }
  const tabs = Array.from(document.querySelectorAll("[role=tab]"));
  tabs.forEach((b, i) => {
    b.onclick = () => tab(b.dataset.tab);
    b.onkeydown = e => {
      const j = {
        ArrowLeft: (i + 2) % 3,
        ArrowRight: (i + 1) % 3,
        Home: 0,
        End: 2
      } [e.key];
      if (j !== undefined) {
        e.preventDefault();
        tab(tabs[j].dataset.tab, true);
      }
    };
  });

  function setReference(r) {
    reference = r;
    el("reference").hidden = !r;
    el("attach-form").hidden = true;
    el("paired-url").value = "";
    if (!r) return;
    el("ref-provider").textContent = r.provider;
    el("ref-title").textContent = r.title || "Song reference";
    el("ref-artist").textContent = r.artist || "";
    el("ref-open").href = r.url;
    el("apple-purchase-help").hidden = r.provider !== "Apple Music";
    el("ref-open").textContent = "Open in " + r.provider + " ↗";
    el("ref-note").textContent = r.unavailable ?
      "The catalog lookup was unavailable. You can still attach the full recording and use its own tags." :
      "Attach the full recording you want to export.";
  }
  el("clear-ref").onclick = () => setReference(null);
  el("attach-url").onclick = () => {
    el("attach-form").hidden = false;
    el("paired-url").focus();
  };
  el("ref-form").onsubmit = async e => {
    e.preventDefault();
    if (lookupTask) lookupTask.abort();
    const task = new AbortController();
    lookupTask = task;
    setReference(null);
    msg("intake-msg", "");
    el("lookup").textContent = "Looking up…";
    el("lookup").disabled = true;
    try {
      const r = await Catalog.lookup(el("ref-url").value, task.signal);
      if (lookupTask === task) setReference(r);
    } catch (err) {
      if (!task.signal.aborted) msg("intake-msg", err.message);
    } finally {
      if (lookupTask === task) {
        lookupTask = null;
        el("lookup").textContent = "Look up track";
        el("lookup").disabled = false;
      }
    }
  };

  function add(j) {
    j.state = "queued";
    j.note = "Queued";
    j.label = j.ref?.title || trimExt(basename(j));
    jobs.push(j);
    msg("queue-msg", "");
  }

  function addFiles(files, ref) {
    for (const file of files) add({
      file,
      ref
    });
    refresh();
  }

  function pick(r) {
    pickerRef = r;
    el("picker").multiple = !r;
    el("picker").value = "";
    el("picker").click();
  }
  el("choose").onclick = () => pick(null);
  el("attach-file").onclick = () => pick(reference);
  el("picker").onchange = () => {
    const files = Array.from(el("picker").files);
    if (files.length) {
      addFiles(files, pickerRef);
      if (pickerRef) setReference(null);
    }
    pickerRef = null;
  };
  const drop = el("drop");
  for (const event of ["dragover", "dragenter"]) {
    drop.addEventListener(event, e => {
      e.preventDefault();
      drop.classList.add("drag");
    });
  }
  for (const event of ["dragleave", "drop"]) {
    drop.addEventListener(event, e => {
      e.preventDefault();
      drop.classList.remove("drag");
    });
  }
  drop.addEventListener("drop", e => addFiles(Array.from(e.dataTransfer.files), null));
  document.addEventListener("dragover", e => e.preventDefault());
  document.addEventListener("drop", e => e.preventDefault());

  function urlForm(e, input, r) {
    e.preventDefault();
    try {
      const url = Catalog.direct(el(input).value);
      add({
        url,
        ref: r
      });
      el(input).value = "";
      if (r) setReference(null);
      msg("intake-msg", "");
      refresh();
    } catch (err) {
      msg("intake-msg", err.message);
    }
  }
  el("url-form").onsubmit = e => urlForm(e, "direct-url", null);
  el("attach-form").onsubmit = e => urlForm(e, "paired-url", reference);
  const desc = j => j.ref ? j.ref.provider + " reference" : j.file ? "Local file" : "Direct download";

  function remove(j) {
    if (j.objectURL) URL.revokeObjectURL(j.objectURL);
    const i = jobs.indexOf(j);
    if (i >= 0) jobs.splice(i, 1);
    msg("queue-msg", "");
    refresh();
  }

  function updateRow(j) {
    if (!j.nodes) return;
    const n = j.nodes;
    n.title.textContent = j.label;
    n.sub.textContent = desc(j) + " · " + j.note;
    n.sub.className = "row-sub" + (j.state === "error" ? " row-error" : "");
    n.progress.hidden = j.percent === undefined;
    if (j.percent !== undefined) n.progress.value = j.percent;
  }

  function refresh() {
    el("count").textContent = String(jobs.length);
    el("empty").hidden = !!jobs.length;
    const retry = jobs.some(j => j.state === "error"),
      todo = jobs.some(j => j.state === "queued" || j.state === "error");
    el("run").disabled = active || zipping || !todo || closed;
    el("run").hidden = active;
    el("run").textContent = retry && !jobs.some(j => j.state === "queued") ? "Retry failed" :
      "Render queue";
    el("stop").hidden = !active;
    el("quality").disabled = active || closed;
    el("bundle").disabled = active || zipping || closed || !jobs.some(j => j.state === "ready");
    el("quit").disabled = active || zipping || closed;
    const list = el("jobs");
    list.textContent = "";
    jobs.forEach((j, i) => {
      const row = element("li", "job"),
        num = element("span", "num", String(i + 1).padStart(2, "0"));
      const details = element("div"),
        title = element("div", "job-title"),
        sub = element("div", "row-sub"),
        progress = element("progress");
      progress.max = 100;
      progress.setAttribute("aria-label", "Encoding progress");
      details.append(title, sub);
      if (j.ref) {
        const source = element("a", "source-link", "Open in " + j.ref.provider + " ↗");
        source.href = j.ref.url;
        source.target = "_blank";
        source.rel = "noreferrer";
        details.append(source);
      }
      details.append(progress);
      const actions = element("div", "job-actions");
      if (j.state === "ready") {
        const listen = button(j.listening ? "Hide player" : "Listen", "text-btn", () => {
          j.listening = !j.listening;
          refresh();
        });
        const save = button("Save MP3", "secondary", () => Transfer.save(j.objectURL, j.outName));
        actions.append(listen, save);
      }
      const del = button("Remove", "text-btn", () => remove(j));
      del.disabled = active || zipping;
      del.setAttribute("aria-label", "Remove " + j.label);
      actions.append(del);
      row.append(num, details, actions);
      if (j.state === "ready" && j.listening) {
        const wrap = element("div", "play-row"),
          player = document.createElement("audio");
        player.controls = true;
        player.src = j.objectURL;
        player.setAttribute("aria-label", j.label);
        wrap.append(player);
        row.append(wrap);
      }
      list.append(row);
      j.nodes = {
        title,
        sub,
        progress
      };
      updateRow(j);
    });
  }

  function step(j, n, p) {
    j.note = n;
    j.percent = p;
    updateRow(j);
  }
  async function render(j, bitrate, signal) {
    step(j, j.file ? "Reading file…" : "Downloading full audio…");
    let buffer = await Transfer.loadSource(j, signal);
    checkSignal(signal);
    const info = AudioFiles.inspect(new Uint8Array(buffer), basename(j)),
      tags = {
        ...(info.tags || {})
      };
    for (const key of ["title", "artist", "album"]) {
      if (j.ref?.[key]) tags[key] = j.ref[key];
    }
    if (!tags.title) tags.title = trimExt(basename(j));
    j.label = tags.title;
    let seconds, blob, note;
    if (info.isMp3) {
      seconds = info.duration;
      blob = j.file || new Blob([buffer], {
        type: "audio/mpeg"
      });
      if (blob.type !== "audio/mpeg") blob = blob.slice(0, blob.size, "audio/mpeg");
      note = "Original MP3 copied";
    } else {
      step(j, "Decoding audio…");
      const audio = await AudioFiles.decode(buffer, signal);
      buffer = null;
      checkSignal(signal);
      seconds = audio.duration;
      step(j, "Encoding MP3…", 0);
      const chunks = await Transfer.encode(audio, bitrate, signal, n => step(j, "Encoding MP3 · " +
        n + "%", n));
      blob = new Blob([AudioFiles.tagBytes(tags), ...chunks], {
        type: "audio/mpeg"
      });
      note = "MP3 · " + bitrate + " kbps";
    }
    checkSignal(signal);
    const used = new Set(jobs.filter(other => other !== j && other.outName).map(other => other
      .outName.toLocaleLowerCase("en-US")));
    j.outName = Transfer.unique((tags.artist ? tags.artist + " - " : "") + tags.title, used);
    j.blob = blob;
    j.objectURL = URL.createObjectURL(blob);
    j.label = j.outName;
    j.state = "ready";
    j.note = note + " · " + duration(seconds);
    if (j.ref?.duration && Math.abs(seconds - j.ref.duration) > Math.max(5, j.ref.duration * .05)) j
      .note += " · Duration differs from the catalog (" + duration(j.ref.duration) +
      "). Check the attached version.";
    j.percent = undefined;
  }
  el("run").onclick = async () => {
    if (active || closed) return;
    active = true;
    controller = new AbortController();
    const signal = controller.signal,
      bitrate = Number(el("quality").value);
    const batch = jobs.filter(j => j.state === "queued" || j.state === "error");
    msg("queue-msg", "");
    el("stop").disabled = false;
    refresh();
    try {
      for (const j of batch) {
        if (signal.aborted) break;
        j.state = "working";
        try {
          await render(j, bitrate, signal);
        } catch (err) {
          j.percent = undefined;
          j.state = signal.aborted ? "queued" : "error";
          j.note = signal.aborted ? "Stopped · Ready to retry" : (err.message ||
            "The export failed.");
        }
        refresh();
      }
    } finally {
      const stopped = signal.aborted;
      active = false;
      controller = null;
      refresh();
      const ready = jobs.filter(j => j.state === "ready").length;
      msg("queue-msg", (stopped ? "Stopped. " : "") + ready + " of " + jobs.length +
        " ready to save.");
    }
  };
  el("stop").onclick = () => {
    controller?.abort();
    el("stop").disabled = true;
    msg("queue-msg", "Stopping the current track…");
  };
  el("bundle").onclick = async () => {
    if (active || zipping) return;
    const ready = jobs.filter(j => j.state === "ready");
    if (!ready.length) return;
    zipping = true;
    refresh();
    msg("queue-msg", "Preparing " + ready.length + " MP3" + (ready.length === 1 ? "" : "s") +
      " for transfer…");
    try {
      const b = await Transfer.zip(ready),
        url = URL.createObjectURL(b);
      Transfer.save(url, "Trackport-MP3s.zip");
      setTimeout(() => URL.revokeObjectURL(url), 90000);
      msg("queue-msg", ready.length + " MP3" + (ready.length === 1 ? "" : "s") +
        " included in the ZIP.");
    } catch (e) {
      msg("queue-msg", e.message);
    } finally {
      zipping = false;
      refresh();
    }
  };
  el("help").onclick = () => el("guide").showModal();
  el("close-help").onclick = () => el("guide").close();
  el("quit").onclick = async () => {
    if (jobs.length && !confirm("End this session and discard the queue? Save your MP3s first."))
      return;
    try {
      await Transfer.webFetch("/api/quit", "");
      closed = true;
      lookupTask?.abort();
      document.querySelectorAll("main button,main input,main select").forEach(n => n.disabled =
        true);
      msg("queue-msg", "Session ended. You can close this tab.");
    } catch (e) {
      msg("queue-msg", e.message);
    }
  };
  window.addEventListener("beforeunload", e => {
    if (active || zipping) {
      e.preventDefault();
      e.returnValue = "";
    }
  });
  refresh();
})();
