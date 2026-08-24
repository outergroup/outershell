(() => {
  "use strict";

  document.documentElement.classList.toggle("outerloop-host", /(^|\.)outerlooplocal$/i.test(window.location.hostname));

  const decoder = new TextDecoder();
  const elements = {
    shell: document.querySelector("#app"),
    sections: document.querySelector("#app-sections"),
    safeSpaces: document.querySelector("#safe-spaces"),
    safeSpacesEmpty: document.querySelector("#safe-spaces-empty"),
    empty: document.querySelector("#empty-state"),
    summary: document.querySelector("#app-summary"),
    status: document.querySelector("#status-banner"),
    search: document.querySelector("#search-input"),
    add: document.querySelector("#add-button"),
    refresh: document.querySelector("#refresh-button"),
    dialogLayer: document.querySelector("#dialog-layer"),
    dialogTemplate: document.querySelector("#dialog-template"),
    toasts: document.querySelector("#toast-region")
  };

  const state = {
    backends: [],
    safeSpaces: [],
    loading: true,
    safeSpacesLoading: true,
    safeSpacesRefreshing: false,
    backendError: "",
    safeSpacesError: "",
    safeSpaceBusy: new Set(),
    safeSpacesTimer: null,
    busy: false,
    query: "",
    addTab: "catalog",
    backendsVersion: 0n,
    logVersion: 0n,
    logSelection: null,
    eventAbort: null,
    stopped: false,
    longPress: null,
    suppressLaunchUntil: 0,
    pageScrollY: null,
    pressFeedback: null,
    appDrag: null
  };

  class PayloadReader {
    constructor(value) {
      this.bytes = value instanceof Uint8Array ? value : new Uint8Array(value);
      this.view = new DataView(this.bytes.buffer, this.bytes.byteOffset, this.bytes.byteLength);
    }

    require(offset, length) {
      if (offset < 0 || length < 0 || offset + length > this.bytes.byteLength) {
        throw new Error("Outer Shell returned an invalid binary payload.");
      }
    }

    u32(offset) {
      this.require(offset, 4);
      return this.view.getUint32(offset, true);
    }

    u64(offset) {
      this.require(offset, 8);
      if (typeof this.view.getBigUint64 === "function") return this.view.getBigUint64(offset, true);
      return BigInt(this.u32(offset)) | (BigInt(this.u32(offset + 4)) << 32n);
    }

    f64(offset) {
      this.require(offset, 8);
      return this.view.getFloat64(offset, true);
    }

    bytesRef(offset) {
      const start = this.u32(offset);
      const length = this.u32(offset + 4);
      this.require(start, length);
      return this.bytes.subarray(start, start + length);
    }

    stringRef(offset) {
      const bytes = this.bytesRef(offset);
      return bytes.length ? decoder.decode(bytes) : "";
    }

    child(offset) {
      return new PayloadReader(this.bytesRef(offset));
    }

    payloadArray() {
      const count = this.u32(0);
      const result = [];
      for (let index = 0; index < count; index += 1) result.push(this.child(4 + index * 8));
      return result;
    }
  }

  function decodeBackends(buffer) {
    const reader = new PayloadReader(buffer);
    const count = reader.u32(8);
    const backends = [];
    for (let index = 0; index < count; index += 1) {
      backends.push(decodeBackend(reader.child(12 + index * 8)));
    }
    return { error: reader.stringRef(0), backends };
  }

  function decodeBackend(reader) {
    const flags = reader.u32(64);
    const value = {
      serviceID: reader.stringRef(0),
      displayName: reader.stringRef(8),
      serviceUnit: reader.stringRef(16),
      serviceUnitPath: reader.stringRef(24),
      serviceScope: reader.stringRef(32),
      status: reader.stringRef(40),
      iconSymbolName: reader.stringRef(48),
      launchdPlistPath: reader.stringRef(56),
      canControl: Boolean(flags & 0x01),
      canUninstall: Boolean(flags & 0x02),
      isBundled: Boolean(flags & 0x04),
      isInstalled: Boolean(flags & 0x08),
      isMigration: Boolean(flags & 0x10),
      ownsLaunchdPlist: Boolean(flags & 0x20),
      supportsRoot: Boolean(flags & 0x40),
      rootOnly: Boolean(flags & 0x80),
      hasRootSupport: Boolean(flags & 0x100),
      menuBarVisibilityEnabled: Boolean(flags & 0x200),
      menuBarVisibilityAvailable: Boolean(flags & 0x400),
      frontends: reader.child(68).payloadArray().map(decodeFrontend),
      logFiles: reader.child(76).payloadArray().map(decodeLogFile),
      installedVersion: "",
      availableVersion: "",
      scriptPath: "",
      publicBaseURL: ""
    };
    if (reader.bytes.length >= 92) value.installedVersion = reader.stringRef(84);
    if (reader.bytes.length >= 100) value.availableVersion = reader.stringRef(92);
    if (reader.bytes.length >= 108) value.scriptPath = reader.stringRef(100);
    if (reader.bytes.length >= 116) value.publicBaseURL = reader.stringRef(108);
    return value;
  }

  function decodeFrontend(reader) {
    const flags = reader.bytes.length >= 64 ? reader.u32(60) : 1;
    return {
      name: reader.stringRef(0),
      url: reader.stringRef(8),
      socketPath: reader.stringRef(16),
      iconPath: reader.stringRef(24),
      iconData: reader.bytesRef(32),
      list: reader.stringRef(40),
      port: reader.u32(48),
      id: reader.bytes.length >= 60 ? reader.stringRef(52) : "",
      iconObservationToken: reader.bytes.length >= 72 ? reader.stringRef(64) : "",
      isRunning: Boolean(flags & 0x01)
    };
  }

  function decodeLogFile(reader) {
    return {
      identifier: reader.stringRef(0),
      displayName: reader.stringRef(8),
      path: reader.stringRef(16),
      size: Number(reader.u64(24)),
      modified: reader.f64(32),
      readable: Boolean(reader.u32(40) & 0x01)
    };
  }

  function decodeAction(buffer) {
    const reader = new PayloadReader(buffer);
    const flags = reader.u32(0);
    return {
      ok: Boolean(flags & 0x01),
      needsPassword: Boolean(flags & 0x02),
      updateAvailable: Boolean(flags & 0x04),
      message: reader.stringRef(4),
      installedVersion: reader.bytes.length >= 20 ? reader.stringRef(12) : "",
      availableVersion: reader.bytes.length >= 28 ? reader.stringRef(20) : ""
    };
  }

  function decodeLog(buffer) {
    const reader = new PayloadReader(buffer);
    return {
      serviceID: reader.stringRef(0),
      path: reader.stringRef(8),
      contents: reader.stringRef(16),
      truncated: Boolean(reader.u32(24) & 0x01),
      fileSize: Number(reader.u64(28)),
      modified: reader.f64(36),
      error: reader.stringRef(44)
    };
  }

  function decodeEvents(buffer) {
    const reader = new PayloadReader(buffer);
    const flags = reader.u32(0);
    return {
      backendsChanged: Boolean(flags & 0x01),
      logChanged: Boolean(flags & 0x02),
      timedOut: Boolean(flags & 0x04),
      backendsVersion: reader.u64(8),
      logVersion: reader.u64(16)
    };
  }

  const escapeHTML = value => String(value ?? "").replace(/[&<>'"]/g, character => ({
    "&": "&amp;", "<": "&lt;", ">": "&gt;", "'": "&#39;", '"': "&quot;"
  })[character]);

  function backendKey(backend) {
    return [backend.serviceID, backend.serviceScope, backend.serviceUnitPath || backend.serviceUnit].join("\u001f");
  }

  function findBackend(key) {
    return state.backends.find(backend => backendKey(backend) === key);
  }

  function initials(name) {
    const parts = String(name || "App").trim().split(/\s+/).filter(Boolean);
    return parts.slice(0, 2).map(part => part[0]).join("").toUpperCase() || "A";
  }

  function dataURL(bytes) {
    if (!bytes || !bytes.length) return "";
    let binary = "";
    for (let offset = 0; offset < bytes.length; offset += 0x8000) {
      binary += String.fromCharCode(...bytes.subarray(offset, offset + 0x8000));
    }
    return `data:image/png;base64,${btoa(binary)}`;
  }

  function iconHTML(item, extraClass = "") {
    const source = dataURL(item.frontend?.iconData);
    const content = source
      ? `<img src="${source}" alt="">`
      : `<span>${escapeHTML(initials(item.displayName || item.backend.displayName))}</span>`;
    return `<span class="app-icon ${extraClass}" aria-hidden="true">${content}</span>`;
  }

  function launcherIconHTML(item) {
    const source = dataURL(item.frontend?.iconData);
    const content = source
      ? `<img src="${source}" alt="">`
      : `<span class="launcher-fallback" aria-hidden="true">${"<i></i>".repeat(6)}</span>`;
    return `<span class="launcher-icon" aria-hidden="true">${content}</span>`;
  }

  function listIconHTML(item) {
    const source = dataURL(item.frontend?.iconData);
    const content = source
      ? `<img src="${source}" alt="">`
      : `<span>${escapeHTML(initials(item.displayName).slice(0, 1))}</span>`;
    return `<span class="list-icon${source ? " has-image" : ""}" aria-hidden="true">${content}</span>`;
  }

  function safeSpaceAppIconHTML(app) {
    const source = app.iconData ? `data:image/png;base64,${app.iconData}` : "";
    const content = source
      ? `<img src="${source}" alt="">`
      : `<span>${escapeHTML(initials(app.displayName || app.serviceID).slice(0, 1))}</span>`;
    return `<span class="safe-space-app-icon${source ? " has-image" : ""}" aria-hidden="true">${content}</span>`;
  }

  function runningBadgesHTML(item) {
    const badges = [];
    if (item.user && endpointRunning(item.user)) badges.push(`<a class="running-badge-button" href="${escapeHTML(navigationURL(item.user.frontend))}" data-action="launch-endpoint" data-app-key="${escapeHTML(item.identity)}" data-app-scope="user" aria-label="Open ${escapeHTML(item.displayName)} as you" title="Open as you"><span class="running-badge user-running-badge" aria-hidden="true"></span></a>`);
    if (item.root && endpointRunning(item.root)) badges.push(`<a class="running-badge-button" href="${escapeHTML(navigationURL(item.root.frontend))}" data-action="launch-endpoint" data-app-key="${escapeHTML(item.identity)}" data-app-scope="root" aria-label="Open ${escapeHTML(item.displayName)} as root" title="Open as root"><span class="running-badge root-running-badge" aria-hidden="true"><svg viewBox="0 0 20 22"><path d="M10 1.15 17.25 3.9v5.55c0 4.9-2.85 8.62-7.25 11.4-4.4-2.78-7.25-6.5-7.25-11.4V3.9L10 1.15Z"/><path class="root-running-check" d="m6.15 10.8 2.5 2.55 5.25-5.6"/></svg></span></a>`);
    return badges.length ? `<span class="running-badges">${badges.join("")}</span>` : "";
  }

  function normalizedPath(value) {
    const trimmed = String(value || "").trim();
    if (!trimmed) return "/";
    if (trimmed.startsWith("/")) return trimmed;
    if (trimmed.startsWith("?")) return `/${trimmed}`;
    return `/${trimmed}`;
  }

  function pathAndQuery(frontend) {
    const raw = String(frontend.url || "").trim();
    const socket = String(frontend.socketPath || "").trim();
    if (socket && raw === socket) return "/";
    if (socket && raw.startsWith(socket)) return normalizedPath(raw.slice(socket.length));
    if (raw.toLowerCase().startsWith("http+unix://")) {
      const rest = raw.slice("http+unix://".length);
      const index = rest.search(/[/?#]/);
      return index < 0 ? "/" : normalizedPath(rest.slice(index));
    }
    try {
      const parsed = new URL(raw);
      return `${parsed.pathname || "/"}${parsed.search}`;
    } catch (_) {
      const slash = raw.indexOf("/");
      return slash < 0 ? "/" : normalizedPath(raw.slice(slash));
    }
  }

  function navigationURL(frontend) {
    const socket = String(frontend.socketPath || "").trim();
    let target;
    if (socket) target = `http+unix://${encodeURIComponent(socket)}${pathAndQuery(frontend)}`;
    else if (frontend.port > 0) target = `http://127.0.0.1:${frontend.port}${pathAndQuery(frontend)}`;
    else target = String(frontend.url || "").trim() || "#";

    const token = String(frontend.iconObservationToken || "");
    if (window.outerLoopPageIconObservationSupported === true && !frontend.iconData?.length && token) {
      const callback = new URL("/api/icon-observation", window.location.href);
      callback.search = new URLSearchParams({ token }).toString();
      const observation = new URL("outerloop://observe-page-icon");
      observation.search = new URLSearchParams({ url: target, callback: callback.href }).toString();
      return observation.href;
    }
    return target;
  }

  function endpointRunning(endpoint) {
    const hasEndpoint = Boolean(endpoint.frontend.socketPath || endpoint.frontend.port > 0 || /^[a-z][a-z0-9+.-]*:/i.test(endpoint.frontend.url || ""));
    return hasEndpoint ? endpoint.frontend.isRunning : endpoint.frontend.isRunning || endpoint.backend.status === "running";
  }

  function endpointReady(endpoint) {
    const frontend = endpoint.frontend;
    const hasEndpoint = Boolean(frontend.socketPath || frontend.port > 0 || /^[a-z][a-z0-9+.-]*:/i.test(frontend.url || ""));
    return endpointRunning(endpoint) || (["available", "awaiting"].includes(endpoint.backend.status) && hasEndpoint);
  }

  function launcherItems() {
    const endpoints = state.backends.flatMap(backend => {
      if (backend.serviceID === "org.outershell.OuterShell" || !backend.isInstalled) return [];
      return backend.frontends.map((frontend, index) => ({ backend, frontend, index }));
    });
    const groups = new Map();
    endpoints.forEach(endpoint => {
      const name = endpoint.frontend.name.trim() || endpoint.backend.displayName.trim() || "App";
      const identity = [endpoint.backend.serviceID, name, endpoint.frontend.id || pathAndQuery(endpoint.frontend)].join("\u001f");
      if (!groups.has(identity)) groups.set(identity, []);
      groups.get(identity).push(endpoint);
    });
    return [...groups.entries()].map(([identity, group]) => {
      group.sort((left, right) => left.backend.serviceScope === "system" ? 1 : right.backend.serviceScope === "system" ? -1 : 0);
      const user = group.find(endpoint => endpoint.backend.serviceScope !== "system");
      const root = group.find(endpoint => endpoint.backend.serviceScope === "system");
      const primary = group.find(endpoint => endpointRunning(endpoint)) || group.find(endpoint => endpointReady(endpoint)) || user || root || group[0];
      return {
        identity,
        endpoints: group,
        user,
        root,
        primary,
        backend: primary.backend,
        frontend: primary.frontend,
        displayName: primary.frontend.name.trim() || primary.backend.displayName.trim() || "App",
        subtitle: primary.backend.displayName.trim() || primary.backend.serviceID
      };
    }).sort((left, right) => left.displayName.localeCompare(right.displayName, undefined, { sensitivity: "base" }));
  }

  function render() {
    elements.shell.setAttribute("aria-busy", state.loading ? "true" : "false");
    if (state.loading && !state.backends.length) {
      elements.sections.innerHTML = `<div class="loading-grid">${"<div class=\"skeleton\"></div>".repeat(6)}</div>`;
      elements.empty.hidden = true;
      renderSafeSpaces();
      return;
    }
    const items = launcherItems();
    const query = state.query.trim().toLocaleLowerCase();
    const visible = query ? items.filter(item => `${item.displayName} ${item.subtitle} ${item.backend.serviceID}`.toLocaleLowerCase().includes(query)) : items;
    elements.summary.textContent = items.length === 1 ? "1 app is ready to open." : `${items.length} apps are ready to open.`;
    elements.empty.hidden = visible.length > 0 || state.loading;
    if (!visible.length) {
      elements.sections.innerHTML = "";
      if (query) {
        elements.empty.querySelector("h2").textContent = "No matching apps";
        elements.empty.querySelector("p").textContent = "Try a different name or identifier.";
        elements.empty.querySelector("button").hidden = true;
      } else {
        elements.empty.querySelector("h2").textContent = "No apps here yet";
        elements.empty.querySelector("p").textContent = "Install a bundled app or turn a Bash command into one.";
        elements.empty.querySelector("button").hidden = false;
      }
      renderSafeSpaces();
      return;
    }

    const iconItems = visible.filter(item => !item.frontend.list?.trim());
    const listSections = new Map();
    visible.filter(item => item.frontend.list?.trim()).forEach(item => {
      const name = item.frontend.list.trim();
      if (!listSections.has(name)) listSections.set(name, []);
      listSections.get(name).push(item);
    });
    const orderedLists = [...listSections.entries()].sort(([left], [right]) => left.localeCompare(right, undefined, { sensitivity: "base" }));
    const addTile = query ? "" : `<article class="launcher-tile add-app-tile">
      <button class="launcher-link" type="button" data-action="open-add" aria-label="Add app"><span class="add-app-icon" aria-hidden="true"><span></span></span></button>
      <span class="launcher-name">Add app</span>
    </article>`;
    elements.sections.classList.toggle("single-column", orderedLists.length === 0);
    elements.sections.innerHTML = `
      <section class="launcher-column" data-drop-list="" aria-label="Apps">
        <div class="launcher-grid">${iconItems.map(renderLauncherTile).join("")}${addTile}</div>
      </section>
      ${orderedLists.length ? `<section class="list-column" aria-label="App lists">${orderedLists.map(renderListGroup).join("")}</section>` : ""}`;
    renderSafeSpaces();
  }

  function safeSpaceState(workspace) {
    return String(workspace.state || "stopped").toLocaleLowerCase();
  }

  function safeSpaceIsRunning(workspace) {
    return safeSpaceState(workspace) === "running";
  }

  function safeSpaceRuntimeDescription(workspace) {
    const parts = [workspace.runtime?.providerName || workspace.runtime?.isolationName];
    const system = [workspace.runtime?.operatingSystemName, workspace.runtime?.operatingSystemVersion]
      .filter(Boolean).join(" ");
    if (system) parts.push(system);
    if (workspace.runtime?.architecture) parts.push(workspace.runtime.architecture);
    return parts.filter(Boolean).join(" · ") || "Container";
  }

  function safeSpacePathAndQuery(app) {
    return pathAndQuery({ url: app.url || "", socketPath: app.socketPath || "" });
  }

  function safeSpaceAppURL(app) {
    const path = safeSpacePathAndQuery(app);
    if (Number(app.publishedPort) > 0) return `http://127.0.0.1:${Number(app.publishedPort)}${path}`;
    const socket = String(app.externalSocketPath || "").trim();
    return socket ? `http+unix://${encodeURIComponent(socket)}${path}` : "#";
  }

  function safeSpaceAppNavigationURL(workspace, app) {
    const target = safeSpaceAppURL(app);
    if (target === "#" || window.outerLoopFriendlyNavigationSupported !== true) return target;
    const name = app.displayName || app.serviceID || "App";
    const user = String(app.socketPath || "").startsWith("/run/user/0/") ? "root" : "workspace";
    const parameters = [
      `url=${encodeURIComponent(target)}`,
      `display=${encodeURIComponent(`${workspace.name || "Container"} / ${user} / ${name}`)}`
    ].join("&");
    return `outerloop://navigate?${parameters}`;
  }

  function safeSpaceAppIsReady(workspace, app) {
    return safeSpaceIsRunning(workspace) && app.isRunning === true && safeSpaceAppURL(app) !== "#";
  }

  function renderSafeSpaces() {
    if (state.safeSpacesLoading && !state.safeSpaces.length) {
      elements.safeSpaces.innerHTML = `${"<div class=\"safe-space-skeleton\"></div>".repeat(2)}`;
      elements.safeSpacesEmpty.hidden = true;
      return;
    }
    elements.safeSpacesEmpty.hidden = state.safeSpaces.length > 0 || state.safeSpacesLoading;
    elements.safeSpaces.innerHTML = state.safeSpaces.map(renderSafeSpace).join("");
  }

  function renderSafeSpace(workspace) {
    const running = safeSpaceIsRunning(workspace);
    const busy = state.safeSpaceBusy.has(workspace.id);
    const apps = Array.isArray(workspace.apps) ? workspace.apps : [];
    const appRows = apps.length
      ? apps.map(app => renderSafeSpaceApp(workspace, app)).join("")
      : `<p class="safe-space-no-apps">${running ? "No apps are installed in this container." : "Start this container to inspect its apps."}</p>`;
    return `<article class="safe-space-card${running ? " is-running" : ""}" data-safe-space-id="${escapeHTML(workspace.id)}">
      <header class="safe-space-header">
        <span class="safe-space-state" aria-label="${running ? "Running" : "Stopped"}"></span>
        <div class="safe-space-title-wrap">
          <h3>${escapeHTML(workspace.name || "Container")}</h3>
          <p>${escapeHTML(safeSpaceRuntimeDescription(workspace))}</p>
        </div>
        ${running ? "" : `<button class="safe-space-start secondary-button" type="button" data-action="start-safe-space" data-safe-space-id="${escapeHTML(workspace.id)}" ${busy ? "disabled" : ""}>${busy ? "Starting…" : "Start"}</button>`}
      </header>
      <div class="safe-space-apps">${appRows}</div>
    </article>`;
  }

  function renderSafeSpaceApp(workspace, app) {
    const ready = safeSpaceAppIsReady(workspace, app);
    const key = `${workspace.id}\u001f${app.serviceID}`;
    const busy = state.safeSpaceBusy.has(key);
    const name = app.displayName || app.serviceID || "App";
    return `<a class="safe-space-app${app.isRunning ? " is-running" : ""}${busy ? " is-busy" : ""}"
      href="${escapeHTML(ready ? safeSpaceAppNavigationURL(workspace, app) : "#")}" data-action="launch-safe-space-app"
      data-safe-space-id="${escapeHTML(workspace.id)}" data-service-id="${escapeHTML(app.serviceID)}"
      aria-label="Open ${escapeHTML(name)} in ${escapeHTML(workspace.name || "container")}">
      ${safeSpaceAppIconHTML(app)}
      <span class="safe-space-app-name">${escapeHTML(name)}</span>
      ${app.isRunning ? `<span class="safe-space-running-badge" aria-label="Running"></span>` : ""}
      ${busy ? `<span class="safe-space-app-progress" aria-hidden="true"></span>` : ""}
    </a>`;
  }

  function slug(value) {
    return String(value).toLocaleLowerCase().replace(/[^a-z0-9]+/g, "-").replace(/^-|-$/g, "") || "apps";
  }

  function renderLauncherTile(item) {
    const readyURL = endpointReady(item.primary) ? navigationURL(item.frontend) : "#";
    return `<article class="launcher-tile" data-drag-app-key="${escapeHTML(item.identity)}">
      <span class="launcher-icon-row"><a class="launcher-link" href="${escapeHTML(readyURL)}" data-action="launch" data-app-key="${escapeHTML(item.identity)}" aria-label="Open ${escapeHTML(item.displayName)}" aria-keyshortcuts="Shift+F10">${launcherIconHTML(item)}</a>${runningBadgesHTML(item)}</span>
      <h2 class="launcher-name">${escapeHTML(item.displayName)}</h2>
    </article>`;
  }

  function renderListGroup([name, items]) {
    return `<section class="list-group" aria-labelledby="list-${slug(name)}">
      <div class="list-widget" data-drop-list="${escapeHTML(name)}">${items.map(renderListRow).join("")}</div>
      <h2 id="list-${slug(name)}" class="list-label">${escapeHTML(name)}</h2>
    </section>`;
  }

  function renderListRow(item) {
    const readyURL = endpointReady(item.primary) ? navigationURL(item.frontend) : "#";
    const badges = runningBadgesHTML(item);
    return `<article class="list-row ${badges ? "has-running-badges" : ""}" data-drag-app-key="${escapeHTML(item.identity)}">
      <a class="list-link" href="${escapeHTML(readyURL)}" data-action="launch" data-app-key="${escapeHTML(item.identity)}" aria-label="Open ${escapeHTML(item.displayName)}" aria-keyshortcuts="Shift+F10">${listIconHTML(item)}<h3 class="list-name">${escapeHTML(item.displayName)}</h3></a>
      ${badges}
    </article>`;
  }

  async function requestBuffer(url, options = {}, acceptErrors = false) {
    const response = await fetch(url, { cache: "no-store", ...options });
    const buffer = await response.arrayBuffer();
    if (!response.ok && !acceptErrors) {
      const text = decoder.decode(buffer).trim();
      throw new Error(text || `Outer Shell returned HTTP ${response.status}.`);
    }
    return { response, buffer };
  }

  function makeRequestID() {
    if (typeof globalThis.crypto?.randomUUID === "function") {
      return globalThis.crypto.randomUUID();
    }

    const bytes = new Uint8Array(16);
    if (typeof globalThis.crypto?.getRandomValues === "function") {
      globalThis.crypto.getRandomValues(bytes);
    } else {
      for (let index = 0; index < bytes.length; index += 1) {
        bytes[index] = Math.floor(Math.random() * 256);
      }
    }
    bytes[6] = (bytes[6] & 0x0f) | 0x40;
    bytes[8] = (bytes[8] & 0x3f) | 0x80;
    const hex = Array.from(bytes, value => value.toString(16).padStart(2, "0"));
    return `${hex[0]}${hex[1]}${hex[2]}${hex[3]}-${hex[4]}${hex[5]}-${hex[6]}${hex[7]}-${hex[8]}${hex[9]}-${hex[10]}${hex[11]}${hex[12]}${hex[13]}${hex[14]}${hex[15]}`;
  }

  async function safeSpaceRequest(operation, values = {}) {
    const requestID = makeRequestID();
    const response = await fetch("/api/safe-spaces", {
      method: "POST",
      cache: "no-store",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ requestID, operation, ...values })
    });
    const result = await response.json();
    if (!response.ok || result.error) throw new Error(result.error || `Outer Shell returned HTTP ${response.status}.`);
    return result;
  }

  async function refreshSafeSpaces({ quiet = false } = {}) {
    if (state.safeSpacesRefreshing) return;
    state.safeSpacesRefreshing = true;
    if (!quiet) {
      state.safeSpacesLoading = true;
      renderSafeSpaces();
    }
    try {
      const result = await safeSpaceRequest("list");
      state.safeSpaces = Array.isArray(result.workspaces) ? result.workspaces : [];
      state.safeSpacesError = "";
    } catch (error) {
      state.safeSpacesError = error.message || String(error);
      if (!quiet) toast(state.safeSpacesError, true);
    } finally {
      state.safeSpacesRefreshing = false;
      state.safeSpacesLoading = false;
      updateStatus();
      renderSafeSpaces();
    }
  }

  function findSafeSpace(id) {
    return state.safeSpaces.find(workspace => workspace.id === id);
  }

  async function waitForSafeSpaceApp(workspaceID, serviceID) {
    for (let attempt = 0; attempt < 40; attempt += 1) {
      await refreshSafeSpaces({ quiet: true });
      const workspace = findSafeSpace(workspaceID);
      const app = workspace?.apps?.find(candidate => candidate.serviceID === serviceID);
      if (workspace && app && safeSpaceAppIsReady(workspace, app)) return { workspace, app };
      await delay(250);
    }
    throw new Error("The app started, but its address is not ready yet.");
  }

  async function startSafeSpace(workspaceID) {
    state.safeSpaceBusy.add(workspaceID);
    renderSafeSpaces();
    try {
      const result = await safeSpaceRequest("start", { workspaceID });
      state.safeSpaces = Array.isArray(result.workspaces) ? result.workspaces : state.safeSpaces;
    } finally {
      state.safeSpaceBusy.delete(workspaceID);
      renderSafeSpaces();
    }
  }

  async function launchSafeSpaceApp(workspaceID, serviceID) {
    let workspace = findSafeSpace(workspaceID);
    let app = workspace?.apps?.find(candidate => candidate.serviceID === serviceID);
    if (!workspace || !app) throw new Error("The selected container app no longer exists.");
    if (safeSpaceAppIsReady(workspace, app)) {
      window.location.assign(safeSpaceAppNavigationURL(workspace, app));
      return;
    }
    const key = `${workspaceID}\u001f${serviceID}`;
    state.safeSpaceBusy.add(key);
    renderSafeSpaces();
    try {
      if (!safeSpaceIsRunning(workspace)) {
        const result = await safeSpaceRequest("start", { workspaceID });
        state.safeSpaces = Array.isArray(result.workspaces) ? result.workspaces : state.safeSpaces;
        workspace = findSafeSpace(workspaceID) || workspace;
        app = workspace.apps?.find(candidate => candidate.serviceID === serviceID) || app;
      }
      if (app.isRunning !== true) await safeSpaceRequest("startApp", { workspaceID, serviceID });
      const ready = await waitForSafeSpaceApp(workspaceID, serviceID);
      window.location.assign(safeSpaceAppNavigationURL(ready.workspace, ready.app));
    } finally {
      state.safeSpaceBusy.delete(key);
      renderSafeSpaces();
    }
  }

  async function refreshBackends({ quiet = false } = {}) {
    if (!quiet) {
      state.loading = true;
      render();
    }
    try {
      const { buffer } = await requestBuffer("/api/backends");
      const result = decodeBackends(buffer);
      state.backends = result.backends;
      state.backendError = result.error;
      updateStatus();
    } catch (error) {
      state.backendError = error.message || String(error);
      updateStatus();
      if (!quiet) toast(error.message || String(error), true);
    } finally {
      state.loading = false;
      render();
    }
  }

  function showStatus(message = "") {
    elements.status.textContent = message;
    elements.status.hidden = !message;
  }

  function updateStatus() {
    showStatus([state.backendError, state.safeSpacesError].filter(Boolean).join("\n"));
  }

  function toast(message, isError = false) {
    if (!message) return;
    const node = document.createElement("div");
    node.className = `toast${isError ? " error" : ""}`;
    node.textContent = message;
    elements.toasts.append(node);
    window.setTimeout(() => node.remove(), 4200);
  }

  function findItem(identity) {
    return launcherItems().find(item => item.identity === identity);
  }

  function itemIdentityFromTarget(target) {
    if (!(target instanceof Element)) return "";
    return target.closest("[data-app-key]")?.dataset.appKey || "";
  }

  function cancelLongPress() {
    if (!state.longPress) return;
    window.clearTimeout(state.longPress.timer);
    state.longPress = null;
  }

  function clearTextSelection() {
    window.getSelection()?.removeAllRanges();
  }

  function beginLongPress(event) {
    if (event.pointerType !== "touch" || !event.isPrimary || elements.dialogLayer.childElementCount) return;
    const identity = itemIdentityFromTarget(event.target);
    if (!identity) return;
    cancelLongPress();
    const press = {
      identity,
      pointerID: event.pointerId,
      x: event.clientX,
      y: event.clientY,
      timer: 0
    };
    press.timer = window.setTimeout(() => {
      if (state.longPress !== press) return;
      state.longPress = null;
      cancelAppDrag();
      clearPressFeedback();
      state.suppressLaunchUntil = Date.now() + 900;
      navigator.vibrate?.(8);
      openAppMenu(identity);
      clearTextSelection();
      window.requestAnimationFrame(clearTextSelection);
    }, 520);
    state.longPress = press;
  }

  function moveLongPress(event) {
    const press = state.longPress;
    if (!press || event.pointerId !== press.pointerID) return;
    if (Math.hypot(event.clientX - press.x, event.clientY - press.y) > 10) cancelLongPress();
  }

  function beginPressFeedback(event) {
    if (!event.isPrimary || event.button !== 0 || event.ctrlKey || !(event.target instanceof Element)) return;
    const target = event.target.closest(".launcher-link, .list-link, .running-badge-button");
    if (!target) return;
    clearPressFeedback();
    target.classList.add("pressed");
    state.pressFeedback = { target, pointerID: event.pointerId, x: event.clientX, y: event.clientY };
  }

  function movePressFeedback(event) {
    const feedback = state.pressFeedback;
    if (!feedback || event.pointerId !== feedback.pointerID) return;
    if (Math.hypot(event.clientX - feedback.x, event.clientY - feedback.y) > 10) clearPressFeedback();
  }

  function clearPressFeedback() {
    state.pressFeedback?.target.classList.remove("pressed");
    state.pressFeedback = null;
  }

  function beginAppDrag(event) {
    if (!event.isPrimary || event.button !== 0 || event.ctrlKey || elements.dialogLayer.childElementCount || !(event.target instanceof Element)) return;
    const source = event.target.closest(".launcher-link[data-app-key], .list-link[data-app-key]");
    if (!source || source.closest(".context-menu")) return;
    const item = findItem(source.dataset.appKey);
    if (!item) return;
    cancelAppDrag();
    const drag = {
      identity: item.identity,
      item,
      source,
      sourceContainer: source.closest("[data-drag-app-key]"),
      pointerID: event.pointerId,
      pointerType: event.pointerType,
      startX: event.clientX,
      startY: event.clientY,
      x: event.clientX,
      y: event.clientY,
      currentList: String(item.frontend.list || "").trim(),
      currentDropList: null,
      dropTarget: null,
      preview: null,
      isDragging: false,
      touchArmed: event.pointerType !== "touch",
      armTimer: 0,
      scrollVelocity: 0,
      scrollFrame: 0
    };
    if (event.pointerType === "touch") {
      drag.armTimer = window.setTimeout(() => {
        if (state.appDrag === drag) drag.touchArmed = true;
      }, 260);
    }
    state.appDrag = drag;
  }

  function startAppDrag(drag, event) {
    if (drag.isDragging) return;
    drag.isDragging = true;
    window.clearTimeout(drag.armTimer);
    drag.armTimer = 0;
    cancelLongPress();
    clearPressFeedback();
    clearTextSelection();
    state.suppressLaunchUntil = Date.now() + 900;
    drag.source.setAttribute("aria-grabbed", "true");
    drag.sourceContainer?.classList.add("app-drag-source");
    document.body.classList.add("app-dragging");
    const preview = document.createElement("div");
    preview.className = "app-drag-preview";
    preview.setAttribute("aria-hidden", "true");
    preview.innerHTML = `${launcherIconHTML(drag.item)}<span>${escapeHTML(drag.item.displayName)}</span>`;
    document.body.append(preview);
    drag.preview = preview;
    try { drag.source.setPointerCapture(event.pointerId); } catch (_) {}
    updateAppDrag(drag, event.clientX, event.clientY);
  }

  function dropTargetAt(x, y) {
    const node = document.elementFromPoint(x, y);
    const target = node instanceof Element ? node.closest("[data-drop-list]") : null;
    return target ? { element: target, list: target.dataset.dropList || "" } : null;
  }

  function updateAppDropTarget(drag) {
    const target = dropTargetAt(drag.x, drag.y);
    const changedTarget = target && target.list !== drag.currentList ? target : null;
    if (drag.dropTarget !== changedTarget?.element) {
      drag.dropTarget?.classList.remove("app-drop-target");
      changedTarget?.element.classList.add("app-drop-target");
      drag.dropTarget = changedTarget?.element || null;
    }
    drag.currentDropList = target?.list ?? null;
  }

  function stepAppDragScroll() {
    const drag = state.appDrag;
    if (!drag?.isDragging || !drag.scrollVelocity) return;
    window.scrollBy(0, drag.scrollVelocity);
    updateAppDropTarget(drag);
    drag.scrollFrame = window.requestAnimationFrame(stepAppDragScroll);
  }

  function updateAppDragScroll(drag) {
    const edge = Math.min(84, Math.max(54, window.innerHeight * 0.12));
    let velocity = 0;
    if (drag.y < edge) velocity = -Math.max(2, (edge - drag.y) * 0.12);
    else if (drag.y > window.innerHeight - edge) velocity = Math.max(2, (drag.y - (window.innerHeight - edge)) * 0.12);
    drag.scrollVelocity = Math.max(-13, Math.min(13, velocity));
    if (drag.scrollVelocity && !drag.scrollFrame) drag.scrollFrame = window.requestAnimationFrame(stepAppDragScroll);
    if (!drag.scrollVelocity && drag.scrollFrame) {
      window.cancelAnimationFrame(drag.scrollFrame);
      drag.scrollFrame = 0;
    }
  }

  function updateAppDrag(drag, x, y) {
    drag.x = x;
    drag.y = y;
    if (drag.preview) {
      drag.preview.style.left = `${x}px`;
      drag.preview.style.top = `${y}px`;
    }
    updateAppDropTarget(drag);
    updateAppDragScroll(drag);
  }

  function moveAppDrag(event) {
    const drag = state.appDrag;
    if (!drag || event.pointerId !== drag.pointerID) return;
    const distance = Math.hypot(event.clientX - drag.startX, event.clientY - drag.startY);
    if (!drag.isDragging) {
      if (drag.pointerType === "touch" && !drag.touchArmed) {
        if (distance > 10) cancelAppDrag();
        return;
      }
      if (distance < 4) return;
      startAppDrag(drag, event);
    }
    if (event.cancelable) event.preventDefault();
    updateAppDrag(drag, event.clientX, event.clientY);
  }

  function cancelAppDrag() {
    const drag = state.appDrag;
    if (!drag) return;
    window.clearTimeout(drag.armTimer);
    if (drag.scrollFrame) window.cancelAnimationFrame(drag.scrollFrame);
    drag.dropTarget?.classList.remove("app-drop-target");
    drag.sourceContainer?.classList.remove("app-drag-source");
    drag.source.removeAttribute("aria-grabbed");
    drag.preview?.remove();
    document.body.classList.remove("app-dragging");
    state.appDrag = null;
  }

  async function setFrontendList(item, listName) {
    const serviceID = item.backend.serviceID;
    const frontendID = item.frontend.id;
    const frontendURL = item.frontend.url;
    state.backends.forEach(backend => {
      if (backend.serviceID !== serviceID) return;
      backend.frontends.forEach(frontend => {
        const matches = frontendID ? frontend.id === frontendID : frontend.url === frontendURL;
        if (matches) frontend.list = listName;
      });
    });
    render();
    try {
      const result = await control(item.backend, "setFrontendList", {
        frontendID,
        frontendURL,
        list: listName
      });
      if (!result.ok) throw new Error(result.message || "Could not update the app list.");
    } catch (error) {
      await refreshBackends({ quiet: true });
      toast(error.message || String(error), true);
    }
  }

  function finishAppDrag(event) {
    const drag = state.appDrag;
    if (!drag || event.pointerId !== drag.pointerID) return;
    const wasDragging = drag.isDragging;
    const listName = drag.currentDropList;
    const shouldMove = wasDragging && listName !== null && listName !== drag.currentList;
    if (wasDragging) {
      if (event.cancelable) event.preventDefault();
      state.suppressLaunchUntil = Date.now() + 900;
    }
    cancelAppDrag();
    if (shouldMove) setFrontendList(drag.item, listName);
  }

  async function launch(identity, scope = "primary") {
    let item = findItem(identity);
    if (!item) return;
    let endpoint = item[scope];
    if (!endpoint) throw new Error(`${item.displayName} is not available for this account.`);
    if (!endpointReady(endpoint)) {
      toast(`Starting ${item.displayName}…`);
      const action = await control(endpoint.backend, "start");
      if (!action.ok) throw new Error(action.message || `Could not start ${item.displayName}.`);
      for (let attempt = 0; attempt < 30; attempt += 1) {
        await delay(500);
        await refreshBackends({ quiet: true });
        item = findItem(identity);
        endpoint = item?.[scope];
        if (endpoint && endpointReady(endpoint)) break;
      }
    }
    if (!endpoint || !endpointReady(endpoint)) throw new Error(`Timed out waiting for ${item?.displayName || "the app"}.`);
    window.location.assign(navigationURL(endpoint.frontend));
  }

  const delay = milliseconds => new Promise(resolve => window.setTimeout(resolve, milliseconds));

  async function control(backend, operation, values = {}, allowPassword = true) {
    const query = new URLSearchParams({ serviceID: backend.serviceID, scope: backend.serviceScope, operation });
    const body = new URLSearchParams(values);
    const { buffer } = await requestBuffer(`/api/control?${query}`, {
      method: "POST",
      headers: { "Content-Type": "application/x-www-form-urlencoded; charset=utf-8" },
      body
    }, true);
    let result;
    try {
      result = decodeAction(buffer);
    } catch (_) {
      throw new Error(decoder.decode(buffer).trim() || "Outer Shell could not complete the request.");
    }
    if (!result.ok && result.needsPassword && allowPassword) {
      const password = await requestPassword(backend.displayName, result.message);
      if (password === null) return result;
      return control(backend, operation, { ...values, sudoPassword: password }, false);
    }
    return result;
  }

  function openDialog(bodyHTML, className = "") {
    lockPageScroll();
    elements.dialogLayer.replaceChildren();
    const fragment = elements.dialogTemplate.content.cloneNode(true);
    const backdrop = fragment.querySelector(".dialog-backdrop");
    const dialog = fragment.querySelector(".dialog");
    dialog.className = `dialog ${className}`.trim();
    if (dialog.classList.contains("context-menu")) backdrop.classList.add("context-menu-backdrop");
    dialog.innerHTML = bodyHTML;
    backdrop.addEventListener("click", event => {
      if (event.target === backdrop) closeDialog();
    });
    elements.dialogLayer.append(fragment);
    window.setTimeout(() => {
      if (dialog.classList.contains("context-menu")) {
        dialog.tabIndex = -1;
        dialog.focus({ preventScroll: true });
      } else {
        dialog.querySelector("button, a, input, select, textarea")?.focus();
      }
    }, 0);
    return dialog;
  }

  function positionContextMenu(dialog, point) {
    if (!point || window.matchMedia("(max-width: 680px), (max-width: 950px) and (orientation: landscape)").matches) return;
    dialog.dataset.anchored = "true";
    dialog.style.visibility = "hidden";
    const bounds = dialog.getBoundingClientRect();
    const inset = 9;
    const left = Math.min(Math.max(point.x, inset), window.innerWidth - bounds.width - inset);
    const top = Math.min(Math.max(point.y, inset), window.innerHeight - bounds.height - inset);
    dialog.style.left = `${left}px`;
    dialog.style.top = `${top}px`;
    dialog.style.visibility = "visible";
  }

  function closeDialog() {
    elements.dialogLayer.replaceChildren();
    unlockPageScroll();
    state.logSelection = null;
    restartEventWatch();
  }

  function lockPageScroll() {
    if (state.pageScrollY !== null) return;
    state.pageScrollY = window.scrollY;
    document.body.style.top = `-${state.pageScrollY}px`;
    document.body.classList.add("dialog-open");
  }

  function unlockPageScroll() {
    if (state.pageScrollY === null) return;
    const scrollY = state.pageScrollY;
    state.pageScrollY = null;
    document.body.classList.remove("dialog-open");
    document.body.style.top = "";
    window.scrollTo(0, scrollY);
  }

  function requestPassword(name, message) {
    return new Promise(resolve => {
      const dialog = openDialog(`
        <header class="dialog-header">
          <div class="dialog-title-wrap"><h2 id="dialog-title">Administrator password</h2><p>${escapeHTML(name)}</p></div>
          <button class="dialog-close" type="button" data-password-cancel aria-label="Cancel">×</button>
        </header>
        <form id="password-form">
          <div class="dialog-body">
            <p>${escapeHTML(message || "Administrator password required.")}</p>
            <div class="field"><label for="sudo-password">Password</label><input id="sudo-password" name="password" type="password" autocomplete="current-password" required></div>
          </div>
          <footer class="dialog-footer"><button class="secondary-button" type="button" data-password-cancel>Cancel</button><button class="primary-button" type="submit">Continue</button></footer>
        </form>`);
      const finish = value => {
        elements.dialogLayer.replaceChildren();
        unlockPageScroll();
        resolve(value);
      };
      dialog.querySelectorAll("[data-password-cancel]").forEach(button => button.addEventListener("click", () => finish(null)));
      dialog.querySelector("#password-form").addEventListener("submit", event => {
        event.preventDefault();
        finish(new FormData(event.currentTarget).get("password"));
      });
      dialog.querySelector("#sudo-password")?.focus();
    });
  }

  function openAddDialog(tab = state.addTab) {
    state.addTab = tab;
    const available = state.backends.filter(backend => backend.isBundled && !backend.isInstalled && !backend.isMigration);
    const catalog = available.length ? `<div class="catalog-list">${available.map(backend => {
      const item = { backend, frontend: null, displayName: backend.displayName };
      return `<article class="catalog-item">
        ${iconHTML(item)}
        <div><h3>${escapeHTML(backend.displayName)}</h3><p>${escapeHTML(backend.rootOnly ? "Runs with full system access" : "Bundled with Outer Shell")}</p></div>
        <button class="primary-button" type="button" data-action="install" data-backend-key="${escapeHTML(backendKey(backend))}">${backend.rootOnly ? "Install as root" : "Install"}</button>
      </article>`;
    }).join("")}</div>` : `<div class="empty-state"><div class="empty-glyph">✓</div><h2>Bundled apps installed</h2><p>There are no additional bundled apps available.</p></div>`;
    const bash = `<form id="bash-form">
      <div class="form-grid">
        <div class="field"><label for="bash-name">Display name</label><input id="bash-name" name="name" placeholder="My server" required></div>
        <div class="field"><label for="bash-id">Identifier</label><input id="bash-id" class="mono" name="identifier" placeholder="my-server" required><span class="field-note">Used for the service and files.</span></div>
        <div class="field full"><label for="bash-command">Bash commands</label><textarea id="bash-command" name="command" placeholder="cd ~/dev/my-app&#10;npm start" required></textarea></div>
        <div class="field"><label for="bash-transport">Connection</label><select id="bash-transport" name="frontendTransport"><option value="port">TCP port</option><option value="unixSocket">Unix socket</option></select></div>
        <div class="field" data-transport="port"><label for="bash-port">Port</label><input id="bash-port" class="mono" name="port" inputmode="numeric" placeholder="4000" required></div>
        <div class="field" data-transport="unixSocket" hidden><label for="bash-socket">Socket path</label><input id="bash-socket" class="mono" name="socketPath" placeholder="/run/user/1000/my-app.sock"></div>
        <div class="field full"><label for="bash-icon">Icon path <span class="field-note">(optional)</span></label><input id="bash-icon" class="mono" name="iconPath" placeholder="~/Pictures/my-app.png"></div>
      </div>
      <p id="bash-message" class="form-message" role="status"></p>
      <footer class="dialog-footer"><button class="secondary-button" type="button" data-action="close-dialog">Cancel</button><button class="primary-button" type="submit">Create app</button></footer>
    </form>`;
    const dialog = openDialog(`
      <header class="dialog-header"><div class="dialog-title-wrap"><h2 id="dialog-title">Add apps</h2><p>Install a bundled app or run your own command.</p></div><button class="dialog-close" type="button" data-action="close-dialog" aria-label="Close">×</button></header>
      <div class="dialog-body">
        <nav class="tabs" aria-label="Add app type"><button class="tab ${tab === "catalog" ? "active" : ""}" type="button" data-action="add-tab" data-tab="catalog">Install apps</button><button class="tab ${tab === "bash" ? "active" : ""}" type="button" data-action="add-tab" data-tab="bash">Bash command</button></nav>
        ${tab === "catalog" ? catalog : bash}
      </div>`, tab === "catalog" ? "" : "wide");
    if (tab === "bash") wireBashForm(dialog);
  }

  function wireBashForm(dialog) {
    const name = dialog.querySelector("#bash-name");
    const identifier = dialog.querySelector("#bash-id");
    let identifierWasEdited = false;
    identifier.addEventListener("input", () => { identifierWasEdited = true; });
    name.addEventListener("input", () => {
      if (!identifierWasEdited) identifier.value = slug(name.value);
    });
    const transport = dialog.querySelector("#bash-transport");
    const updateTransport = () => {
      dialog.querySelectorAll("[data-transport]").forEach(field => {
        const active = field.dataset.transport === transport.value;
        field.hidden = !active;
        field.querySelector("input").required = active;
      });
    };
    transport.addEventListener("change", updateTransport);
    updateTransport();
    dialog.querySelector("#bash-form").addEventListener("submit", submitBashForm);
  }

  async function submitBashForm(event) {
    event.preventDefault();
    const form = event.currentTarget;
    const message = form.querySelector("#bash-message");
    const submit = form.querySelector("button[type=submit]");
    const values = Object.fromEntries(new FormData(form));
    const query = new URLSearchParams({
      recipe: "command-port",
      command: values.command,
      workdir: "~",
      frontendTransport: values.frontendTransport,
      name: values.name.trim(),
      identifier: values.identifier.trim()
    });
    if (values.frontendTransport === "unixSocket") query.set("socketPath", values.socketPath.trim());
    else query.set("port", values.port.trim());
    if (values.iconPath.trim()) query.set("iconPath", values.iconPath.trim());
    submit.disabled = true;
    submit.textContent = "Creating…";
    message.textContent = "";
    try {
      const { buffer } = await requestBuffer(`/api/create?${query}`, { method: "POST" }, true);
      const result = decodeAction(buffer);
      if (!result.ok) throw new Error(result.message || "Could not create the app.");
      closeDialog();
      toast(result.message || `Created ${values.name}.`);
      await refreshBackends({ quiet: true });
    } catch (error) {
      message.textContent = error.message || String(error);
      submit.disabled = false;
      submit.textContent = "Create app";
    }
  }

  function contextMenuGlyph(value) {
    return `<span class="context-menu-glyph" aria-hidden="true">${value}</span>`;
  }

  function endpointContextMenuHTML(item, scope, title) {
    const endpoint = item[scope];
    if (!endpoint) return "";
    const backend = endpoint.backend;
    const running = endpointRunning(endpoint);
    const showControl = backend.canControl && (running || !endpointReady(endpoint));
    const key = escapeHTML(backendKey(backend));
    const script = String(backend.scriptPath || "").trim();
    return `<section class="context-menu-section">
      <h3>${escapeHTML(title)}</h3>
      <a class="context-menu-item" href="${escapeHTML(navigationURL(endpoint.frontend))}" data-action="launch-endpoint" data-app-key="${escapeHTML(item.identity)}" data-app-scope="${scope}" role="menuitem">${contextMenuGlyph("↗")}<span>Open</span></a>
      ${showControl ? `<button class="context-menu-item" type="button" data-action="control" data-operation="${running ? "stop" : "start"}" data-backend-key="${key}" role="menuitem">${contextMenuGlyph(running ? "■" : "▶")}<span>${running ? "Stop" : "Start"}</span></button>` : ""}
      ${backend.logFiles.length ? `<button class="context-menu-item" type="button" data-action="logs" data-backend-key="${key}" role="menuitem">${contextMenuGlyph("≡")}<span>View Logs</span></button>` : ""}
      ${script ? `<button class="context-menu-item" type="button" data-action="copy-script" data-script="${escapeHTML(script)}" role="menuitem">${contextMenuGlyph("⧉")}<span>Copy Script Path</span></button>` : ""}
    </section>`;
  }

  function showContextMenu(contents, point = null, label = "Actions") {
    const dialog = openDialog(`<h2 id="dialog-title" class="context-menu-title">${escapeHTML(label)}</h2><div class="context-menu-scroll" role="menu">${contents}</div>`, "context-menu");
    dialog.setAttribute("aria-label", label);
    positionContextMenu(dialog, point);
    return dialog;
  }

  function openAppMenu(identity, point = null) {
    const item = findItem(identity);
    if (!item) return;
    const backend = item.backend;
    const managementBackend = item.user?.backend || backend;
    const sections = [];
    if (backend.rootOnly) {
      sections.push(endpointContextMenuHTML(item, "root", "Root"));
    } else {
      sections.push(endpointContextMenuHTML(item, "user", "User"));
      sections.push(endpointContextMenuHTML(item, "root", "Root"));
    }
    const management = [];
    if (managementBackend.supportsRoot && !managementBackend.rootOnly) {
      const hasRootSupport = Boolean(item.root || managementBackend.hasRootSupport);
      management.push(`<button class="context-menu-item" type="button" data-action="control" data-operation="${hasRootSupport ? "removeRootSupport" : "addRootSupport"}" data-backend-key="${escapeHTML(backendKey(managementBackend))}" role="menuitem">${contextMenuGlyph("◇")}<span>${hasRootSupport ? "Reinstall as User-only" : "Reinstall with Root Support"}</span></button>`);
    }
    if (backend.canUninstall) {
      management.push(`<button class="context-menu-item danger" type="button" data-action="uninstall" data-backend-key="${escapeHTML(backendKey(backend))}" role="menuitem">${contextMenuGlyph("−")}<span>Uninstall</span></button>`);
    }
    if (management.length) sections.push(`<section class="context-menu-section context-menu-management">${management.join("")}</section>`);
    showContextMenu(`<div class="context-menu-app-heading">${launcherIconHTML(item)}<strong>${escapeHTML(item.displayName)}</strong></div>${sections.filter(Boolean).join("")}`, point, `${item.displayName} actions`);
  }

  function openHomeMenu(anchor) {
    const outerShell = state.backends.find(backend => backend.serviceID === "org.outershell.OuterShell" && backend.serviceScope !== "system")
      || state.backends.find(backend => backend.serviceID === "org.outershell.OuterShell");
    if (!outerShell) return;
    const key = escapeHTML(backendKey(outerShell));
    const actions = [
      `<button class="context-menu-item plain" type="button" data-action="about-outer-shell" data-backend-key="${key}" role="menuitem">About Outer Shell</button>`,
      `<button class="context-menu-item plain" type="button" data-action="logs" data-backend-key="${key}" role="menuitem">View Logs for Outer Shell</button>`
    ];
    if (outerShell.menuBarVisibilityAvailable) {
      actions.push(`<button class="context-menu-item menu-toggle" type="button" data-action="toggle-menu-bar" data-backend-key="${key}" data-enabled="${outerShell.menuBarVisibilityEnabled ? "true" : "false"}" role="menuitemcheckbox" aria-checked="${outerShell.menuBarVisibilityEnabled ? "true" : "false"}">${contextMenuGlyph(outerShell.menuBarVisibilityEnabled ? "✓" : "")}<span>Show in macOS menu bar when backends are running</span></button>`);
    }
    actions.push(`<button class="context-menu-item plain" type="button" data-action="check-outer-shell-update" data-backend-key="${key}" role="menuitem">Check for Updates</button>`);
    actions.push(`<button class="context-menu-item plain" type="button" data-action="uninstall-outer-shell" data-backend-key="${key}" role="menuitem">Uninstall Outer Shell</button>`);
    const bounds = anchor.getBoundingClientRect();
    const point = { x: Math.max(9, bounds.right - 280), y: bounds.bottom + 5 };
    showContextMenu(`<section class="context-menu-section">${actions.join("")}</section>`, point, "Outer Shell");
  }

  function openOuterShellAbout(key) {
    const backend = findBackend(key);
    if (!backend) return;
    const version = String(backend.installedVersion || "").trim() || "unknown";
    const updateSource = String(backend.publicBaseURL || "").trim() || "unknown";
    const text = [
      "Outer Shell",
      `Version: ${version}`,
      `Service ID: ${backend.serviceID}`,
      `Scope: ${backend.serviceScope}`,
      `Status: ${backend.status}`,
      `Update source: ${updateSource}`
    ].join("\n");
    openDialog(`
      <header class="dialog-header"><div class="dialog-title-wrap"><h2 id="dialog-title">About Outer Shell</h2></div><button class="dialog-close" type="button" data-action="close-dialog" aria-label="Close">×</button></header>
      <div class="dialog-body"><pre class="about-output">${escapeHTML(text)}</pre></div>
      <footer class="dialog-footer"><button class="primary-button" type="button" data-action="close-dialog">OK</button></footer>`);
  }

  function openOuterShellUpdate(backend, result) {
    const installed = String(result.installedVersion || backend.installedVersion || "").trim() || "unknown";
    const available = String(result.availableVersion || backend.availableVersion || "").trim() || "the latest version";
    openDialog(`
      <header class="dialog-header"><div class="dialog-title-wrap"><h2 id="dialog-title">Update Outer Shell</h2><p>${escapeHTML(result.message || `Outer Shell ${available} is available.`)}</p></div><button class="dialog-close" type="button" data-action="close-dialog" aria-label="Close">×</button></header>
      <div class="dialog-body"><p>Update Outer Shell from version <strong>${escapeHTML(installed)}</strong> to <strong>${escapeHTML(available)}</strong>?</p></div>
      <footer class="dialog-footer"><button class="secondary-button" type="button" data-action="close-dialog">Cancel</button><button class="primary-button" type="button" data-action="update-outer-shell" data-backend-key="${escapeHTML(backendKey(backend))}">Update</button></footer>`);
  }

  async function checkOuterShellUpdate(key, button) {
    const backend = findBackend(key);
    if (!backend) return;
    button.disabled = true;
    const original = button.textContent;
    button.textContent = "Checking for Updates…";
    try {
      const result = await control(backend, "checkUpdate");
      if (!result.ok) throw new Error(result.message || "Could not check for updates.");
      if (result.updateAvailable) {
        openOuterShellUpdate(backend, result);
      } else {
        closeDialog();
        toast(result.message || "Outer Shell is up to date.");
      }
    } catch (error) {
      toast(error.message || String(error), true);
      button.disabled = false;
      button.textContent = original;
    }
  }

  async function performOuterShellOperation(key, operation, button, reloadAfter = false) {
    const backend = findBackend(key);
    if (!backend) return;
    button.disabled = true;
    const original = button.textContent;
    button.textContent = `${original}…`;
    try {
      const result = await control(backend, operation);
      if (!result.ok) throw new Error(result.message || "The action failed.");
      closeDialog();
      toast(result.message);
      if (reloadAfter) window.setTimeout(() => window.location.reload(), 1250);
    } catch (error) {
      toast(error.message || String(error), true);
      button.disabled = false;
      button.textContent = original;
    }
  }

  async function uninstallOuterShell(key, button) {
    if (!window.confirm("Uninstall Outer Shell?")) return;
    await performOuterShellOperation(key, "uninstallOuterShell", button);
  }

  async function performControl(key, operation, button) {
    const backend = findBackend(key);
    if (!backend) return;
    button.disabled = true;
    const original = button.textContent;
    button.textContent = `${original}…`;
    try {
      const result = await control(backend, operation);
      if (!result.ok) throw new Error(result.message || "The action failed.");
      closeDialog();
      toast(result.message);
      await refreshBackends({ quiet: true });
    } catch (error) {
      toast(error.message || String(error), true);
      button.disabled = false;
      button.textContent = original;
    }
  }

  async function installBackend(key, button) {
    const backend = findBackend(key);
    if (!backend) return;
    button.disabled = true;
    button.textContent = "Installing…";
    try {
      const operation = backend.rootOnly ? "runRoot" : "run";
      const result = await control(backend, operation);
      if (!result.ok) throw new Error(result.message || `Could not install ${backend.displayName}.`);
      closeDialog();
      toast(result.message || `Installed ${backend.displayName}.`);
      await refreshBackends({ quiet: true });
    } catch (error) {
      toast(error.message || String(error), true);
      button.disabled = false;
      button.textContent = backend.rootOnly ? "Install as root" : "Install";
    }
  }

  async function uninstallBackend(key, button) {
    const backend = findBackend(key);
    if (!backend || !window.confirm(`Uninstall ${backend.displayName}?`)) return;
    await performControl(key, "uninstall", button);
  }

  async function copyText(text) {
    try {
      await navigator.clipboard.writeText(text);
      toast("Copied to the clipboard.");
    } catch (_) {
      const area = document.createElement("textarea");
      area.value = text;
      document.body.append(area);
      area.select();
      document.execCommand("copy");
      area.remove();
      toast("Copied to the clipboard.");
    }
  }

  function openLogs(key, selectedIndex = 0) {
    const backend = findBackend(key);
    if (!backend) return;
    if (!backend.logFiles.length) {
      state.logSelection = null;
      openDialog(`
        <header class="dialog-header"><div class="dialog-title-wrap"><h2 id="dialog-title">${escapeHTML(backend.displayName)} logs</h2><p>No registered log file.</p></div><button class="dialog-close" type="button" data-action="close-dialog" aria-label="Close">×</button></header>
        <pre class="log-output">No registered log file.</pre>`, "log-dialog");
      restartEventWatch();
      return;
    }
    state.logSelection = { backendKey: key, index: Math.min(selectedIndex, backend.logFiles.length - 1) };
    openDialog(`
      <header class="dialog-header"><div class="dialog-title-wrap"><h2 id="dialog-title">${escapeHTML(backend.displayName)} logs</h2><p>Updates automatically while this window is open.</p></div><button class="dialog-close" type="button" data-action="close-dialog" aria-label="Close">×</button></header>
      <div class="log-toolbar">
        <select id="log-select" aria-label="Log file">${backend.logFiles.map((log, index) => `<option value="${index}" ${index === state.logSelection.index ? "selected" : ""}>${escapeHTML(log.displayName || log.path)}</option>`).join("")}</select>
        <span id="log-meta" class="log-meta">Loading…</span>
        <button class="secondary-button" type="button" data-action="refresh-log">Refresh</button>
      </div>
      <pre id="log-output" class="log-output">Loading logs…</pre>`, "log-dialog");
    restartEventWatch();
    fetchSelectedLog();
  }

  async function fetchSelectedLog() {
    const selection = state.logSelection;
    if (!selection) return;
    const backend = findBackend(selection.backendKey);
    if (!backend) return;
    const output = elements.dialogLayer.querySelector("#log-output");
    const meta = elements.dialogLayer.querySelector("#log-meta");
    try {
      const query = new URLSearchParams({ serviceID: backend.serviceID, logIndex: String(selection.index), bytes: String(512 * 1024) });
      const { buffer } = await requestBuffer(`/api/logs?${query}`);
      const log = decodeLog(buffer);
      if (!state.logSelection || state.logSelection.backendKey !== selection.backendKey || state.logSelection.index !== selection.index) return;
      if (output) output.textContent = log.error || log.contents || "No logs yet.";
      if (meta) meta.textContent = `${log.path || "No registered log"}${log.truncated ? " · showing latest output" : ""} · ${formatBytes(log.fileSize)}`;
      if (output && output.scrollHeight - output.scrollTop - output.clientHeight < 90) output.scrollTop = output.scrollHeight;
    } catch (error) {
      if (output) output.textContent = error.message || String(error);
      if (meta) meta.textContent = "Could not read logs";
    }
  }

  function formatBytes(bytes) {
    if (!Number.isFinite(bytes) || bytes <= 0) return "0 B";
    const units = ["B", "KB", "MB", "GB"];
    const unit = Math.min(Math.floor(Math.log(bytes) / Math.log(1024)), units.length - 1);
    const value = bytes / (1024 ** unit);
    return `${value >= 10 || unit === 0 ? value.toFixed(0) : value.toFixed(1)} ${units[unit]}`;
  }

  function restartEventWatch() {
    state.eventAbort?.abort();
  }

  async function watchEvents() {
    while (!state.stopped) {
      const controller = new AbortController();
      state.eventAbort = controller;
      const query = new URLSearchParams({
        sinceBackends: state.backendsVersion.toString(),
        sinceLog: state.logVersion.toString()
      });
      const selection = state.logSelection;
      if (selection) {
        const backend = findBackend(selection.backendKey);
        if (backend) {
          query.set("serviceID", backend.serviceID);
          query.set("logIndex", String(selection.index));
        }
      }
      try {
        const { buffer } = await requestBuffer(`/api/events?${query}`, { signal: controller.signal });
        const event = decodeEvents(buffer);
        state.backendsVersion = event.backendsVersion;
        state.logVersion = event.logVersion;
        if (event.backendsChanged) await refreshBackends({ quiet: true });
        if (event.logChanged) await fetchSelectedLog();
      } catch (error) {
        if (error.name !== "AbortError") await delay(1200);
      }
    }
  }

  elements.search.addEventListener("input", event => {
    state.query = event.target.value;
    render();
  });
  elements.add.addEventListener("click", () => openAddDialog());
  elements.refresh.addEventListener("click", event => openHomeMenu(event.currentTarget));
  document.body.addEventListener("contextmenu", event => {
    const identity = itemIdentityFromTarget(event.target);
    if (!identity) return;
    event.preventDefault();
    cancelLongPress();
    cancelAppDrag();
    clearPressFeedback();
    if (Date.now() < state.suppressLaunchUntil && elements.dialogLayer.childElementCount) return;
    openAppMenu(identity, { x: event.clientX, y: event.clientY });
  });
  document.body.addEventListener("pointerdown", beginLongPress);
  document.body.addEventListener("pointerdown", beginPressFeedback);
  document.body.addEventListener("pointerdown", beginAppDrag);
  document.body.addEventListener("pointermove", moveLongPress);
  document.body.addEventListener("pointermove", movePressFeedback);
  document.body.addEventListener("pointermove", moveAppDrag, { passive: false });
  document.body.addEventListener("touchmove", event => {
    if (state.appDrag?.touchArmed || state.appDrag?.isDragging) event.preventDefault();
  }, { passive: false });
  document.body.addEventListener("pointerup", event => { finishAppDrag(event); cancelLongPress(); clearPressFeedback(); });
  document.body.addEventListener("pointercancel", () => { cancelAppDrag(); cancelLongPress(); clearPressFeedback(); });
  document.body.addEventListener("lostpointercapture", () => { cancelAppDrag(); cancelLongPress(); clearPressFeedback(); });
  document.body.addEventListener("selectstart", event => {
    if (itemIdentityFromTarget(event.target) || Date.now() < state.suppressLaunchUntil) event.preventDefault();
  });
  document.body.addEventListener("click", async event => {
    const actionElement = event.target.closest("[data-action]");
    if (!actionElement) return;
    if (actionElement.classList.contains("dialog-backdrop") && event.target !== actionElement) return;
    const action = actionElement.dataset.action;
    if (Date.now() < state.suppressLaunchUntil && actionElement.closest(".app-sections")) {
      event.preventDefault();
      return;
    }
    if (action === "close-dialog") { closeDialog(); return; }
    if (action === "open-add") { openAddDialog(); return; }
    if (action === "start-safe-space") {
      try { await startSafeSpace(actionElement.dataset.safeSpaceId); }
      catch (error) { toast(error.message || String(error), true); }
      return;
    }
    if (action === "launch-safe-space-app") {
      const workspace = findSafeSpace(actionElement.dataset.safeSpaceId);
      const app = workspace?.apps?.find(candidate => candidate.serviceID === actionElement.dataset.serviceId);
      if (workspace && app && safeSpaceAppIsReady(workspace, app)) return;
      event.preventDefault();
      try { await launchSafeSpaceApp(actionElement.dataset.safeSpaceId, actionElement.dataset.serviceId); }
      catch (error) { toast(error.message || String(error), true); }
      return;
    }
    if (action === "add-tab") { openAddDialog(actionElement.dataset.tab); return; }
    if (action === "about-outer-shell") { openOuterShellAbout(actionElement.dataset.backendKey); return; }
    if (action === "check-outer-shell-update") { await checkOuterShellUpdate(actionElement.dataset.backendKey, actionElement); return; }
    if (action === "update-outer-shell") { await performOuterShellOperation(actionElement.dataset.backendKey, "update", actionElement, true); return; }
    if (action === "uninstall-outer-shell") { await uninstallOuterShell(actionElement.dataset.backendKey, actionElement); return; }
    if (action === "toggle-menu-bar") {
      const operation = actionElement.dataset.enabled === "true" ? "hideMenuBarWhenRunning" : "showMenuBarWhenRunning";
      await performControl(actionElement.dataset.backendKey, operation, actionElement);
      return;
    }
    if (action === "launch-endpoint") {
      if (Date.now() < state.suppressLaunchUntil && !actionElement.closest(".context-menu")) {
        event.preventDefault();
        return;
      }
      const item = findItem(actionElement.dataset.appKey);
      const scope = actionElement.dataset.appScope;
      const endpoint = item?.[scope];
      if (endpoint && endpointReady(endpoint)) return;
      event.preventDefault();
      try { await launch(actionElement.dataset.appKey, scope); } catch (error) { toast(error.message || String(error), true); }
      return;
    }
    if (action === "launch") {
      if (Date.now() < state.suppressLaunchUntil && !actionElement.closest(".context-menu")) {
        event.preventDefault();
        return;
      }
      const item = findItem(actionElement.dataset.appKey);
      if (item && endpointReady(item.primary)) return;
      event.preventDefault();
      try { await launch(actionElement.dataset.appKey); } catch (error) { toast(error.message || String(error), true); }
      return;
    }
    if (action === "install") { await installBackend(actionElement.dataset.backendKey, actionElement); return; }
    if (action === "control") { await performControl(actionElement.dataset.backendKey, actionElement.dataset.operation, actionElement); return; }
    if (action === "uninstall") { await uninstallBackend(actionElement.dataset.backendKey, actionElement); return; }
    if (action === "copy-script") { const script = actionElement.dataset.script; closeDialog(); await copyText(script); return; }
    if (action === "logs") { openLogs(actionElement.dataset.backendKey); return; }
    if (action === "refresh-log") { await fetchSelectedLog(); }
  });
  document.body.addEventListener("change", event => {
    if (event.target.id !== "log-select" || !state.logSelection) return;
    state.logSelection.index = Number(event.target.value) || 0;
    state.logVersion = 0n;
    restartEventWatch();
    fetchSelectedLog();
  });
  document.addEventListener("keydown", event => {
    if (event.key === "Escape" && elements.dialogLayer.childElementCount) closeDialog();
    if (event.key === "ContextMenu" || (event.shiftKey && event.key === "F10")) {
      const identity = itemIdentityFromTarget(event.target);
      if (!identity) return;
      event.preventDefault();
      const bounds = event.target.getBoundingClientRect();
      openAppMenu(identity, { x: bounds.left, y: bounds.bottom });
    }
  });
  window.addEventListener("beforeunload", () => {
    cancelAppDrag();
    state.stopped = true;
    state.eventAbort?.abort();
    window.clearInterval(state.safeSpacesTimer);
  });

  document.addEventListener("visibilitychange", () => {
    if (!document.hidden) refreshSafeSpaces({ quiet: true });
  });
  state.safeSpacesTimer = window.setInterval(() => {
    if (!document.hidden && !state.safeSpaceBusy.size) refreshSafeSpaces({ quiet: true });
  }, 2000);

  Promise.all([refreshBackends(), refreshSafeSpaces()]).then(watchEvents);
})();
