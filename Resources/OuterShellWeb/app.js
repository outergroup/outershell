(() => {
  "use strict";

  document.documentElement.classList.toggle("outerloop-host", /(^|\.)outerloop\.localhost$/i.test(window.location.hostname));

  const decoder = new TextDecoder();
  const elements = {
    shell: document.querySelector("#app"),
    status: document.querySelector("#status-banner"),
    overview: document.querySelector("#server-overview"),
    add: document.querySelector("#add-button"),
    dialogLayer: document.querySelector("#dialog-layer"),
    dialogTemplate: document.querySelector("#dialog-template"),
    toasts: document.querySelector("#toast-region")
  };

  const state = {
    hostPlatform: null,
    backends: [],
    backendsReady: false,
    safeSpacesReady: false,
    overviewReady: false,
    showInitialLoading: false,
    initialLoadingTimer: null,
    endpointNames: {},
    layoutReady: false,
    layoutSaving: false,
    layoutFetching: false,
    layoutAbort: null,
    layoutRevision: 0n,
    layoutError: "",
    safeSpaces: [],
    providers: [],
    loading: true,
    safeSpacesLoading: true,
    safeSpacesRefreshing: false,
    backendError: "",
    safeSpacesError: "",
    safeSpaceBusy: new Set(),
    overviewVersion: 0n,
    busy: false,
    query: "",
    addTab: "agent",
    addScope: "user",
    backendsVersion: 0n,
    logVersion: 0n,
    logSelection: null,
    eventAbort: null,
    stopped: false,
    suspended: document.hidden,
    refreshAbort: {},
    refreshErrorTimers: {},
    longPress: null,
    suppressLaunchUntil: 0,
    pageScrollY: null,
    pressFeedback: null,
    groupOrder: [],
    groupPins: {},
    endpointOrder: {},
    endpointDrag: null,
    groupDrag: null,
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
      iconData: flags & 0x800 ? reader.bytesRef(116) : new Uint8Array(),
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
      iconData: flags & 2 ? new Uint8Array() : reader.bytesRef(32),
      iconURL: flags & 2 ? reader.stringRef(32) : "",
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
      logVersion: reader.u64(16),
      overviewChanged: Boolean(flags & 0x08),
      overviewVersion: reader.u64(24)
    };
  }

  const escapeHTML = value => String(value ?? "").replace(/[&<>'"]/g, character => ({
    "&": "&amp;", "<": "&lt;", ">": "&gt;", "'": "&#39;", '"': "&quot;"
  })[character]);

  function backendKey(backend) {
    return [backend.serviceID, backend.serviceScope, backend.serviceUnitPath || backend.serviceUnit].join("\u001f");
  }

  function findBackend(key) {
    return (state.addWorkspace ? state.workspaceCatalog || [] : state.backends).find(backend => backendKey(backend) === key);
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
    const source = dataURL(item.backend?.iconData) || item.frontend?.iconURL || dataURL(item.frontend?.iconData);
    const content = source
      ? `<img src="${escapeHTML(source)}" alt="">`
      : `<span>${escapeHTML(initials(item.displayName || item.backend.displayName))}</span>`;
    return `<span class="app-icon${source ? " has-image" : ""} ${extraClass}" aria-hidden="true">${content}</span>`;
  }

  function launcherIconHTML(item) {
    const source = dataURL(item.backend?.iconData) || item.app?.iconURL || (item.app?.iconData
      ? `data:image/png;base64,${escapeHTML(item.app.iconData)}`
      : (item.frontend?.iconURL || dataURL(item.frontend?.iconData)));
    const content = source
      ? `<img src="${escapeHTML(source)}" alt="">`
      : `<span class="launcher-fallback" aria-hidden="true">${escapeHTML(initials(item.displayName).slice(0, 1))}</span>`;
    return `<span class="launcher-icon" aria-hidden="true">${content}</span>`;
  }

  function listIconHTML(item) {
    const source = dataURL(item.backend?.iconData) || item.app?.iconURL || (item.app?.iconData
      ? `data:image/png;base64,${escapeHTML(item.app.iconData)}`
      : (item.frontend?.iconURL || dataURL(item.frontend?.iconData)));
    const content = source
      ? `<img src="${escapeHTML(source)}" alt="">`
      : `<span>${escapeHTML(initials(item.displayName).slice(0, 1))}</span>`;
    return `<span class="list-icon${source ? " has-image" : ""}" aria-hidden="true">${content}</span>`;
  }

  function safeSpaceAppIconHTML(app) {
    const source = app.iconURL || (app.iconData ? `data:image/png;base64,${app.iconData}` : "");
    const content = source
      ? `<img src="${escapeHTML(source)}" alt="">`
      : `<span>${escapeHTML(initials(app.displayName || app.serviceID).slice(0, 1))}</span>`;
    return `<span class="safe-space-app-icon${source ? " has-image" : ""}" aria-hidden="true">${content}</span>`;
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
    if (/^http\+(unix|pipe):\/\//i.test(raw)) {
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
    if (socket) target = `http+${socket.startsWith("\\\\.\\pipe\\") ? "pipe" : "unix"}://${encodeURIComponent(socket)}${pathAndQuery(frontend)}`;
    else if (frontend.port > 0) target = `http://127.0.0.1:${frontend.port}${pathAndQuery(frontend)}`;
    else target = String(frontend.url || "").trim() || "#";

    const token = String(frontend.iconObservationToken || "");
    if (window.outerLoopPageIconObservationSupported === true && !frontend.iconData?.length && !frontend.iconURL && token) {
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

  function endpointFailure(endpoint, name) {
    if (/^\/(?:private\/)?var\/folders\//.test(endpoint.frontend.socketPath || "") &&
        endpoint.backend.status === "socket unavailable") {
      return `${name} uses an older macOS socket configuration that is failing on this Mac. Reinstall the bundled app to let launchd choose its socket location. Its new service address will stay stable when the Mac restarts.`;
    }
    if (endpoint.backend.status === "suspended after failures") return `${name} was suspended after repeated launch failures to stop automatic restarts. Check the logs, then choose Retry when ready.`;
    if (endpoint.backend.status === "socket unavailable") return `${name} couldn’t start: its connection socket is unavailable.`;
    if (endpoint.backend.status === "failed") return `${name} exited with an error.`;
    return "";
  }

  function endpointReady(endpoint) {
    if (endpointFailure(endpoint, "The app")) return false;
    const frontend = endpoint.frontend;
    const hasEndpoint = Boolean(frontend.socketPath || frontend.port > 0 || /^[a-z][a-z0-9+.-]*:/i.test(frontend.url || ""));
    return endpointRunning(endpoint) || (["available", "awaiting"].includes(endpoint.backend.status) && hasEndpoint);
  }

  function launcherItems(includeShell = false) {
    const endpoints = state.backends.flatMap(backend => {
      if ((!includeShell && backend.serviceID === "org.outershell.OuterShell") || !backend.isInstalled) return [];
      return backend.frontends.map(frontend => ({ backend, frontend }));
    });
    return endpoints.map(endpoint => {
      const root = endpoint.backend.serviceScope === "system";
      const name = endpoint.frontend.name.trim() || endpoint.backend.displayName.trim() || "Untitled";
      return {
        identity: [backendKey(endpoint.backend), endpoint.frontend.id || endpoint.frontend.url].join("\u001f"),
        primary: endpoint,
        backend: endpoint.backend,
        frontend: endpoint.frontend,
        displayName: state.endpointNames[hostBookmarkKey(endpoint)] || name,
        bookmarkContext: root ? "root" : "",
        subtitle: endpoint.backend.displayName.trim() || endpoint.backend.serviceID
      };
    }).sort((left, right) => left.displayName.localeCompare(right.displayName, undefined, { sensitivity: "base" }));
  }


  function hostBookmarkKey(item) {
    if (item.custom) return item.identity;
    if (item.workspace) return containerBookmarkKey(item.workspace, item.app);
    return JSON.stringify(["host", item.backend.serviceScope, item.backend.serviceID, item.frontend.id || navigationURL(item.frontend)]);
  }

  function containerFrontendKey(app) {
    return JSON.stringify([app.frontendID || "", app.socketPath || "", app.url || "", app.port || 0]);
  }

  function findSafeSpaceApp(workspace, serviceID, frontendKey = "") {
    return workspace?.apps?.find(app => app.serviceID === serviceID && (!frontendKey || containerFrontendKey(app) === frontendKey));
  }

  function containerBookmarkKey(workspace, app) {
    return JSON.stringify(["container", workspace.id, app.serviceID, containerFrontendKey(app)]);
  }

  function validateLayout(value) {
    const object = value => value && typeof value === "object" && !Array.isArray(value);
    const strings = value => Array.isArray(value) && value.every(item => typeof item === "string");
    if (!object(value) || (value.version !== undefined && value.version !== 1)) throw new Error("Unsupported server layout.");
    const result = { version: 1, pins: value.pins ?? {}, order: value.order ?? {}, groups: value.groups ?? [], names: value.names ?? {} };
    if (![result.pins, result.order].every(map => object(map) && Object.values(map).every(strings)) || !strings(result.groups) || !object(result.names) || !Object.values(result.names).every(name => typeof name === "string")) throw new Error("Invalid server layout.");
    return result;
  }

  function currentLayout() {
    return { version: 1, pins: state.groupPins, order: state.endpointOrder, groups: state.groupOrder, names: state.endpointNames };
  }

  function applyLayout(layout) {
    state.groupPins = layout.pins;
    state.endpointOrder = layout.order;
    state.groupOrder = layout.groups;
    state.endpointNames = layout.names;
  }

  function encodeLayout(layout, revision) {
    const json = new TextEncoder().encode(JSON.stringify(validateLayout(layout)));
    const bytes = new Uint8Array(16 + json.length);
    bytes.set(new TextEncoder().encode("OSLAY001"));
    new DataView(bytes.buffer).setBigUint64(8, revision, true);
    bytes.set(json, 16);
    return bytes;
  }

  function decodeLayout(buffer) {
    const bytes = new Uint8Array(buffer);
    if (bytes.length < 18 || decoder.decode(bytes.slice(0, 8)) !== "OSLAY001") throw new Error("Invalid layout response.");
    return { revision: new DataView(buffer).getBigUint64(8, true), layout: validateLayout(JSON.parse(new TextDecoder("utf-8", { fatal: true }).decode(bytes.slice(16)))) };
  }

  async function fetchLayout(signal) {
    const { buffer } = await requestBuffer("/api/layout", { signal });
    return decodeLayout(buffer);
  }

  async function writeLayout(layout, revision, signal) {
    const { response, buffer } = await requestBuffer("/api/layout", { method: "POST", headers: { "Content-Type": "application/octet-stream" }, body: encodeLayout(layout, revision), signal }, true);
    if (!response.ok) {
      const error = new Error(response.status === 409 ? "Layout changed in another browser. The latest layout has been loaded; please try again." : "Could not save the layout on the server. Please try again.");
      error.conflict = response.status === 409;
      throw error;
    }
    return decodeLayout(buffer);
  }

  async function refreshLayout() {
    if (state.layoutFetching || state.layoutSaving || state.groupDrag || state.endpointDrag || state.suspended || state.stopped) return;
    state.layoutFetching = true;
    const controller = new AbortController();
    state.layoutAbort = controller;
    try {
      const result = await fetchLayout(controller.signal);
      if (state.layoutAbort !== controller) return;
      if (state.layoutAbort !== controller || state.layoutSaving || state.groupDrag || state.endpointDrag) return;
      if (!state.layoutReady || result.revision > state.layoutRevision) {
        applyLayout(result.layout);
        state.layoutRevision = result.revision;
        state.layoutReady = true;
        render();
      }
      clearRefreshFailure("layoutError");
      state.layoutError = "";
      updateStatus();
    } catch (error) { if (state.layoutAbort === controller) reportRefreshFailure("layoutError", error, true); }
    finally { if (state.layoutAbort === controller) { state.layoutAbort = null; state.layoutFetching = false; } }
  }

  function saveLayout(layout) {
    if (!state.layoutReady || state.layoutSaving) { toast("Please wait for the layout to finish saving or loading.", true); return false; }
    const previous = currentLayout();
    const revision = state.layoutRevision;
    state.layoutSaving = true;
    applyLayout(layout);
    void (async () => {
      try {
        const result = await writeLayout(layout, revision);
        applyLayout(result.layout);
        state.layoutRevision = result.revision;
      } catch (error) {
        applyLayout(previous);
        if (error.conflict) {
          try { const result = await fetchLayout(); applyLayout(result.layout); state.layoutRevision = result.revision; }
          catch { state.layoutReady = false; }
        }
        toast(error.message, true);
      } finally { state.layoutSaving = false; render(); }
    })();
    return true;
  }

  function updatePage() {
    document.title = "Outer Shell";
    renderServerOverview();
  }

  function addressKind(frontend) {
    const socket = String(frontend.socketPath || "");
    if (!socket) return Number(frontend.port) > 0 ? "Ports" : "Other endpoints";
    if (/^\/(?:var\/)?run\/user\/0(?:\/|$)/.test(socket)) return "Root sockets";
    if (/^\/(?:var\/)?run\/user\/\d+\//.test(socket)) return "User sockets";
    if (/^\/(?:var\/)?run\//.test(socket)) return "Root sockets";
    return "Custom sockets";
  }

  function registeredURL(frontend) {
    const socket = String(frontend.socketPath || "").trim();
    if (socket) return `http+${socket.startsWith("\\\\.\\pipe\\") ? "pipe" : "unix"}://${encodeURIComponent(socket)}${pathAndQuery(frontend)}`;
    if (Number(frontend.port) > 0) return `http://127.0.0.1:${Number(frontend.port)}${pathAndQuery(frontend)}`;
    return String(frontend.url || "");
  }

  function registeredAddresses() {
    const host = launcherItems(true).map(item => ({
      key: hostBookmarkKey(item), name: item.displayName, item,
      kind: addressKind(item.frontend), address: registeredURL(item.frontend),
      socket: item.frontend.socketPath || "", running: endpointRunning(item.primary),
      failure: endpointFailure(item.primary, item.displayName),
      serviceID: item.backend.serviceID, groupID: "host", groupName: "This server"
    }));
    const containers = state.safeSpaces.flatMap(workspace => (workspace.apps || []).filter(app => workspace.runtimeKind !== "wsl" || app.serviceID !== "org.outershell.OuterShell").map(app => {
      const frontend = { ...app, port: app.port || 0 };
      return {
        key: containerBookmarkKey(workspace, app), name: state.endpointNames[containerBookmarkKey(workspace, app)] || app.displayName || app.serviceID,
        workspace, app, kind: addressKind(frontend), address: registeredURL(frontend),
        socket: app.socketPath || "", running: safeSpaceAppIsReady(workspace, app),
        serviceID: app.serviceID, groupID: workspace.id, groupName: workspace.name || "Container"
      };
    }));
    return [...host, ...containers];
  }

  function overviewLaunchAttributes(entry) {
    return entry.item
      ? `data-action="launch" data-app-key="${escapeHTML(entry.item.identity)}"`
      : `data-action="launch-safe-space-app" data-safe-space-id="${escapeHTML(entry.workspace.id)}" data-service-id="${escapeHTML(entry.app.serviceID)}" data-frontend-key="${escapeHTML(containerFrontendKey(entry.app))}"`;
  }

  function overviewEndpoint(entry) {
    const target = entry.item ? navigationURL(entry.item.frontend) : safeSpaceAppNavigationURL(entry.workspace, entry.app);
    const isThisPage = entry.serviceID === "org.outershell.OuterShell" && entry.item?.backend.serviceScope !== "system" && !entry.workspace;
    const icon = listIconHTML(entry.item || { app: entry.app, displayName: entry.name });
    const menu = entry.item
      ? `data-action="edit-bookmark" data-app-key="${escapeHTML(entry.item.identity)}"`
      : `data-action="edit-container-bookmark" data-safe-space-id="${escapeHTML(entry.workspace.id)}" data-service-id="${escapeHTML(entry.app.serviceID)}" data-frontend-key="${escapeHTML(containerFrontendKey(entry.app))}"`;
    return `<div class="overview-row" data-endpoint-key="${escapeHTML(entry.key)}"><a href="${escapeHTML(target)}" ${overviewLaunchAttributes(entry)} ${entry.item ? "" : `data-app-key="${escapeHTML(entry.key)}"`}><span class="address-status${entry.failure ? " is-failed" : entry.running ? " is-running" : ""}" aria-label="${entry.failure ? "Unavailable" : entry.running ? "Running" : "Not running"}"></span>${icon}<span>${escapeHTML(entry.name)}${entry.failure ? " — Unavailable" : ""}${isThisPage ? ' <small class="this-page-label">This page</small>' : ""}</span></a><button class="endpoint-menu" type="button" ${menu} aria-label="Actions for ${escapeHTML(entry.name)}" aria-haspopup="dialog">•••</button></div>`;
  }

  function endpointGroupID(entry) {
    return entry.workspace ? `container:${entry.workspace.id}${entry.workspace.runtimeKind === "wsl" ? ":" + (entry.app.userName || "workspace") : ""}` : entry.item.backend.serviceScope === "system" ? "root" : "user";
  }

  function groupPinKeys(id, entries) {
    return state.groupPins[id] ?? ["org.outershell.Files", "org.outershell.Top", "org.outershell.image.opencode"].map(serviceID => entries.find(entry => entry.serviceID === serviceID)?.key).filter(Boolean);
  }

  function pinMenuHTML(key, allowReorder = false) {
    const entries = registeredAddresses();
    const entry = entries.find(entry => entry.key === key);
    if (!entry) return "";
    const id = endpointGroupID(entry);
    const keys = groupPinKeys(id, entries.filter(entry => endpointGroupID(entry) === id));
    const index = keys.indexOf(key);
    const button = (action, label) => `<button class="context-menu-item" type="button" data-action="${action}" data-endpoint-key="${escapeHTML(key)}" role="menuitem">${contextMenuGlyph(action === "toggle-group-pin" ? "◇" : action === "move-pin-earlier" ? "↑" : "↓")}<span>${label}</span></button>`;
    return button("toggle-group-pin", index < 0 ? "Pin to top" : "Unpin from top") + (allowReorder && index > 0 ? button("move-pin-earlier", "Move shortcut earlier") : "") + (allowReorder && index >= 0 && index < keys.length - 1 ? button("move-pin-later", "Move shortcut later") : "");
  }

  function changeGroupPin(key, action) {
    const entries = registeredAddresses();
    const entry = entries.find(entry => entry.key === key);
    if (!entry) return;
    const id = endpointGroupID(entry);
    const keys = [...groupPinKeys(id, entries.filter(entry => endpointGroupID(entry) === id))];
    const index = keys.indexOf(key);
    if (action === "toggle-group-pin") {
      if (index < 0) keys.push(key); else keys.splice(index, 1);
    } else {
      const to = index + (action === "move-pin-earlier" ? -1 : 1);
      if (index < 0 || to < 0 || to >= keys.length) return;
      keys.splice(index, 1); keys.splice(to, 0, key);
    }
    const pins = { ...state.groupPins, [id]: keys };
    if (!saveEndpointLayout(pins, state.endpointOrder)) return;
    closeDialog();
    render();
  }

  function saveEndpointLayout(pins, order) {
    return saveLayout({ ...currentLayout(), pins, order });
  }

  function moveCardEndpoint(groupID, key, area, beforeKey) {
    const entries = registeredAddresses().filter(entry => endpointGroupID(entry) === groupID);
    if (!entries.some(entry => entry.key === key)) return;
    const oldPins = groupPinKeys(groupID, entries);
    const pins = oldPins.filter(value => value !== key);
    const ranks = new Map((state.endpointOrder[groupID] || []).map((value, index) => [value, index]));
    const list = entries.filter(entry => entry.key !== key && !oldPins.includes(entry.key)).sort((a, b) => (ranks.get(a.key) ?? Infinity) - (ranks.get(b.key) ?? Infinity)).map(entry => entry.key);
    const destination = area === "pins" ? pins : list;
    const index = destination.indexOf(beforeKey);
    destination.splice(index < 0 ? destination.length : index, 0, key);
    saveEndpointLayout({ ...state.groupPins, [groupID]: pins }, { ...state.endpointOrder, [groupID]: list });
  }

  function overviewShortcut(entry) {
    const item = entry.item || findItem(entry.key);
    const icon = listIconHTML(item);
    const target = entry.item ? navigationURL(entry.item.frontend) : safeSpaceAppNavigationURL(entry.workspace, entry.app);
    const identity = entry.item?.identity || entry.key;
    return `<div class="overview-shortcut-wrap" data-endpoint-key="${escapeHTML(entry.key)}"><a class="overview-shortcut" href="${escapeHTML(target)}" ${overviewLaunchAttributes(entry)} ${entry.item ? "" : `data-app-key="${escapeHTML(identity)}"`}>${icon}<span class="shortcut-title"><span class="address-status${entry.failure ? " is-failed" : entry.running ? " is-running" : ""}" aria-label="${entry.failure ? "Unavailable" : entry.running ? "Running" : "Not running"}"></span>${escapeHTML(entry.name)}${entry.failure ? " — Unavailable" : ""}</span></a></div>`;
  }

  function renderInitialLoading() {
    if (state.overviewReady) return false;
    if (state.layoutReady && state.backendsReady) {
      state.overviewReady = true;
      window.clearTimeout(state.initialLoadingTimer);
      elements.shell.setAttribute("aria-busy", "false");
      return false;
    }
    elements.overview.innerHTML = state.showInitialLoading
      ? '<p class="overview-empty" role="status">Loading…</p>' : "";
    state.overviewMarkup = "";
    return true;
  }

  function containerCommands(workspace) {
    const commands = (workspace.commands || []).map(command => ({
      id: `command:${command.id}`, name: command.displayName || command.id,
      text: command.containerCommand || command.shellCommand || "", internalText: command.internalCommand || "", iconURL: command.iconURL
    }));
    if (workspace.shellCommand) commands.unshift({ id: "terminal", name: "Terminal", text: workspace.shellCommand });
    return commands.filter(command => command.text.trim());
  }

  function containerCommandsHTML(workspace) {
    if (!workspace) return "";
    const commands = containerCommands(workspace);
    if (!commands.length) return "";
    const disabled = !safeSpaceIsRunning(workspace) || containerPreparing(workspace);
    return `<section class="container-commands" aria-label="Command line tools"><h3>Command line tools</h3>${commands.map(command => `<button type="button" class="container-command" data-action="show-container-command" data-safe-space-id="${escapeHTML(workspace.id)}" data-command-id="${escapeHTML(command.id)}" aria-label="Copy command for ${escapeHTML(command.name)}" aria-haspopup="dialog" ${disabled ? 'disabled title="Start the container to use this tool"' : ""}><span class="address-status" aria-hidden="true"></span><span class="container-command-icon" aria-hidden="true">${command.iconURL ? `<img src="${escapeHTML(command.iconURL)}" alt="">` : '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round"><rect x="2.5" y="4" width="19" height="16" rx="3"/><path d="m6.5 9 3 3-3 3m6 0h5"/></svg>'}</span><span>${escapeHTML(command.name)}</span><span class="container-command-copy" aria-hidden="true">⧉</span></button>`).join("")}</section>`;
  }

  function shellQuotedArgument(value) {
    return /^[a-zA-Z0-9@%_+=:,./-]+$/.test(value) ? value : "'" + value.replace(/'/g, "'\\''") + "'";
  }

  function containerCommandVariants(command) {
    const variants = [];
    const source = window.outerLoop?.sessionContext?.sshCommandArguments;
    const suffix = command.internalText ? " and internal command" : "";
    if (Array.isArray(source) && source.length && source.every(value => typeof value === "string")) {
      const args = [...source];
      if (!args.includes("-t") && !args.includes("-tt")) args.splice(1, 0, "-t");
      args.push(command.text);
      variants.push({ name: command.internalText ? "SSH command, container command, and internal command" : "SSH command and container command", text: args.map(shellQuotedArgument).join(" "), hint: "Run in a terminal on your computer." });
    }
    variants.push({ name: `Container command${suffix}`, text: command.text, hint: "Run in a terminal on the server." });
    if (command.internalText) variants.push({ name: "Internal command", text: command.internalText, hint: "Run in a terminal inside the container." });
    return variants;
  }

  function openContainerCommand(workspaceID, commandID, point = null) {
    const workspace = findSafeSpace(workspaceID);
    if (!workspace || !safeSpaceIsRunning(workspace)) return;
    const command = containerCommands(workspace).find(command => command.id === commandID);
    if (!command) return;
    const variants = containerCommandVariants(command);
    const dialog = showContextMenu(variants.map((variant, index) => {
      const label = variant.name.startsWith("SSH") ? "Copy SSH command"
        : variant.name.startsWith("Internal") ? "Copy internal command" : "Copy container command";
      return `<button type="button" class="context-menu-item" role="menuitem" data-copy-variant="${index}" title="${escapeHTML(variant.name)}">${contextMenuGlyph("⧉")}<span>${label}</span></button>`;
    }).join(""), point, command.name);
    dialog.querySelectorAll("[data-copy-variant]").forEach(button => {
      button.addEventListener("click", () => {
        const variant = variants[Number(button.dataset.copyVariant)];
        if (!variant) return;
        closeDialog();
        copyText(variant.text);
      });
    });
  }

  const agentExamples = [
    "I already have [tool] installed. Please register it with Outer Shell.",
    'Please run TensorBoard for my "experiment1" experiment and add it to Outer Shell.',
    "Please install JupyterLab so I can write and run Python notebooks, and add it to Outer Shell."
  ];

  function addContainer() {
    return state.addWorkspace && state.addWorkspace.runtimeKind !== "wsl" ? state.addWorkspace : null;
  }

  function agentConnection() {
    const workspace = state.addWorkspace;
    if (workspace) {
      const terminal = containerCommands(workspace).find(command => command.id === "terminal");
      const variant = terminal && containerCommandVariants(terminal)[0];
      return { command: variant?.text || "", local: !variant?.name.startsWith("SSH"), workspace };
    }
    const args = window.outerLoop?.sessionContext?.sshCommandArguments;
    if (Array.isArray(args) && args.length && args.every(arg => typeof arg === "string")) {
      return { command: args.map(shellQuotedArgument).join(" "), local: false };
    }
    return { command: "", local: Boolean(window.outerLoop?.sessionContext) };
  }

  function agentPrompt(request, connection = agentConnection()) {
    const destination = connection.workspace
      ? `${connection.local ? "On the server, enter" : "Connect to"} ${connection.workspace.runtimeKind === "wsl" ? "the Linux environment" : "the container"} ${connection.workspace.name}${connection.command ? ` using:\n${connection.command}` : "."}`
      : connection.command ? `Connect to this server using:\n${connection.command}`
      : connection.local ? "Work on this server, where Outer Shell is running."
      : "Work on the server running Outer Shell.";
    const containerContext = connection.workspace && connection.workspace.runtimeKind !== "wsl"
      ? "\n\nInstall and register apps inside this container. Read https://outershell.org/api/containers/ for instructions on publishing apps and preserving changes across rebuilds. Keep user data in persistent or mounted folders."
      : "";
    return `${destination}\n\nRead https://outershell.org/api/ for Outer Shell integration instructions.${containerContext}\n\n${request}`;
  }

  function openCodeItem() {
    return launcherItems().find(item => (/opencode/i.test(item.backend.serviceID) || /opencode/i.test(item.displayName)) && item.primary?.backend.serviceScope !== "system");
  }

  function codingAgentHTML() {
    const connection = agentConnection();
    const prompt = agentPrompt("[Your request here]", connection);
    const container = addContainer();
    const openCode = container?.apps?.find(app => /opencode/i.test(app.serviceID) || /opencode/i.test(app.displayName));
    const hosted = container
      ? `<aside class="agent-hosted"><h2>Want a coding agent here?</h2><p>OpenCode runs inside this container. Use it from your phone or computer to install tools and build things.</p>${openCode ? `<a class="primary-button" href="${escapeHTML(safeSpaceAppNavigationURL(container, openCode))}" data-action="launch-safe-space-app" data-safe-space-id="${escapeHTML(container.id)}" data-service-id="${escapeHTML(openCode.serviceID)}" data-frontend-key="${escapeHTML(containerFrontendKey(openCode))}">Open OpenCode →</a>` : managedContainer(container) ? `<button class="primary-button" type="button" data-action="configure-container" data-safe-space-id="${escapeHTML(container.id)}">Set up OpenCode →</button><small>In the Dockerfile editor, choose Add OpenCode, then Save &amp; rebuild.</small>` : "<p>Ask your coding agent to install OpenCode in this container.</p>"}<small>Bring your model provider account or API key.</small></aside>`
      : state.addWorkspace ? "" : `<aside class="agent-hosted"><h2>Want a coding agent here?</h2><p>OpenCode runs on this server. Use it from your phone or computer to install tools and build things.</p><div class="agent-actions"><button class="primary-button" data-action="setup-opencode" type="button">${openCodeItem() ? "Open OpenCode" : "Set up OpenCode"} <span aria-hidden="true">→</span></button><button class="secondary-button" data-action="setup-container-opencode" type="button">Set up in a container</button></div><small>Bring your model provider account or API key.</small></aside>`;
    return `<div class="coding-agent-panel">
      <div class="customize-intro"><h3>${container ? "Edit your Dockerfile with a coding agent" : "Add something with a coding agent"}</h3><p>${container ? "Describe the tools and apps you want. Your coding agent can update the Dockerfile and apply changes in the running container, so they’re available now and survive a rebuild." : "Notebooks, experiments, dashboards, your own apps. Tell a coding agent what you’d like to use here."}</p></div>
      <div class="agent-options">
        <section class="agent-own agent-guide" aria-labelledby="own-agent-title"><h2 id="own-agent-title">Use your coding agent</h2><p>Paste these details into your agent, then add your request.</p>
          ${!connection.command && !connection.local ? '<p class="field-note">Outer Loop includes your SSH command here automatically.</p>' : ""}
          <pre>${escapeHTML(prompt).replace(/https:\/\/outershell\.org\/api\/(?:containers\/)?/g, url => `<a href="${url}" target="_blank" rel="noopener noreferrer">${url}</a>`)}</pre>
          <button class="secondary-button" type="button" data-action="copy-agent-prompt">Copy prompt</button>
          <div class="customize-examples"><h2>A few things you could ask</h2>${agentExamples.map((example, index) => `<button type="button" data-action="example-prompt" data-example="${index}" aria-label="${escapeHTML(`Copy example: ${example}`)}"><span>${escapeHTML(example)}</span><span class="example-copy" aria-hidden="true">Copy</span></button>`).join("")}</div>
        </section>
        ${hosted}
      </div>
    </div>`;
  }

  function windowsOpenCodeCommand(script, port) {
    const bytes = new TextEncoder().encode(script);
    const encoded = btoa(Array.from(bytes, byte => String.fromCharCode(byte)).join(""));
    const chunks = encoded.match(/.{1,1200}/g) || [];
    return [
      'set "OC_SCRIPT=%LOCALAPPDATA%\\outershell-opencode-start.ps1"',
      ...chunks.map((chunk, index) => `${index ? ">>" : ">"} "%OC_SCRIPT%.b64" echo ${chunk}`),
      `powershell.exe -NoProfile -Command "[IO.File]::WriteAllBytes($env:OC_SCRIPT,[Convert]::FromBase64String([IO.File]::ReadAllText($env:OC_SCRIPT+'.b64')))"`,
      'if errorlevel 1 exit /b 1',
      `powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%OC_SCRIPT%" -Port ${port}`,
      'exit /b %errorlevel%'
    ].join("\n");
  }

  const containerAppCatalog = [
    { id: "files", name: "Files", summary: "Browse and manage files in this container.", marker: "org.outershell.Files" },
    { id: "plaintext", name: "Plaintext", summary: "Read and edit text files.", marker: "org.outershell.Plaintext" },
    { id: "top", name: "Top", summary: "Inspect processes running in this container.", marker: "org.outershell.Top" },
    { id: "opencode", name: "OpenCode", summary: "Use a coding agent inside this container.", marker: "/usr/local/bin/outershell-opencode" }
  ];

  async function containerAppFragment(id, allowAllActions = false) {
    const app = containerAppCatalog.find(app => app.id === id);
    if (!app) throw new Error("Unknown container app.");
    if (id === "opencode") return openCodeContainerFragment(allowAllActions);
    const template = `USER root
RUN <<'OUTERSHELL_BUNDLED_APP'
set -eu
export DEBIAN_FRONTEND=noninteractive
/usr/bin/apt-get update
/usr/bin/apt-get install -y --no-install-recommends ca-certificates curl
case "$(/usr/bin/uname -m)" in
    aarch64|arm64) platform=linux-aarch64 ; architecture=aarch64 ;;
    x86_64|amd64) platform=linux-x86_64 ; architecture=x86_64 ;;
    *) /usr/bin/printf 'Unsupported architecture.\\n' >&2; exit 1 ;;
esac
archive=/tmp/__APP_NAME__.tar.gz
/usr/bin/curl --fail --location --retry 4 --retry-delay 2 \\
    --output "\${archive}" \\
    "https://outershell.org/outer-shell/dev/apps/__APP_NAME__/\${platform}.tar.gz"
/bin/rm -rf /var/lib/outershell/apps/__SERVICE_ID__
/bin/mkdir -p /etc/outershell/apps.d /var/lib/outershell/apps/__SERVICE_ID__ "$(/usr/bin/dirname /var/lib/outershell/services/__SERVICE_ID__.outerservice)"
/bin/tar -xzf "\${archive}" --strip-components=1 -C /var/lib/outershell/apps/__SERVICE_ID__
/bin/chmod 0755 /var/lib/outershell/apps/__SERVICE_ID__/RemoteLinuxBinaries/\${architecture}/__APP_NAME__Backend
/usr/bin/touch /var/lib/outershell/apps/__SERVICE_ID__/backend.log
/bin/cat >/var/lib/outershell/services/__SERVICE_ID__.outerservice <<OUTERSHELL_SERVICE
[Service]
Format=1
Name=__APP_NAME__
Executable=/var/lib/outershell/apps/__SERVICE_ID__/RemoteLinuxBinaries/\${architecture}/__APP_NAME__Backend
Argument=--label
Argument=__SERVICE_ID__
Argument=--socket-path
Argument=/run/user/0/__SERVICE_ID__
Argument=--bundles-dir
Argument=/var/lib/outershell/apps/__SERVICE_ID__/bundles
Argument=--icon-file
Argument=/var/lib/outershell/apps/__SERVICE_ID__/app-icon.png
WorkingDirectory=/var/lib/outershell/apps/__SERVICE_ID__
Environment=HOME=/root
Environment=USER=root
Environment=LOGNAME=root
Environment=OUTERSHELL_SERVICE_STATE_DIR=/var/lib/outershell/apps/__SERVICE_ID__
Environment=OUTERSHELLD_API_SOCKET=/run/user/0/outershelld-api
EnvironmentPolicy=clean
Start=socket
Restart=on-failure
LogPath=/var/lib/outershell/apps/__SERVICE_ID__/backend.log

[Socket.http]
Type=unix
Path=/run/user/0/__SERVICE_ID__
Mode=0600
Backlog=64
OUTERSHELL_SERVICE
/bin/cat >/etc/outershell/apps.d/__SERVICE_ID__.sh <<'OUTERSHELL_REGISTRATION'
#!/bin/sh
set -eu
export HOME=/root USER=root LOGNAME=root XDG_RUNTIME_DIR=/run/user/0
export OUTERSHELLD_API_SOCKET=/run/user/0/outershelld-api
outerctl=/usr/local/bin/outerctl
"\${outerctl}" backend upsert --backend __SERVICE_ID__ --name "__APP_NAME__" --service-file /var/lib/outershell/services/__SERVICE_ID__.outerservice --outershell-owns true
"\${outerctl}" app upsert --backend __SERVICE_ID__ --frontend-id __SERVICE_ID__:main --socket-path /run/user/0/__SERVICE_ID__ --name "__APP_NAME__" --url / --icon-path /var/lib/outershell/apps/__SERVICE_ID__/app-icon.png
"\${outerctl}" log add --backend __SERVICE_ID__ --path /var/lib/outershell/apps/__SERVICE_ID__/backend.log
__OPENER__
OUTERSHELL_REGISTRATION
/bin/chmod 0755 /etc/outershell/apps.d/__SERVICE_ID__.sh
/bin/rm -f "\${archive}"
/bin/rm -rf /var/lib/apt/lists/*
OUTERSHELL_BUNDLED_APP
`;
    return template.replaceAll("__APP_NAME__", app.name)
      .replaceAll("__SERVICE_ID__", app.marker)
      .replaceAll("__OPENER__", id === "plaintext" ? '"${outerctl}" opener upsert --backend org.outershell.Plaintext --content-type public.text --url-template \'?file={file}\' --rank 0 --capabilities view,edit' : "");
  }

  async function containerAppDraft(dockerfile, id) {
    const app = containerAppCatalog.find(app => app.id === id);
    if (!app) throw new Error("Unknown container app.");
    if (containerAppIncluded(dockerfile, app)) {
      return { dockerfile, message: `This Dockerfile already includes ${app.name}.` };
    }
    return {
      dockerfile: dockerfile.trimEnd() + "\n\n" + await containerAppFragment(id),
      message: `${app.name} added to this draft. Review the Dockerfile, then choose Save & rebuild to install it.`
    };
  }

  function containerAppIncluded(dockerfile, app) {
    const active = dockerfile.split("\n").filter(line => !line.trimStart().startsWith("#")).join("\n");
    return active.includes(app.marker) || active.includes(`ghcr.io/outergroup/outershell-app-${app.id}:`);
  }

  async function openContainerAdd(workspace) {
    state.addWorkspace = workspace;
    state.addScope = "user";
    state.containerCatalog = null;
    openAddDialog("agent");
    try {
      const result = await safeSpaceRequest("listDetails");
      if (state.addWorkspace !== workspace) return;
      const current = result.workspaces?.find(value => value.id === workspace.id);
      if (typeof current?.recipe?.containerfile !== "string") throw new Error("The container Dockerfile is not available yet.");
      const original = current.recipe.containerfile;
      state.containerCatalog = { id: workspace.id, original, dockerfile: original, selected: [], busy: false, message: "" };
      if (state.addTab === "catalog") openAddDialog("catalog");
    } catch (error) {
      if (state.addWorkspace !== workspace) return;
      state.containerCatalog = { id: workspace.id, error: error.message || String(error) };
      if (state.addTab === "catalog") openAddDialog("catalog");
    }
  }

  function containerCatalogHTML(container) {
    const draft = state.containerCatalog;
    if (!draft || draft.id !== container.id) return '<p role="status">Loading Dockerfile…</p>';
    if (draft.error) return `<p role="alert">${escapeHTML(draft.error)}</p><button type="button" class="secondary-button" data-action="reload-container-catalog">Retry</button>`;
    return `<p>Add the apps you want, then save and rebuild to install them. These setups require Debian or Ubuntu with Outer Shell support.</p><div class="add-catalog">${containerAppCatalog.map(app => {
      const included = containerAppIncluded(draft.original, app);
      const selected = draft.selected.includes(app.id);
      return `<article class="add-catalog-app ${included || selected ? "installed" : ""}"><div class="app-icon has-image" aria-hidden="true"><img src="/web/icons/${app.id === "opencode" ? "opencode.svg" : `org.outershell.${app.name}.png`}" alt=""></div><h3>${app.name}</h3><p>${app.summary}</p>${included ? '<span class="catalog-installed">In Dockerfile</span>' : `<button class="secondary-button" type="button" data-action="add-container-app" data-catalog-id="${app.id}" aria-pressed="${selected}" ${draft.busy ? "disabled" : ""}>${selected ? "Added · Undo" : "Add to Dockerfile"}</button>`}</article>`;
    }).join("")}</div>
    <p class="form-message" role="status">${escapeHTML(draft.message || (draft.selected.length ? `${draft.selected.length} app${draft.selected.length === 1 ? "" : "s"} added. Changes are not saved yet.` : ""))}</p>
    ${draft.selected.length ? `<details class="catalog-dockerfile"><summary>Review Dockerfile</summary><pre>${escapeHTML(draft.dockerfile)}</pre></details>` : ""}
    <footer class="dialog-footer"><button class="secondary-button" type="button" data-action="close-dialog">Cancel</button><button class="primary-button" type="button" data-action="save-container-catalog" ${draft.busy || !draft.selected.length ? "disabled" : ""}>${draft.busy ? "Working…" : "Save &amp; rebuild"}</button></footer>`;
  }

  async function toggleContainerCatalogApp(id) {
    const draft = state.containerCatalog;
    const app = containerAppCatalog.find(app => app.id === id);
    if (!draft || draft.error || draft.busy || !app || containerAppIncluded(draft.original, app)) return;
    draft.busy = true;
    draft.message = "";
    openAddDialog("catalog");
    try {
      const selected = draft.selected.includes(id) ? draft.selected.filter(value => value !== id) : [...draft.selected, id];
      let dockerfile = draft.original;
      for (const selectedID of selected) dockerfile = (await containerAppDraft(dockerfile, selectedID)).dockerfile;
      draft.selected = selected;
      draft.dockerfile = dockerfile;
    } catch (error) { draft.message = error.message || String(error); }
    finally {
      draft.busy = false;
      if (state.containerCatalog === draft && state.addTab === "catalog") openAddDialog("catalog");
    }
  }

  async function saveContainerCatalog() {
    const draft = state.containerCatalog;
    if (!draft || draft.error || draft.busy || !draft.selected.length) return;
    draft.busy = true;
    draft.message = "Saving…";
    openAddDialog("catalog");
    try {
      const result = await safeSpaceRequest("listDetails");
      const current = result.workspaces?.find(value => value.id === draft.id)?.recipe?.containerfile;
      if (current !== draft.original) throw new Error("The Dockerfile changed elsewhere. Close this dialog and reopen the catalog to load the latest version.");
      await containerOperation("updateDockerfile", { workspaceID: draft.id, command: draft.dockerfile, dockerfile: draft.dockerfile });
      draft.original = draft.dockerfile;
      draft.message = "Starting rebuild…";
      await containerOperation("rebuildRecipe", { workspaceID: draft.id });
      if (state.containerCatalog === draft) closeDialog();
      await refreshSafeSpaces({ quiet: true });
    } catch (error) { draft.message = error.message || String(error); }
    finally {
      draft.busy = false;
      if (state.containerCatalog === draft && state.addTab === "catalog") openAddDialog("catalog");
    }
  }

  async function openCodeContainerFragment(allowAllActions = false) {
    const load = async name => {
      const response = await fetch(`/web/${name}`);
      if (!response.ok) throw new Error("Could not load the OpenCode container setup. Refresh Outer Shell and try again.");
      return response.text();
    };
    const [template, instructions, reference, skill] = await Promise.all([
      load("opencode-container.Dockerfile"), load("container-agent.md"), load("container-outerctl.md"), load("outer-shell-skill.md")
    ]);
    const encode = async text => {
      const compressed = new Blob([text]).stream().pipeThrough(new CompressionStream("gzip"));
      const bytes = new Uint8Array(await new Response(compressed).arrayBuffer());
      return btoa(Array.from(bytes, byte => String.fromCharCode(byte)).join(""));
    };
    const encoded = await Promise.all([instructions, reference, skill].map(encode));
    const config = { instructions: ["/usr/local/share/outershell/container-agent.md"] };
    if (allowAllActions) config.permission = "allow";
    return template.replace("__OPENCODE_CONFIG__", () => JSON.stringify(config))
      .replace("__CONTAINER_AGENT__", () => encoded[0])
      .replace("__CONTAINER_OUTERCTL__", () => encoded[1])
      .replace("__OUTERSHELL_SKILL__", () => encoded[2]);
  }

  async function pinInstalledOpenCode(item) {
    while (state.layoutSaving) await delay(50);
    state.layoutAbort?.abort();
    state.layoutSaving = true;
    try {
      for (let attempt = 0; attempt < 3; attempt += 1) {
        const current = await fetchLayout();
        const key = hostBookmarkKey(item);
        const entries = registeredAddresses().filter(entry => endpointGroupID(entry) === "user");
        const defaults = ["org.outershell.Files", "org.outershell.Top"].map(serviceID => entries.find(entry => entry.serviceID === serviceID)?.key).filter(Boolean);
        const pins = current.layout.pins.user ?? defaults;
        if (!pins.includes(key)) {
          try {
            const layout = { ...current.layout, pins: { ...current.layout.pins, user: [...pins, key] } };
            const saved = await writeLayout(layout, current.revision);
            applyLayout(saved.layout);
            state.layoutRevision = saved.revision;
            state.layoutReady = true;
            return;
          } catch (error) {
            if (error.conflict && attempt < 2) continue;
            throw error;
          }
        }
        applyLayout(current.layout);
        state.layoutRevision = current.revision;
        state.layoutReady = true;
        return;
      }
    } finally { state.layoutSaving = false; render(); }
  }

  async function openOpenCodeSetup() {
    const existing = openCodeItem();
    if (existing) { await launch(existing.identity); return; }
    const windows = state.hostPlatform === "windows";
    const dialog = openDialog(`<header class="dialog-header"><div class="dialog-title-wrap"><h2 id="dialog-title">Your coding agent, on this server</h2><p>Set up OpenCode</p></div><button class="dialog-close" type="button" data-action="close-dialog" aria-label="Close">×</button></header>
      <div class="dialog-body agent-guide"><p><button class="secondary-button" type="button" data-opencode-container>Set up in a new container</button></p><p>Or install directly on this server:</p><ol class="assistant-steps"><li><strong>Install OpenCode</strong><span>Outer Shell downloads it and adds it to your apps. No terminal needed.</span></li><li><strong>Connect a model provider</strong><span>Sign in or add your API key in OpenCode. Your provider’s pricing applies.</span></li><li><strong>Ask for what you want</strong><span>OpenCode comes with instructions for adding apps to Outer Shell.</span></li></ol>
      <details><summary>Connection settings</summary><label for="opencode-port">Port</label><input id="opencode-port" type="number" min="1024" max="65535" value="4096"><p class="field-note">Available only on this server and through your Outer Loop connection.</p></details>
      <p class="form-message" role="status" aria-live="polite"></p></div>
      <footer class="dialog-footer"><button class="secondary-button" type="button" data-action="agent-guide">Use my own agent</button><button class="primary-button" type="button" data-install-opencode>Install OpenCode</button></footer>`);
    dialog.querySelector("[data-opencode-container]").addEventListener("click", () => openCreateContainer(true));
    dialog.querySelector("[data-install-opencode]")?.addEventListener("click", async event => {
      const button = event.currentTarget;
      if (button.disabled) return;
      const message = dialog.querySelector(".form-message");
      const port = Number(dialog.querySelector("#opencode-port").value);
      if (!Number.isInteger(port) || port < 1024 || port > 65535) { message.textContent = "Choose a port between 1024 and 65535."; return; }
      button.disabled = true;
      button.textContent = "Installing…";
      message.textContent = "Downloading OpenCode and preparing your coding agent. This can take a few minutes. You can close this dialog; installation will continue.";
      try {
        const load = async path => { const response = await fetch(path); if (!response.ok) throw new Error("Could not load the OpenCode installer. Refresh Outer Shell and try again."); return response.text(); };
        const [script, skill, admin] = await Promise.all([load(windows ? "/web/opencode-start.ps1" : "/web/opencode-start.sh"), load("/web/outer-shell-skill.md"), load("/web/server-admin.md")]);
        const prepared = script.replace("__OUTERSHELL_SKILL__", () => skill.trimEnd()).replace("__OUTERSHELL_ADMIN__", () => admin.trimEnd());
        const command = windows ? windowsOpenCodeCommand(prepared, port) : prepared;
        if (new TextEncoder().encode(command).length >= (windows ? 262144 : 32768)) throw new Error("OpenCode installer exceeds the command size limit.");
        const query = new URLSearchParams({ recipe: "command-port", name: "OpenCode", identifier: windows ? "opencode" : "org.outershell.OpenCode", command, workdir: "~", scope: "user", frontendTransport: "port", port: String(port) });
        const { buffer } = await requestBuffer("/api/create", { method: "POST", headers: { "Content-Type": "application/x-www-form-urlencoded" }, body: query });
        const result = decodeAction(buffer);
        if (!result.ok) throw new Error(result.message || "Could not install OpenCode.");
        let pinned = false;
        for (let attempt = 0; attempt < 180; attempt += 1) {
          await refreshBackends({ quiet: true });
          const item = openCodeItem();
          if (item && !pinned) {
            await pinInstalledOpenCode(item);
            pinned = true;
          }
          if (item?.primary && endpointRunning(item.primary) && /\/session\/ses_/.test(item.primary.frontend?.url || item.primary.frontend?.path || "")) {
            if (!dialog.isConnected) { toast("OpenCode is ready. Open it from your apps."); return; }
            message.textContent = "OpenCode is ready. Connect your model provider, then ask it to add your first app.";
            button.disabled = false; button.textContent = "Open OpenCode";
            button.replaceWith(Object.assign(document.createElement("button"), { type: "button", className: "primary-button", textContent: "Open OpenCode", onclick: () => launch(item.identity).catch(error => toast(error.message || String(error))) }));
            return;
          }
          const failure = item?.primary && endpointFailure(item.primary, "OpenCode");
          if (failure) throw new Error(`${failure} Open its app menu and check the logs for details.`);
          await delay(2000);
        }
        throw new Error("OpenCode is still getting ready. Check its logs from the app menu, then open it when ready.");
      } catch (error) {
        message.textContent = error.message || String(error);
        await refreshBackends({ quiet: true });
        button.disabled = false; button.textContent = "Check OpenCode";
        button.replaceWith(button.cloneNode(true));
        const retry = dialog.querySelector("[data-install-opencode]");
        retry?.addEventListener("click", () => { closeDialog(); openOpenCodeSetup().catch(error => toast(error.message || String(error))); });
      }
    });
  }

  function renderOverview(entries) {
    if (state.groupDrag || state.endpointDrag) return;
    if (renderInitialLoading()) return;
    const sessionUsername = window.outerLoop?.sessionContext?.username;
    const userName = typeof sessionUsername === "string" ? sessionUsername.trim() : "";
    const isWindows = state.hostPlatform === "windows";
    const groups = [
      { id: "user", name: userName || "Your user", note: "User", entries: entries.filter(entry => entry.item && (isWindows || entry.item.backend.serviceScope !== "system")) },
      ...(!isWindows ? [{ id: "root", name: "root", note: "Administrator", entries: entries.filter(entry => entry.item && entry.item.backend.serviceScope === "system") }] : []),
      ...state.safeSpaces.flatMap(workspace => {
        const apps = entries.filter(entry => entry.workspace?.id === workspace.id);
        if (workspace.runtimeKind !== "wsl") return [{ id: `container:${workspace.id}`, name: workspace.name || "Container", note: `${workspace.runtimeKind === "wsl" ? "WSL" : "Container"} · ${safeSpaceState(workspace)}`, workspace, entries: apps }];
        return [...new Set([...apps.map(entry => entry.app.userName || "workspace"), workspace.userName || "workspace", "root"])].sort((a, b) => (a === "root") - (b === "root") || a.localeCompare(b)).map(user => ({
          id: `container:${workspace.id}:${user}`, name: `${workspace.name} / ${user}`, note: `WSL · ${user === "root" ? "Administrator" : "User"} · ${safeSpaceState(workspace)}`, workspace,
          entries: apps.filter(entry => (entry.app.userName || "workspace") === user)
        }));
      })
    ];
    const ranks = new Map(state.groupOrder.map((id, index) => [id, index]));
    groups.sort((a, b) => (ranks.get(a.id) ?? Infinity) - (ranks.get(b.id) ?? Infinity));
    const markup = groups.map(group => {
      const shield = group.shield ? '<svg aria-hidden="true" viewBox="0 0 16 18" width="14" height="16" style="vertical-align:middle;margin-right:6px;color:#2678d4"><path d="M8 1L14 3V8C14 12 11 15 8 17C5 15 2 12 2 8V3Z" fill="none" stroke="currentColor"/><path d="M8 1V17C11 15 14 12 14 8V3Z" fill="currentColor"/></svg>' : "";
      const workspace = group.workspace;
      const id = escapeHTML(workspace?.id || "");
      const preparing = workspace && containerPreparing(workspace);
      const pins = groupPinKeys(group.id, group.entries);
      const order = state.endpointOrder[group.id] || [];
      const ranks = new Map(order.map((key, index) => [key, index]));
      const listed = group.entries.filter(entry => !pins.includes(entry.key)).sort((a, b) => (ranks.get(a.key) ?? Infinity) - (ranks.get(b.key) ?? Infinity));
      const controls = workspace ? `<div class="overview-controls">${!safeSpaceIsRunning(workspace) ? `<button type="button" data-action="start-safe-space" data-safe-space-id="${id}" ${preparing ? "disabled" : ""}>${preparing ? "Starting…" : "Start"}</button>` : ""}<button type="button" data-action="edit-container" data-safe-space-id="${id}" aria-label="Manage ${escapeHTML(group.name)}">Manage…</button></div>` : "";
      const addMore = workspace?.runtimeKind === "wsl"
        ? `<button class="overview-add-more" type="button" data-action="open-wsl-add" data-safe-space-id="${id}" data-scope="${group.id.endsWith(":root") ? "system" : "user"}" >Add more…</button>`
        : workspace
        ? `<button class="overview-add-more" type="button" data-action="open-container-add" data-safe-space-id="${id}" ${preparing ? "disabled" : ""}>Add more…</button>`
        : `<button class="overview-add-more" type="button" data-action="open-add" data-scope="${group.id === "root" ? "system" : "user"}">Add more…</button>`;
      return `<section class="overview-identity" data-group-id="${escapeHTML(group.id)}"><header class="overview-group-handle" tabindex="0" aria-label="Reorder ${escapeHTML(group.name)}${group.shield ? ", Administrator" : ""}. Hold and drag, or use arrow keys." title="Hold and drag to reorder" data-group-handle><div class="overview-group-heading"><h2>${escapeHTML(group.name)}</h2><p>${shield}${escapeHTML(group.note)}</p></div>${controls}</header><div class="overview-shortcuts" data-endpoint-area="pins">${pins.map(key => group.entries.find(entry => entry.key === key)).filter(Boolean).map(overviewShortcut).join("")}</div><div class="overview-list" data-endpoint-area="list">${listed.map(overviewEndpoint).join("") || `<p class="overview-empty">${state.loading || (workspace && state.safeSpacesLoading) ? "Loading…" : group.entries.length ? "" : "No registered endpoints."}</p>`}</div>${containerCommandsHTML(workspace)}${workspace?.buildProgress ? `<details class="container-build-progress"><summary>${escapeHTML(workspace.buildProgress.detail || workspace.buildProgress.phase)}</summary><pre>${escapeHTML(workspace.buildProgress.log || "")}</pre></details>` : ""}${addMore}</section>`;
    }).join("") + `<button class="overview-add-container" type="button" data-action="create-container"><span aria-hidden="true">+</span>Add container (beta)…</button><button class="overview-add-container" type="button" data-action="import-container">Import container…</button>${state.providers.some(provider => provider.capabilities?.supportsStorageCleanup) ? `<button class="overview-add-container" type="button" data-action="container-storage">Container storage…</button>` : ""}`;
    if (state.overviewMarkup !== markup) {
      elements.overview.innerHTML = markup;
      state.overviewMarkup = markup;
    }
  }

  function saveGroupOrder(groups) {
    return saveLayout({ ...currentLayout(), groups });
  }

  function finishGroupDrag(commit) {
    const drag = state.groupDrag;
    if (!drag) return;
    state.groupDrag = null;
    window.clearTimeout(drag.armTimer);
    drag.handle.classList.remove("is-armed");
    if (drag.pointerId !== undefined && drag.handle.hasPointerCapture(drag.pointerId)) drag.handle.releasePointerCapture(drag.pointerId);
    if (commit && drag.moved && drag.target) {
      const order = [...elements.overview.querySelectorAll("[data-group-id]")].map(card => card.dataset.groupId);
      const from = order.indexOf(drag.id), to = order.indexOf(drag.target);
      if (from !== -1 && to !== -1) { order.splice(from, 1); order.splice(to, 0, drag.id); saveGroupOrder(order); }
    }
    elements.overview.querySelectorAll(".is-group-dragging, .group-drop-before, .group-drop-after").forEach(card => card.classList.remove("is-group-dragging", "group-drop-before", "group-drop-after"));
    renderOverview(registeredAddresses());
  }

  function renderServerOverview() {
    renderOverview(registeredAddresses());
  }

  function allEndpointItems() {
    const containers = state.safeSpaces.flatMap(workspace => (workspace.apps || []).filter(app => workspace.runtimeKind !== "wsl" || app.serviceID !== "org.outershell.OuterShell").map(app => ({
      identity: containerBookmarkKey(workspace, app), workspace, app,
      displayName: state.endpointNames[containerBookmarkKey(workspace, app)] || app.displayName || app.serviceID,
      subtitle: workspace.name || "Container",
      backend: { serviceID: app.serviceID },
      frontend: {}
    })));
    return [...launcherItems(true), ...containers].sort((a, b) => a.displayName.localeCompare(b.displayName, undefined, { sensitivity: "base" }));
  }

  function render() {
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
    const description = parts.filter(Boolean).join(" · ") || "Container";
    return workspace.ownsContainer === false || workspace.managementKind === "attached"
      ? `Attached · ${description}`
      : description;
  }

  function safeSpacePathAndQuery(app) {
    return pathAndQuery({ url: app.url || "", socketPath: app.socketPath || "" });
  }

  function safeSpaceAppURL(app) {
    const path = safeSpacePathAndQuery(app);
    if (Number(app.publishedPort) > 0) return `http://127.0.0.1:${Number(app.publishedPort)}${path}`;
    const socket = String(app.externalSocketPath || "").trim();
    return socket ? `http+${socket.startsWith("\\\\.\\pipe\\") ? "pipe" : "unix"}://${encodeURIComponent(socket)}${path}` : "#";
  }

  function safeSpaceAppNavigationURL(workspace, app) {
    const target = safeSpaceAppURL(app);
    if (target === "#") return target;
    if (window.outerLoopPageIconObservationSupported === true && !app.iconData && !app.iconURL && app.iconObservationToken) {
      const callback = new URL("/api/icon-observation", window.location.href);
      callback.search = new URLSearchParams({ token: app.iconObservationToken }).toString();
      return `outerloop://observe-page-icon?${new URLSearchParams({ url: target, callback: callback.href }).toString().replace(/\+/g, "%20")}`;
    }
    return target;
  }

  function safeSpaceAppIsReady(workspace, app) {
    return safeSpaceIsRunning(workspace) && app.isRunning === true && safeSpaceAppURL(app) !== "#";
  }

  function renderSafeSpaces() {
    renderServerOverview();
  }

  function slug(value) {
    return String(value).toLocaleLowerCase().replace(/[^a-z0-9]+/g, "-").replace(/^-|-$/g, "") || "apps";
  }

  function renameEndpointMenuHTML(item) {
    return `<button class="context-menu-item" type="button" data-action="rename-endpoint" data-app-key="${escapeHTML(item.identity)}" role="menuitem">${contextMenuGlyph("✎")}<span>Rename endpoint…</span></button>`;
  }

  function openRenameEndpoint(item) {
    const key = hostBookmarkKey(item);
    const dialog = openDialog(`${containerDialogHeader("Rename endpoint")}<form><div class="dialog-body"><label class="field">Name<input name="name" required value="${escapeHTML(item.displayName)}" autocomplete="off"></label><p class="field-note">Saved on this server. Used in endpoint lists and pinned shortcuts.</p><p class="container-message" role="status"></p></div><footer class="dialog-footer"><button type="button" data-action="close-dialog">Cancel</button><button type="submit" class="primary-button">Rename</button></footer></form>`);
    const form = dialog.querySelector("form");
    form.elements.name.select();
    form.addEventListener("submit", event => {
      event.preventDefault();
      const name = form.elements.name.value.trim();
      const message = form.querySelector(".container-message");
      if (!name) { message.textContent = "Enter a name."; return; }
      const names = { ...state.endpointNames, [key]: name };
      if (!saveLayout({ ...currentLayout(), names })) return;
      closeDialog();
      render();
    });
  }

  function workspaceAPIPath(path) {
    return state.addWorkspace ? `/api/wsl/${state.addWorkspace.id}/${path.replace(/^\/api\//, "")}` : path;
  }

  async function openWorkspaceAdd(workspace, scope) {
    state.addWorkspace = workspace;
    state.addScope = scope;
    state.workspaceCatalog = [];
    state.recipes = [];
    try {
      const { buffer } = await requestBuffer(workspaceAPIPath("/api/backends?web=1"));
      if (state.addWorkspace !== workspace) return;
      const result = decodeBackends(buffer);
      if (result.error) throw new Error(result.error);
      state.workspaceCatalog = result.backends;
      openAddDialog("catalog");
    } catch (error) {
      state.addWorkspace = null;
      toast(error.message || String(error), true);
    }
  }

  async function requestBuffer(url, options = {}, acceptErrors = false) {
    if (/^\/api\/(control|create|recipes|file-picker)(\?|$)/.test(url)) url = workspaceAPIPath(url);
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

  async function safeSpaceRequest(operation, values = {}, signal) {
    const requestID = makeRequestID();
    const response = operation === "list"
      ? await fetch("/api/container-snapshot", { cache: "no-store", signal })
      : await fetch("/api/safe-spaces", {
      method: "POST",
      cache: "no-store",
      headers: { "Content-Type": "application/json" },
      signal,
      body: JSON.stringify({ requestID, operation: operation === "listDetails" ? "list" : operation, ...values })
    });
    if (!response.ok) {
      const text = await response.text();
      let detail = text.trim();
      try { detail = JSON.parse(text).error || detail; } catch (_) {}
      throw new Error(detail || `Outer Shell returned HTTP ${response.status}.`);
    }
    const result = await response.json();
    if (result.error) throw new Error(result.error);
    if (!signal?.aborted && typeof result.hostPlatform === "string") state.hostPlatform = result.hostPlatform;
    if (!signal?.aborted && Array.isArray(result.providers)) state.providers = result.providers;
    return result;
  }

  async function refreshSafeSpaces({ quiet = false } = {}) {
    if (state.stopped || state.suspended || state.safeSpacesRefreshing) return;
    const controller = new AbortController();
    state.refreshAbort.safeSpacesError = controller;
    state.safeSpacesRefreshing = true;
    if (!quiet) {
      state.safeSpacesLoading = true;
      renderSafeSpaces();
    }
    try {
      const result = await safeSpaceRequest("list", {}, controller.signal);
      if (state.refreshAbort.safeSpacesError !== controller) return;
      state.safeSpaces = Array.isArray(result.workspaces) ? result.workspaces : [];
      state.safeSpacesReady = true;
      clearRefreshFailure("safeSpacesError");
      state.safeSpacesError = "";
    } catch (error) {
      if (state.refreshAbort.safeSpacesError === controller) reportRefreshFailure("safeSpacesError", error, quiet);
    } finally {
      if (state.refreshAbort.safeSpacesError !== controller) return;
      delete state.refreshAbort.safeSpacesError;
      state.safeSpacesRefreshing = false;
      state.safeSpacesLoading = false;
      if (!state.stopped) { updateStatus(); renderSafeSpaces(); }
    }
  }

  function findSafeSpace(id) {
    return state.safeSpaces.find(workspace => workspace.id === id);
  }

  async function waitForSafeSpaceApp(workspaceID, serviceID, frontendKey) {
    for (let attempt = 0; attempt < 40; attempt += 1) {
      await refreshSafeSpaces({ quiet: true });
      const workspace = findSafeSpace(workspaceID);
      const app = findSafeSpaceApp(workspace, serviceID, frontendKey);
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

  async function launchSafeSpaceApp(workspaceID, serviceID, frontendKey) {
    let workspace = findSafeSpace(workspaceID);
    let app = findSafeSpaceApp(workspace, serviceID, frontendKey);
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
        app = findSafeSpaceApp(workspace, serviceID, frontendKey) || app;
      }
      if (app.isRunning !== true) await safeSpaceRequest("startApp", { workspaceID, serviceID, serviceScope: app.serviceScope });
      const ready = await waitForSafeSpaceApp(workspaceID, serviceID, frontendKey);
      window.location.assign(safeSpaceAppNavigationURL(ready.workspace, ready.app));
    } finally {
      state.safeSpaceBusy.delete(key);
      renderSafeSpaces();
    }
  }

  async function refreshBackends({ quiet = false } = {}) {
    if (state.stopped || state.suspended || state.refreshAbort.backendError) return;
    const controller = new AbortController();
    state.refreshAbort.backendError = controller;
    if (!quiet) {
      state.loading = true;
      render();
    }
    try {
      const { buffer } = await requestBuffer("/api/backends?web=1", { signal: controller.signal });
      if (state.refreshAbort.backendError !== controller) return;
      const result = decodeBackends(buffer);
      state.backends = result.backends;
      state.backendsReady = true;
      clearRefreshFailure("backendError");
      state.backendError = result.error;
      updateStatus();
    } catch (error) {
      if (state.refreshAbort.backendError === controller) reportRefreshFailure("backendError", error, quiet);
    } finally {
      if (state.refreshAbort.backendError !== controller) return;
      delete state.refreshAbort.backendError;
      state.loading = false;
      if (!state.stopped) render();
    }
  }

  function clearRefreshFailure(key) {
    window.clearTimeout(state.refreshErrorTimers[key]);
    delete state.refreshErrorTimers[key];
  }

  function reportRefreshFailure(key, error) {
    if (state.stopped || state.suspended || document.hidden || error.name === "AbortError" || state.refreshErrorTimers[key]) return;
    state.refreshErrorTimers[key] = window.setTimeout(() => {
      delete state.refreshErrorTimers[key];
      if (state.stopped || state.suspended || document.hidden) return;
      state[key] = "Unable to refresh right now. Reconnecting…";
      updateStatus();
    }, 10000);
  }

  function suspendRefreshes() {
    state.suspended = true;
    state.layoutAbort?.abort();
    state.layoutAbort = null;
    state.layoutFetching = false;
    state.eventAbort?.abort();
    for (const key of ["backendError", "safeSpacesError", "layoutError"]) {
      state.refreshAbort[key]?.abort();
      delete state.refreshAbort[key];
      clearRefreshFailure(key);
      state[key] = "";
    }
    state.safeSpacesRefreshing = false;
    updateStatus();
  }

  function resumeRefreshes() {
    if (document.hidden || !state.suspended || state.stopped) return;
    state.suspended = false;
    refreshLayout();
    refreshBackends({ quiet: true });
    refreshSafeSpaces({ quiet: true });
  }

  function showStatus(message = "") {
    elements.status.textContent = message;
    elements.status.hidden = !message;
  }

  function updateStatus() {
    if (!state.stopped) showStatus([...new Set([state.backendError, state.safeSpacesError, state.layoutError].filter(Boolean))].join("\n"));
  }

  function toast(message, isError = false) {
    if (!message || state.stopped) return;
    const node = document.createElement("div");
    node.className = `toast${isError ? " error" : ""}`;
    node.textContent = message;
    elements.toasts.append(node);
    window.setTimeout(() => node.remove(), 4200);
  }

  function findItem(identity) {
    return allEndpointItems().find(item => item.identity === identity);
  }

  function itemIdentityFromTarget(target) {
    if (!(target instanceof Element)) return "";
    return target.closest("[data-app-key]")?.dataset.appKey
      || target.closest("[data-drag-app-key]")?.dataset.dragAppKey || "";
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
      context: menuContext(event.target),
      pointerID: event.pointerId,
      x: event.clientX,
      y: event.clientY,
      timer: 0
    };
    press.timer = window.setTimeout(() => {
      if (state.longPress !== press) return;
      state.longPress = null;
      clearPressFeedback();
      state.suppressLaunchUntil = Date.now() + 900;
      finishEndpointDrag(false);
      navigator.vibrate?.(8);
      if (identity) openAppMenu(identity, { x: press.x, y: press.y }, press.context);
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

  function showLaunchFailure(item, endpoint, scope) {
    const key = escapeHTML(backendKey(endpoint.backend));
    openDialog(`<h2 id="dialog-title">Could not open ${escapeHTML(item.displayName)}</h2>
      <p>${escapeHTML(endpointFailure(endpoint, item.displayName))}</p>
      <div class="dialog-actions">
        <button type="button" data-action="retry-launch" data-app-key="${escapeHTML(item.identity)}" data-app-scope="${escapeHTML(scope)}">Retry</button>
        <button type="button" data-action="logs" data-backend-key="${key}">View Logs</button>
        <button type="button" data-action="close-dialog">Close</button>
      </div>`);
  }

  async function launch(identity, scope = "primary", retry = false) {
    let item = findItem(identity);
    if (!item) return;
    let endpoint = item[scope];
    if (!endpoint) throw new Error(`${item.displayName} is not available for this account.`);
    if (!retry && endpointFailure(endpoint, item.displayName)) {
      showLaunchFailure(item, endpoint, scope);
      return;
    }
    if (!endpointReady(endpoint)) {
      toast(`Starting ${item.displayName}…`);
      const action = await control(endpoint.backend, "start");
      if (!action.ok) throw new Error(action.message || `Could not start ${item.displayName}.`);
      for (let attempt = 0; attempt < 30; attempt += 1) {
        await delay(500);
        await refreshBackends({ quiet: true });
        item = findItem(identity);
        endpoint = item?.[scope];
        if (endpoint && endpointFailure(endpoint, item.displayName)) {
          showLaunchFailure(item, endpoint, scope);
          return;
        }
        if (endpoint && endpointReady(endpoint)) break;
      }
    }
    if (!endpoint || !endpointReady(endpoint)) throw new Error(`Timed out waiting for ${item?.displayName || "the app"}.`);
    window.location.assign(navigationURL(endpoint.frontend));
  }

  const delay = milliseconds => new Promise(resolve => window.setTimeout(resolve, milliseconds));

  async function control(backend, operation, values = {}, allowPassword = true, workspace = state.addWorkspace) {
    const query = new URLSearchParams({ serviceID: backend.serviceID, scope: backend.serviceScope, operation });
    const body = new URLSearchParams(values);
    const path = workspace ? `/api/wsl/${workspace.id}/control?${query}` : `/api/control?${query}`;
    const { buffer } = await requestBuffer(path, {
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
      return control(backend, operation, { ...values, sudoPassword: password }, false, workspace);
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
    let backdropPress = false;
    let backdropRelease = false;
    backdrop.addEventListener("pointerdown", event => {
      backdropPress = event.target === backdrop;
      backdropRelease = false;
    });
    backdrop.addEventListener("pointerup", event => {
      backdropRelease = event.target === backdrop;
    });
    backdrop.addEventListener("pointercancel", () => {
      backdropPress = false;
      backdropRelease = false;
    });
    backdrop.addEventListener("click", event => {
      const dismiss = backdropPress && backdropRelease && event.target === backdrop;
      backdropPress = false;
      backdropRelease = false;
      if (dismiss) closeDialog();
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
    if (!point) return;
    dialog.dataset.anchored = "true";
    dialog.style.visibility = "hidden";
    const bounds = dialog.getBoundingClientRect();
    const inset = 9;
    const left = Math.max(inset, Math.min(point.x, window.innerWidth - bounds.width - inset));
    const top = Math.max(inset, Math.min(point.y, window.innerHeight - bounds.height - inset));
    dialog.style.left = `${left}px`;
    dialog.style.top = `${top}px`;
    dialog.style.visibility = "visible";
  }

  function closeDialog() {
    elements.dialogLayer.replaceChildren();
    unlockPageScroll();
    state.logSelection = null;
    state.addWorkspace = null;
    state.containerCatalog = null;
    state.workspaceCatalog = [];
    state.recipes = [];
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
    const container = addContainer();
    if ((container && (!managedContainer(container) || !["agent", "catalog", "dockerfile"].includes(tab))) || (tab === "dockerfile" && (!container || !managedContainer(container)))) tab = "agent";
    const windowsCommands = state.hostPlatform === "windows" && !state.addWorkspace;
    state.addTab = tab;
    const apps = (state.addWorkspace ? state.workspaceCatalog || [] : state.backends).filter(backend => backend.serviceID !== "org.outershell.OuterShell" && backend.isBundled && !backend.isMigration && backend.serviceScope === state.addScope)
      .sort((a, b) => a.displayName.localeCompare(b.displayName));
    const catalogEntries = apps.map(backend => {
      const item = { backend, frontend: backend.frontends?.[0], displayName: backend.displayName };
      return { name: backend.displayName, html: `<article class="add-catalog-app ${backend.isInstalled ? "installed" : ""}">
        ${iconHTML(item)}<h3>${escapeHTML(backend.displayName)}</h3>
        ${backend.isInstalled ? '<span class="catalog-installed">Installed</span>' : `<button class="secondary-button" type="button" data-action="install" data-backend-key="${escapeHTML(backendKey(backend))}">${backend.serviceScope === "system" ? (state.hostPlatform === "windows" && !state.addWorkspace ? "Install as administrator" : "Install as root") : "Install"}</button>`}
      </article>` };
    });
    if (!state.addWorkspace && state.addScope !== "system") {
      catalogEntries.push({ name: "OpenCode", html: `<article class="add-catalog-app">
        <div class="app-icon" aria-hidden="true">&gt;_</div><h3>OpenCode</h3>
        <button class="secondary-button" type="button" data-action="setup-opencode">${openCodeItem() ? "Open OpenCode" : "Set up OpenCode"}</button>
      </article>` });
    }
    const catalog = container ? containerCatalogHTML(container) : catalogEntries.length ? `<div class="add-catalog">${catalogEntries.sort((a, b) => a.name.localeCompare(b.name)).map(entry => entry.html).join("")}</div>` : `<p class="add-empty">No bundled apps are available for this account.</p>`;
    const bash = `<form id="bash-form">
      <div class="form-grid">
        <div class="field"><label for="bash-name">Display name</label><input id="bash-name" name="name" placeholder="My server" required></div>
        <div class="field"><label for="bash-id">Identifier</label><input id="bash-id" class="mono" name="identifier" placeholder="my-server" required><span class="field-note">Used for the service and files.</span></div>
        <div class="field full"><label for="bash-command">${windowsCommands ? "Command Prompt commands (cmd.exe)" : "Bash commands"}</label><textarea id="bash-command" name="command" placeholder="${windowsCommands ? "cd /d C:\\dev\\my-app&#10;npm start" : "cd ~/dev/my-app&#10;npm start"}" required></textarea></div>
        <div class="field"><label for="bash-transport">Connection</label><select id="bash-transport" name="frontendTransport"><option value="port">TCP port</option>${windowsCommands ? '<option value="namedPipe">Named pipe</option>' : '<option value="unixSocket">Unix socket</option>'}</select></div>
        <div class="field" data-transport="port"><label for="bash-port">Port</label><input id="bash-port" class="mono" name="port" inputmode="numeric" placeholder="4000" required></div>
        <div class="field" data-transport="${windowsCommands ? "namedPipe" : "unixSocket"}" hidden><label for="bash-socket">${windowsCommands ? "Named pipe path" : "Socket path"}</label><input id="bash-socket" class="mono" name="socketPath" placeholder="${windowsCommands ? String.raw`\\.\pipe\my-app` : "/run/user/1000/my-app.sock"}"></div>
        <div class="field full" ${windowsCommands ? "hidden" : ""}><label for="bash-icon">Icon path <span class="field-note">(optional)</span></label><input id="bash-icon" class="mono" name="iconPath" placeholder="~/Pictures/my-app.png"></div>
      </div>
      ${windowsCommands ? "<p>Outer Shell starts these commands as your signed-in user. The command script must keep running until your server exits. For example, use <code>python server.py</code>, not <code>start python server.py</code>. Configure the server to listen on this port or named pipe.</p>" : ""}
      <p id="bash-message" class="form-message" role="status"></p>
      <footer class="dialog-footer"><button class="secondary-button" type="button" data-action="close-dialog">Cancel</button><button class="primary-button" type="submit">Create app</button></footer>
    </form>`;
    const codingAgent = codingAgentHTML();
    const dockerfile = container ? `<div class="customize-intro"><h3>Edit your Dockerfile directly</h3><p>Write the Dockerfile changes yourself, or use the Coding agent option to help write them. Both approaches preserve your setup across rebuilds.</p><p class="dockerfile-details">The container editor also includes folder mounts, environment values, and runtime settings for ${escapeHTML(container.name)}.</p><div class="add-actions"><button class="primary-button" type="button" data-action="configure-container" data-safe-space-id="${escapeHTML(container.id)}">Edit container…</button></div></div>` : "";
    const categories = [
      ["agent", "Coding agent", '<path d="M21 11a8 8 0 0 1-8 8H7l-5 3V11a9 9 0 0 1 19 0Z"/><path d="m7 9 3 3-3 3m6 0h4"/>'],
      ...(container && managedContainer(container) ? [["dockerfile", "Edit Dockerfile", '<path d="M14 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V8Z"/><path d="M14 2v6h6M8 13h8M8 17h6"/>']] : []),
      ["catalog", "Outer Shell app catalog", '<rect x="3" y="3" width="7" height="7" rx="1"/><rect x="14" y="3" width="7" height="7" rx="1"/><rect x="3" y="14" width="7" height="7" rx="1"/><rect x="14" y="14" width="7" height="7" rx="1"/>'],
      ["bash", windowsCommands ? "Run commands" : "Run bash commands", '<rect x="2" y="3" width="20" height="18" rx="2"/><path d="m6 8 3 3-3 3m6 0h5"/>'],
      ["recipes", "Other recipes", '<rect x="2" y="3" width="20" height="18" rx="2"/><path d="M6 8h1m3 0h8M6 12h1m3 0h8M6 16h1m3 0h5"/>']
    ].filter(([id]) => (!container || id === "agent" || managedContainer(container) && (id === "dockerfile" || id === "catalog")) && (!(state.addWorkspace || windowsCommands) || state.addScope !== "system" || id === "catalog" || id === "agent" || id === "dockerfile" || windowsCommands && id === "bash") && (!windowsCommands || id !== "recipes"));
    const dialog = openDialog(`
      <header class="dialog-header"><div class="dialog-title-wrap"><h2 id="dialog-title">Add Apps${state.addWorkspace ? ` · ${escapeHTML(state.addWorkspace.name)}${container ? "" : ` / ${state.addScope === "system" ? "root" : escapeHTML(state.addWorkspace.userName || "user")}`}` : ""}</h2></div><button class="dialog-close" type="button" data-action="close-dialog" aria-label="Close">×</button></header>
      <div class="dialog-body">
        <nav class="add-categories" aria-label="Add app type">${categories.map(([id, title, paths]) => `<button class="add-category" type="button" data-action="add-tab" data-tab="${id}" aria-pressed="${tab === id}"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">${paths}</svg><span>${title}</span></button>`).join("")}</nav>
        <section class="add-content" aria-label="${categories.find(([id]) => id === tab)?.[1] || "Apps"}">${tab === "dockerfile" ? dockerfile : tab === "catalog" ? catalog : tab === "bash" ? bash : tab === "agent" ? codingAgent : '<div id="add-recipes" aria-live="polite">Loading recipes…</div>'}</section>
      </div>`, "add-apps-dialog");
    if (tab === "bash") wireBashForm(dialog);
    if (tab === "recipes") loadAddRecipes(dialog);
  }

  async function loadAddRecipes(dialog) {
    const host = dialog.querySelector("#add-recipes");
    try {
      const { buffer } = await requestBuffer("/api/recipes");
      if (!dialog.isConnected) return;
      const recipes = new PayloadReader(buffer).child(8).payloadArray().map(reader => ({
        id: reader.stringRef(0), name: reader.stringRef(8), summary: reader.stringRef(16),
        fields: reader.child(24).payloadArray().map(field => ({
          key: field.stringRef(0), label: field.stringRef(8), value: field.stringRef(16),
          type: field.stringRef(24), placeholder: field.stringRef(32),
          choices: field.child(48).payloadArray().map(choice => ({ title: choice.stringRef(0), value: choice.stringRef(8) }))
        }))
      })).filter(recipe => ["jupyter", "jupyter-uv"].includes(recipe.id));
      if (!recipes.length) { host.textContent = "No other recipes are available on this server."; return; }
      host.innerHTML = `<div class="recipe-choices">${recipes.map((recipe, index) => `<button type="button" class="recipe-choice" data-recipe-index="${index}" aria-pressed="${index === 0}">${escapeHTML(recipe.name)}</button>`).join("")}</div><div class="recipe-editor"></div>`;
      const selectRecipe = index => {
        const recipe = recipes[index];
        host.querySelectorAll("[data-recipe-index]").forEach(button => button.setAttribute("aria-pressed", String(Number(button.dataset.recipeIndex) === index)));
        host.querySelector(".recipe-editor").innerHTML = `<p class="recipe-summary">${escapeHTML(recipe.summary)}</p><form id="recipe-form"><div class="form-grid">${recipe.fields.map(field => {
          const id = `recipe-${field.key}`;
          const input = field.type === "choice"
            ? `<select id="${escapeHTML(id)}" name="${escapeHTML(field.key)}">${field.choices.map(choice => `<option value="${escapeHTML(choice.value)}" ${choice.value === field.value ? "selected" : ""}>${escapeHTML(choice.title)}</option>`).join("")}</select>`
            : `<input id="${escapeHTML(id)}" name="${escapeHTML(field.key)}" value="${escapeHTML(field.value)}" placeholder="${escapeHTML(field.placeholder)}" ${["name", "identifier", "python", "workdir", "projectDir"].includes(field.key) ? "required" : ""}>`;
          return `<div class="field" ${field.key === "port" ? 'data-recipe-port' : ""}><label for="${escapeHTML(id)}">${escapeHTML(field.label)}</label>${input}</div>`;
        }).join("")}</div><p class="form-message" role="status"></p><footer class="dialog-footer"><button class="secondary-button" type="button" data-action="close-dialog">Cancel</button><button class="primary-button" type="submit">Create app</button></footer></form>`;
        const form = host.querySelector("form");
        const transport = form.elements.namedItem("frontendTransport");
        const updateTransport = () => {
          const port = form.querySelector("[data-recipe-port]");
          if (port) { port.hidden = transport.value !== "port"; port.querySelector("input").disabled = port.hidden; }
        };
        transport?.addEventListener("change", updateTransport);
        if (transport) updateTransport();
        form.addEventListener("submit", async event => {
          event.preventDefault();
          const button = form.querySelector('[type="submit"]');
          const message = form.querySelector(".form-message");
          button.disabled = true; button.textContent = "Creating…"; message.textContent = "";
          try {
            const query = new URLSearchParams(new FormData(form)); query.set("recipe", recipe.id);
            const { buffer } = await requestBuffer(`/api/create?${query}`, { method: "POST" }, true);
            const result = decodeAction(buffer);
            if (!result.ok) throw new Error(result.message || "Could not create the app.");
            if (dialog.isConnected) closeDialog();
            toast(result.message || `Created ${recipe.name}.`);
            await refreshBackends({ quiet: true });
          } catch (error) { message.textContent = error.message || String(error); button.disabled = false; button.textContent = "Create app"; }
        });
      };
      host.querySelectorAll("[data-recipe-index]").forEach(button => button.addEventListener("click", () => selectRecipe(Number(button.dataset.recipeIndex))));
      selectRecipe(0);
    } catch (error) {
      if (!dialog.isConnected) return;
      host.textContent = error.message || "Could not load recipes.";
      const retry = document.createElement("button"); retry.type = "button"; retry.className = "secondary-button"; retry.textContent = "Retry";
      retry.addEventListener("click", () => { host.textContent = "Loading recipes…"; loadAddRecipes(dialog); });
      host.append(retry);
    }
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
    const windowsCommands = state.hostPlatform === "windows" && !state.addWorkspace;
    const query = new URLSearchParams({
      recipe: "command-port",
      command: values.command,
      workdir: "~",
      frontendTransport: values.frontendTransport,
      name: values.name.trim(),
      identifier: values.identifier.trim()
    });
    if (values.frontendTransport === "unixSocket" || values.frontendTransport === "namedPipe") query.set("socketPath", values.socketPath.trim());
    else query.set("port", values.port.trim());
    if (values.iconPath.trim()) query.set("iconPath", values.iconPath.trim());
    if (windowsCommands) query.set("scope", state.addScope);
    submit.disabled = true;
    submit.textContent = "Creating…";
    message.textContent = "";
    try {
      const { buffer } = await requestBuffer(windowsCommands ? "/api/create" : `/api/create?${query}`, { method: "POST", ...(windowsCommands ? {headers: {"Content-Type": "application/x-www-form-urlencoded"}, body: query} : {}) }, true);
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
      ${showControl ? `<button class="context-menu-item" type="button" data-action="control" data-operation="${running ? "stop" : "start"}" data-backend-key="${key}" role="menuitem">${contextMenuGlyph(running ? "■" : "▶")}<span>${running ? "Stop" : endpointFailure(endpoint, item.displayName) ? "Retry" : "Start"}</span></button>` : ""}
      ${backend.logFiles.length ? `<button class="context-menu-item" type="button" data-action="logs" data-backend-key="${key}" role="menuitem">${contextMenuGlyph("≡")}<span>View Logs</span></button>` : ""}
      ${script ? `<button class="context-menu-item" type="button" data-action="copy-script" data-script="${escapeHTML(script)}" role="menuitem">${contextMenuGlyph("⧉")}<span>Copy Script Path</span></button>` : ""}
    </section>`;
  }

  function showContextMenu(contents, point = null, label = "Actions") {
    clearTextSelection();
    const dialog = openDialog(`<h2 id="dialog-title" class="context-menu-title">${escapeHTML(label)}</h2><div class="context-menu-scroll" role="menu">${contents}</div>`, "context-menu");
    dialog.setAttribute("aria-label", label);
    positionContextMenu(dialog, point);
    return dialog;
  }

  function menuContext(target) {
    if (target.closest(".overview-shortcut")) return "shortcut";
    return "endpoint";
  }

  function openAppMenu(identity, point = null, context = "endpoint") {
    const item = findItem(identity);
    if (!item) return;
    if (item.workspace) { openContainerMenu(item.workspace.id, item.app.serviceID, containerFrontendKey(item.app), point, context); return; }
    const backend = item.backend;
    const sections = [endpointContextMenuHTML(item, "primary", backend.serviceScope === "system" ? "Root" : "User")];
    const management = [
      pinMenuHTML(hostBookmarkKey(item), context === "shortcut"),
      renameEndpointMenuHTML(item),
      `<button class="context-menu-item" type="button" data-action="copy-url" data-app-key="${escapeHTML(item.identity)}" role="menuitem">${contextMenuGlyph("⧉")}<span>Copy URL</span></button>`
    ];
    const isOuterShell = backend.serviceID === "org.outershell.OuterShell" && backend.serviceScope !== "system";
    if (isOuterShell) {
      const key = escapeHTML(backendKey(backend));
      management.push(
        `<button class="context-menu-item" type="button" data-action="about-outer-shell" data-backend-key="${key}" role="menuitem">${contextMenuGlyph("ⓘ")}<span>About Outer Shell</span></button>`,
        `<button class="context-menu-item" type="button" data-action="check-outer-shell-update" data-backend-key="${key}" role="menuitem">${contextMenuGlyph("↻")}<span>Check for Updates</span></button>`
      );
      if (backend.menuBarVisibilityAvailable) {
        management.push(`<button class="context-menu-item menu-toggle" type="button" data-action="toggle-menu-bar" data-backend-key="${key}" data-enabled="${backend.menuBarVisibilityEnabled ? "true" : "false"}" role="menuitemcheckbox" aria-checked="${backend.menuBarVisibilityEnabled ? "true" : "false"}">${contextMenuGlyph(backend.menuBarVisibilityEnabled ? "✓" : "")}<span>Show in macOS menu bar when backends are running</span></button>`);
      }
    }
    if (isOuterShell || backend.canUninstall) {
      management.push(`<button class="context-menu-item danger" type="button" data-action="${isOuterShell ? "uninstall-outer-shell" : "uninstall"}" data-backend-key="${escapeHTML(backendKey(backend))}" role="menuitem">${contextMenuGlyph("−")}<span>Uninstall</span></button>`);
    }
    if (management.length) sections.push(`<section class="context-menu-section context-menu-management">${management.join("")}</section>`);
    showContextMenu(`<div class="context-menu-app-heading">${launcherIconHTML(item)}<strong>${escapeHTML(item.displayName)}</strong></div>${sections.filter(Boolean).join("")}`, point, `${item.displayName} actions`);
  }

  function managedContainer(workspace) {
    return workspace.ownsContainer !== false && workspace.managementKind !== "attached";
  }

  function containerPreparing(workspace) {
    return state.safeSpaceBusy.has(workspace.id) || ["creating", "starting", "rebuilding"].includes(safeSpaceState(workspace));
  }

  function containerDialogHeader(title) {
    return `<header class="dialog-header"><h2 id="dialog-title">${escapeHTML(title)}</h2><button class="dialog-close" type="button" data-action="close-dialog" aria-label="Close">×</button></header>`;
  }

  function runtimeCanCreate(provider) {
    return provider.isAvailable === true || (provider.id === "apple.container" && provider.status === "stopped");
  }

  function runtimeGuidance() {
    if (!state.providers.length) return "Runtime discovery has not completed. Choose Check again.";
    return state.providers.map(provider => {
      const setup = typeof provider.setupURL === "string" && provider.setupURL.startsWith("https://")
        ? ` <a href="${escapeHTML(provider.setupURL)}" target="_blank" rel="noopener noreferrer">Setup instructions</a>` : "";
      return `<p><strong>${escapeHTML(provider.name)}</strong>: ${escapeHTML(provider.detail || "Runtime status unavailable.")}${!provider.isAvailable ? setup : ""}</p>`;
    }).join("");
  }

  function runtimeOptions(selected = "") {
    return state.providers.map(provider => `<option value="${escapeHTML(provider.id)}" ${provider.id === selected ? "selected" : ""} ${runtimeCanCreate(provider) ? "" : "disabled"}>${escapeHTML(provider.name)}${provider.isAvailable ? "" : provider.status === "stopped" ? " (stopped)" : " (unavailable)"}</option>`).join("");
  }

  async function containerOperation(operation, values) {
    const id = values.workspaceID;
    if (id) state.safeSpaceBusy.add(id);
    render();
    try {
      const result = await safeSpaceRequest(operation, values);
      if (Array.isArray(result.workspaces)) state.safeSpaces = result.workspaces;
      return result;
    } finally {
      if (id) state.safeSpaceBusy.delete(id);
      render();
    }
  }

  async function uploadContainerArchive(file, transferID, onProgress, signal) {
    if (!/^[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}$/i.test(transferID)) throw new Error("Invalid container transfer identifier.");
    const endpoint = `/api/container-transfers/${transferID}`;
    const headers = { "Upload-Protocol": "outershell-resumable-v1" };
    const readOffset = response => {
      const text = response.headers.get("Upload-Offset");
      const offset = Number(text);
      if (!text || !/^\d+$/.test(text) || !Number.isSafeInteger(offset) || offset < 0 || offset > file.size) throw new Error("Invalid container upload offset.");
      return offset;
    };
    let offset = 0, failures = 0;
    while (offset < file.size) {
      signal.throwIfAborted();
      try {
        const head = await fetch(endpoint, { method: "HEAD", headers, cache: "no-store", signal });
        if (!head.ok) throw new Error(`Could not resume container upload (HTTP ${head.status}).`);
        offset = readOffset(head);
        if (offset === file.size) break;
        const end = Math.min(offset + 4 * 1024 * 1024, file.size);
        const response = await fetch(endpoint, { method: "PATCH", headers: { ...headers, "Content-Type": "application/offset+octet-stream", "Upload-Offset": String(offset) }, body: file.slice(offset, end), signal });
        if (response.status === 409) throw new Error("The upload offset changed. Retrying…");
        if (!response.ok) throw new Error(`Could not upload container (HTTP ${response.status}).`);
        const next = readOffset(response);
        if (next <= offset || next > end) throw new Error("The container upload made no progress.");
        offset = next;
        failures = 0;
        onProgress(Math.floor(offset / file.size * 100));
      } catch (error) {
        if (signal.aborted || ++failures > 3) throw error;
      }
    }
  }

  function openContainerImport(droppedFile) {
    const provider = state.providers.find(runtimeCanCreate);
    const dialog = openDialog(`${containerDialogHeader("Import container")}
      <form class="container-form"><div class="dialog-body form-grid">
        <label class="field full">Container file<input name="archive" type="file" accept=".outershell-container" ${droppedFile ? "" : "required"}></label>
        <p class="field-note full" data-import-file>${droppedFile ? escapeHTML(droppedFile.name) : "Choose a .outershell-container file, or drop one onto Outer Shell."}</p>
        <label class="field full">Runtime<select name="runtimeProviderID">${runtimeOptions(provider?.id)}</select></label>
        <div class="full" data-import-mounts hidden><p>This archive includes mounted folders. Choose where to restore them on this server.</p><ul data-import-mount-list></ul><label class="field">Destination folder on this server<input name="mountDestinationRoot" spellcheck="false"></label></div>
        <progress class="full" max="100" value="0" hidden></progress>
        <p class="container-message full" role="status">${provider ? "" : "No container runtime is ready on this server."}</p>
      </div><footer class="dialog-footer"><button type="button" data-action="close-dialog">Cancel</button><button class="primary-button" type="submit" ${provider ? "" : "disabled"}>Import</button></footer></form>`);
    const form = dialog.querySelector("form");
    const message = form.querySelector(".container-message");
    const button = form.querySelector('[type="submit"]');
    const progress = form.querySelector("progress");
    const controller = new AbortController();
    let file = droppedFile, transferID, uploaded = false, busy = false, finishing = false, completed = false;
    const observer = new MutationObserver(() => {
      if (dialog.isConnected) return;
      observer.disconnect();
      if (!finishing) controller.abort();
      if (!busy && transferID && !completed) safeSpaceRequest("cancelImport", { transferID }).catch(() => {});
    });
    observer.observe(elements.dialogLayer, { childList: true });
    form.elements.archive.addEventListener("change", () => {
      file = form.elements.archive.files[0];
      form.querySelector("[data-import-file]").textContent = file?.name || "Choose a .outershell-container file.";
    });
    form.addEventListener("submit", async event => {
      event.preventDefault();
      if (busy) return;
      if (!file || !file.name.toLowerCase().endsWith(".outershell-container") || !file.size) { message.textContent = "Choose a nonempty .outershell-container file."; return; }
      if (file.size > 8 * 1024 ** 3) { message.textContent = "Container archives must be no larger than 8 GB."; return; }
      busy = true; button.disabled = true;
      form.elements.archive.disabled = true;
      form.elements.runtimeProviderID.disabled = true;
      try {
        if (!transferID) {
          message.textContent = "Preparing upload…";
          const result = await safeSpaceRequest("beginImport");
          transferID = result.transferID;
          if (!transferID) throw new Error("The server could not begin the import.");
        }
        if (!uploaded) {
          progress.hidden = false;
          await uploadContainerArchive(file, transferID, percent => {
            progress.value = percent; message.textContent = `Uploading ${file.name}… ${percent}%`;
          }, controller.signal);
          uploaded = true;
        }
        controller.signal.throwIfAborted();
        finishing = true;
        message.textContent = "Importing container…";
        const values = { transferID, runtimeProviderID: form.elements.runtimeProviderID.value };
        if (form.elements.mountDestinationRoot.value.trim()) values.mountDestinationRoot = form.elements.mountDestinationRoot.value.trim();
        const result = await safeSpaceRequest("finishImport", values);
        finishing = false;
        if (result.needsMountDestination) {
          if (!dialog.isConnected) throw new Error("Import needs a destination for mounted folders. Drop the archive again to continue.");
          form.querySelector("[data-import-mounts]").hidden = false;
          form.querySelector("[data-import-mount-list]").innerHTML = (result.importMounts || []).map(mount => `<li>${escapeHTML(mount.name || mount.guestPath || "Mounted folder")}</li>`).join("");
          form.elements.mountDestinationRoot.value = result.suggestedMountRoot || "";
          form.elements.mountDestinationRoot.required = true;
          message.textContent = "Choose a destination folder, then import. The archive is already uploaded.";
          button.textContent = "Import with folders";
          return;
        }
        completed = true;
        if (dialog.isConnected) closeDialog();
        toast(result.omittedMountCount ? `Container imported. ${result.omittedMountCount} folder mount(s) need to be reconnected.` : "Container imported. Its image is being built.");
        await refreshSafeSpaces();
      } catch (error) {
        finishing = false;
        if (dialog.isConnected) message.textContent = error.message || String(error);
        else if (error.name !== "AbortError") toast(error.message || String(error), true);
      } finally {
        busy = false; button.disabled = false;
        if (!dialog.isConnected && transferID && !completed) safeSpaceRequest("cancelImport", { transferID }).catch(() => {});
      }
    });
  }

  function openCreateContainer(includeOpenCode = false) {
    const provider = state.providers.find(runtimeCanCreate);
    const dialog = openDialog(`${containerDialogHeader("New container")}
      <form class="container-form"><div class="dialog-body form-grid">
        <label class="field full">Name<input name="name" required autocomplete="off" value="${includeOpenCode ? "OpenCode workspace" : ""}"></label>
        <label class="field full">Runtime<select name="runtimeProviderID">${runtimeOptions(provider?.id)}</select></label>
        <label class="field full">Base image<input name="baseImage" value="${escapeHTML(provider?.defaultBaseImage || "")}" required spellcheck="false"></label>
        <label class="field">Max CPUs<input name="cpus" type="number" min="1" step="1" value="4" required></label>
        <label class="field">Max Memory (GB)<input name="memoryInGB" type="number" min="1" step="1" value="8" required></label>
        <label class="container-check full"><input name="includeOpenCode" type="checkbox" ${includeOpenCode ? "checked" : ""}>Install OpenCode</label>
        <div class="full" data-opencode-permissions ${includeOpenCode ? "" : "hidden"}><label class="container-check"><input name="allowAllOpenCodeActions" type="checkbox">Allow all actions without asking</label><p class="field-note">Applies to OpenCode in this container, including folders mounted into it.</p></div>
        <p class="field-note full">OpenCode opens a workspace with instructions for adding apps to this container. Its workspace and model-provider settings persist across rebuilds. Requires a Debian or Ubuntu image with Outer Shell support.</p>
        <p class="field-note full">The container is built on this server. You can edit its Dockerfile and runtime configuration after creation.</p>
        <div class="runtime-guidance full">${runtimeGuidance()}</div>
        <button type="button" class="runtime-recheck">Check again</button>
        <p class="container-message full" role="status">${provider ? "" : "No container runtime is ready on this server."}</p>
      </div><footer class="dialog-footer"><button type="button" data-action="close-dialog">Cancel</button><button class="primary-button" type="submit" ${provider ? "" : "disabled"}>Create</button></footer></form>`);
    const form = dialog.querySelector("form");
    form.elements.includeOpenCode.addEventListener("change", () => {
      form.querySelector("[data-opencode-permissions]").hidden = !form.elements.includeOpenCode.checked;
    });
    form.querySelector(".runtime-recheck").addEventListener("click", async event => {
      const button = event.currentTarget;
      button.disabled = true;
      const message = form.querySelector(".container-message");
      message.textContent = "Checking runtimes on this server…";
      try {
        await safeSpaceRequest("checkRuntimes");
        if (!dialog.isConnected) return;
        const select = form.elements.runtimeProviderID;
        const selected = state.providers.find(value => value.id === select.value && runtimeCanCreate(value)) || state.providers.find(runtimeCanCreate);
        select.innerHTML = runtimeOptions(selected?.id);
        if (!form.elements.baseImage.value && selected) form.elements.baseImage.value = selected.defaultBaseImage || "";
        form.querySelector(".runtime-guidance").innerHTML = runtimeGuidance();
        form.querySelector('[type="submit"]').disabled = !selected;
        message.textContent = selected ? "Runtime check complete." : "No container runtime is ready on this server.";
      } catch (error) { message.textContent = error.message; }
      finally { button.disabled = false; }
    });
    form.addEventListener("submit", async event => {
      event.preventDefault();
      const data = new FormData(form);
      const button = form.querySelector('[type="submit"]');
      button.disabled = true;
      const message = form.querySelector(".container-message");
      message.textContent = "Creating container…";
      try {
        const baseImage = String(data.get("baseImage")).trim();
        if (!/^[a-zA-Z0-9][a-zA-Z0-9._/:@-]*$/.test(baseImage)) throw new Error("Enter a valid base image reference.");
        const dockerfile = data.get("includeOpenCode") ? `# syntax=docker/dockerfile:1\nFROM ${baseImage}\n${await openCodeContainerFragment(data.get("allowAllOpenCodeActions") === "on")}` : undefined;
        await containerOperation("create", { name: String(data.get("name")).trim(), runtimeProviderID: data.get("runtimeProviderID"), baseImage, dockerfile, cpus: Number(data.get("cpus")), memoryInGB: Number(data.get("memoryInGB")) });
        if (dialog.isConnected) closeDialog();
        toast("Container creation requested. Its status appears under All endpoints.");
      } catch (error) { message.textContent = error.message; button.disabled = false; }
    });
  }

  function configurationRow(kind, value = {}) {
    const input = (name, label, content, type = "text") => `<label class="field">${label}<input data-column="${name}" type="${type}" value="${escapeHTML(content ?? "")}" ${type === "number" ? 'min="1" max="65535" step="1"' : 'spellcheck="false"'}></label>`;
    let fields;
    if (kind === "mounts") {
      fields = input("name", "Name", value.name) + input("hostPath", "Server folder", value.hostPath) + input("guestPath", "Container path", value.guestPath) + `<label class="container-check"><input data-column="isReadOnly" type="checkbox" ${value.isReadOnly ? "checked" : ""}>Read only</label>`;
    } else if (kind === "environment") {
      fields = input("name", "Name", value.name) + input("value", "Value", value.value);
    } else {
      fields = input("hostPort", "Server port", value.hostPort, "number") + input("containerPort", "Container port", value.containerPort, "number");
    }
    return `<div class="configuration-row" data-row="${kind}" data-id="${escapeHTML(value.id || makeRequestID())}">${fields}<button type="button" data-remove-row aria-label="Remove ${kind === "mounts" ? "mount" : kind === "environment" ? "variable" : "port"}">×</button></div>`;
  }

  function configurationValues(form, kind) {
    return [...form.querySelectorAll(`[data-row="${kind}"]`)].map(row => {
      const value = kind === "mounts" ? { id: row.dataset.id } : {};
      row.querySelectorAll("[data-column]").forEach(input => {
        value[input.dataset.column] = input.type === "checkbox" ? input.checked : input.type === "number" ? Number(input.value) : input.value;
      });
      return value;
    });
  }

  function validateConfiguration(value) {
    if (!value.dockerfile.trim()) throw new Error("The Dockerfile cannot be empty.");
    const paths = new Set();
    for (const mount of value.mounts) {
      mount.name = mount.name.trim(); mount.hostPath = mount.hostPath.trim(); mount.guestPath = mount.guestPath.trim();
      if (!mount.hostPath.startsWith("/") || !mount.guestPath.startsWith("/") || mount.guestPath.includes(":") || mount.guestPath === "/var/lib/outershell/project" || paths.has(mount.guestPath)) throw new Error("Mounts need an absolute server path and a unique absolute container path.");
      paths.add(mount.guestPath);
    }
    const names = new Set();
    for (const variable of value.environment) {
      variable.name = variable.name.trim();
      if (!/^[A-Za-z_][A-Za-z0-9_]*$/.test(variable.name) || names.has(variable.name) || variable.value.includes("\0")) throw new Error("Environment names must be unique shell identifiers.");
      names.add(variable.name);
    }
    const ports = new Set();
    for (const port of value.publishedPorts) {
      if (![port.hostPort, port.containerPort].every(number => Number.isInteger(number) && number >= 1 && number <= 65535) || ports.has(port.hostPort)) throw new Error("Ports must be between 1 and 65535, with no repeated server ports.");
      ports.add(port.hostPort);
    }
  }

  async function openContainerConfiguration(id, catalogID = null) {
    let workspace = findSafeSpace(id);
    if (workspace && !workspace.recipe) {
      try {
        const result = await safeSpaceRequest("listDetails");
        workspace = result.workspaces?.find(value => value.id === id);
      } catch (error) { toast(error.message, true); return; }
    }
    if (!workspace || !managedContainer(workspace)) return;
    const recipe = workspace.recipe;
    if (!recipe || typeof recipe.containerfile !== "string") { toast("The container configuration is not available yet.", true); return; }
    const catalogApp = containerAppCatalog.find(app => app.id === catalogID);
    let proposedDockerfile = recipe.containerfile;
    let proposalMessage = "";
    if (catalogApp) {
      try {
        const draft = await containerAppDraft(recipe.containerfile, catalogID);
        proposedDockerfile = draft.dockerfile;
        proposalMessage = draft.message;
      } catch (error) { toast(error.message || String(error), true); return; }
    }
    const infrastructure = (workspace.mounts || []).filter(mount => mount.isInfrastructure);
    const tabs = [["dockerfile", "Dockerfile"], ["mounts", "Folder mounts"], ["environment", "Environment"], ["ports", "Ports"], ["runtime", "Runtime"]];
    const dialog = openDialog(`${containerDialogHeader(`Edit ${workspace.name}`)}
      <form class="container-form" novalidate><div class="dialog-body">
        <label class="field">Name<input name="name" value="${escapeHTML(workspace.name)}" required></label>
        <div class="configuration-tabs" role="tablist" aria-label="Container configuration">${tabs.map(([key, label], index) => `<button type="button" role="tab" id="config-tab-${key}" aria-controls="config-panel-${key}" aria-selected="${index === 0}" data-config-tab="${key}">${label}</button>`).join("")}</div>
        <section role="tabpanel" id="config-panel-dockerfile" aria-labelledby="config-tab-dockerfile" data-config-panel="dockerfile"><label class="field">Dockerfile<textarea name="dockerfile" class="dockerfile-editor" spellcheck="false" autocapitalize="off" autocorrect="off">${escapeHTML(proposedDockerfile)}</textarea></label><p class="field-note">Save stores the definition. Save &amp; rebuild applies it to the container.</p><label class="container-check"><input name="allowAllOpenCodeActions" type="checkbox">Allow all actions without asking</label><p class="field-note">Applies when adding OpenCode below, including access to folders mounted into this container.</p><button type="button" data-add-opencode>Add OpenCode</button><p class="field-note">Adds OpenCode and container instructions to this Dockerfile. Requires Debian or Ubuntu with Outer Shell support.</p></section>
        <section role="tabpanel" id="config-panel-mounts" aria-labelledby="config-tab-mounts" data-config-panel="mounts" hidden><p class="field-note">Server folders are paths on the server running Outer Shell. They must already exist.</p><div data-rows="mounts">${(workspace.mounts || []).filter(mount => !mount.isInfrastructure).map(mount => configurationRow("mounts", mount)).join("")}</div><button type="button" data-add-row="mounts">Add folder mount</button>${infrastructure.length ? `<details class="managed-mounts"><summary>Managed mounts (${infrastructure.length})</summary>${infrastructure.map(mount => `<p><code>${escapeHTML(mount.hostPath)} → ${escapeHTML(mount.guestPath)}</code></p>`).join("")}</details>` : ""}</section>
        <section role="tabpanel" id="config-panel-environment" aria-labelledby="config-tab-environment" data-config-panel="environment" hidden><div data-rows="environment">${(recipe.environment || []).map(value => configurationRow("environment", value)).join("")}</div><button type="button" data-add-row="environment">Add variable</button></section>
        <section role="tabpanel" id="config-panel-ports" aria-labelledby="config-tab-ports" data-config-panel="ports" hidden><p class="field-note">Publish a container TCP port on this server.</p><div data-rows="ports">${(recipe.publishedPorts || []).map(value => configurationRow("ports", value)).join("")}</div><button type="button" data-add-row="ports">Add port</button></section>
        <section role="tabpanel" id="config-panel-runtime" aria-labelledby="config-tab-runtime" data-config-panel="runtime" hidden><p>${escapeHTML(safeSpaceRuntimeDescription(workspace))} · ${escapeHTML(workspace.cpus)} CPUs · ${escapeHTML(workspace.memoryInGB)} GB</p><label class="field">Runtime<select name="runtimeProviderID">${runtimeOptions(workspace.runtime?.providerID)}</select></label><p class="field-note">Changing runtime saves the configuration and rebuilds the container with the selected provider.</p><button type="submit" value="runtime">Save &amp; change runtime</button></section>
        <p class="container-message" role="status">${escapeHTML(proposalMessage || (recipe.needsRebuild ? "Saved changes need a rebuild." : ""))}</p>
        <p class="field-note">Rebuilding restarts the container. Changes outside its Dockerfile, persistent data, and mounted folders are not retained.</p>
      </div><footer class="dialog-footer"><button type="button" data-action="close-dialog">Close</button><button type="submit" value="save">Save</button><button class="primary-button" type="submit" value="rebuild">Save &amp; rebuild</button></footer></form>`, "wide container-editor");
    const form = dialog.querySelector("form");
    form.addEventListener("click", event => {
      const tab = event.target.closest("[data-config-tab]");
      if (tab) {
        form.querySelectorAll("[data-config-tab]").forEach(button => button.setAttribute("aria-selected", String(button === tab)));
        form.querySelectorAll("[data-config-panel]").forEach(panel => { panel.hidden = panel.dataset.configPanel !== tab.dataset.configTab; });
      }
      const add = event.target.closest("[data-add-row]");
      if (add) form.querySelector(`[data-rows="${add.dataset.addRow}"]`).insertAdjacentHTML("beforeend", configurationRow(add.dataset.addRow));
      event.target.closest("[data-remove-row]")?.closest("[data-row]").remove();
    });
    form.querySelector("[data-add-opencode]").addEventListener("click", async event => {
      const button = event.currentTarget;
      const message = form.querySelector(".container-message");
      if (form.elements.dockerfile.value.includes("/usr/local/bin/outershell-opencode")) {
        message.textContent = "This Dockerfile already includes OpenCode.";
        return;
      }
      button.disabled = true;
      try {
        const fragment = await openCodeContainerFragment(form.elements.allowAllOpenCodeActions.checked);
        form.elements.dockerfile.value = form.elements.dockerfile.value.trimEnd() + "\n\n" + fragment;
        message.textContent = "OpenCode added. Choose Save & rebuild to install it.";
      } catch (error) { message.textContent = error.message; }
      finally { button.disabled = false; }
    });
    let savedDockerfile = recipe.containerfile;
    let savedName = workspace.name;
    let currentProvider = workspace.runtime?.providerID;
    form.addEventListener("submit", async event => {
      event.preventDefault();
      const action = event.submitter?.value || "save";
      const message = form.querySelector(".container-message");
      const values = { workspaceID: id, dockerfile: form.elements.dockerfile.value, mounts: configurationValues(form, "mounts"), environment: configurationValues(form, "environment"), publishedPorts: configurationValues(form, "ports") };
      const name = form.elements.name.value.trim();
      try { if (!name) throw new Error("Enter a container name."); validateConfiguration(values); }
      catch (error) { message.textContent = error.message; return; }
      const provider = form.elements.runtimeProviderID.value;
      const controls = [...form.querySelectorAll("input, select, textarea, button")];
      controls.forEach(control => { control.disabled = true; });
      let saved = false;
      message.textContent = "Saving…";
      try {
        if (values.dockerfile !== savedDockerfile) {
          // macOS reads command; the Linux provider reads dockerfile.
          await containerOperation("updateDockerfile", { workspaceID: id, command: values.dockerfile, dockerfile: values.dockerfile });
          savedDockerfile = values.dockerfile;
          saved = true;
        }
        await containerOperation("updateContainerConfiguration", values);
        saved = true;
        if (name !== savedName) { await containerOperation("rename", { workspaceID: id, name }); savedName = name; }
        if (action === "runtime" && provider !== currentProvider) {
          message.textContent = "Changing runtime…";
          await containerOperation("changeRuntime", { workspaceID: id, runtimeProviderID: provider });
          currentProvider = provider;
        } else if (action === "rebuild") {
          message.textContent = "Rebuilding…";
          await containerOperation("rebuildRecipe", { workspaceID: id });
        }
        if (action === "save") message.textContent = "Saved. Rebuild to apply configuration changes.";
        else {
          if (dialog.isConnected) closeDialog();
          await refreshSafeSpaces({ quiet: true });
        }
      } catch (error) { message.textContent = `${saved ? "Some changes were saved. " : ""}${error.message}`; }
      finally { controls.forEach(control => { control.disabled = false; }); }
    });
  }

  function openShareContainer(id) {
    const workspace = findSafeSpace(id);
    if (!workspace || !managedContainer(workspace)) return;
    const dialog = openDialog(`${containerDialogHeader("Share Container")}
      <form class="container-form"><div class="dialog-body">
        <p>Create a portable archive of ${escapeHTML(workspace.name)} with its Dockerfile and build-context files.</p>
        <label class="container-check"><input name="persistent" type="checkbox">Include persistent data</label>
        <label class="container-check"><input name="mounts" type="checkbox">Include mounted folders from this server</label>
        <p class="field-note">The archive includes configured environment values. Only include data you intend to share.</p>
        <p class="container-message" role="status"></p><div class="container-download"></div>
      </div><footer class="dialog-footer"><button type="button" data-action="close-dialog">Close</button><button class="primary-button" type="submit">Prepare archive</button></footer></form>`);
    const form = dialog.querySelector("form");
    form.addEventListener("submit", async event => {
      event.preventDefault();
      const button = form.querySelector('[type="submit"]');
      const message = form.querySelector(".container-message");
      const includePersistentData = form.elements.persistent.checked;
      const includeMountedFolders = form.elements.mounts.checked;
      button.disabled = true;
      try {
        message.textContent = "Preparing archive…";
        const result = await containerOperation("prepareShare", { workspaceID: id, includePersistentData, includeMountedFolders });
        if (!/^[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}$/i.test(result.transferID)) throw new Error("Invalid container transfer identifier.");
        const extension = ".outershell-container";
        const fileName = (result.fileName || "Container").replace(/[^a-zA-Z0-9._-]/g, "-");
        const stem = fileName.endsWith(extension) ? fileName.slice(0, -extension.length) : fileName;
        const name = (stem || "Container").slice(0, 240 - extension.length) + extension;
        const link = document.createElement("a");
        link.href = `/api/container-downloads/${result.transferID}/${name}`;
        link.download = name;
        link.textContent = `Download ${name}`;
        form.querySelector(".container-download").replaceChildren(link);
        message.textContent = "Archive ready.";
        link.click();
      } catch (error) {
        message.textContent = error.name === "AbortError" ? "Cancelled." : error.message;
      } finally { button.disabled = false; }
    });
  }

  async function openContainerStorage() {
    const providers = state.providers.filter(provider => provider.capabilities?.supportsStorageCleanup);
    const dialog = openDialog(`${containerDialogHeader("Container storage")}
      <form><div class="dialog-body"><label class="field">Runtime<select name="runtimeProviderID">${providers.map(provider => `<option value="${escapeHTML(provider.id)}">${escapeHTML(provider.name)}</option>`).join("")}</select></label>
      <p>Choose obsolete Outer Shell images to remove. Containers, persistent data, and mounted folders are preserved. Apple container also cleans orphaned snapshots.</p>
      <p class="field-note">Image sizes are estimates; shared layers and disk blocks can reduce the space recovered. Unrelated images are retained.</p>
      <div data-storage-images></div><p class="container-message" role="status">Inspecting storage…</p>
      <section data-build-cache hidden><p>Docker build cache is shared with other projects on this engine. This separate action removes eligible cache older than 7 days, with a 10 GB retention setting. Future builds may take longer.</p><button type="button" data-clean-build-cache disabled>Clean old build cache</button></section></div>
      <footer class="dialog-footer"><button type="button" data-action="close-dialog">Close</button><button type="submit" class="primary-button" disabled>Clean selected images</button></footer></form>`);
    const form = dialog.querySelector("form");
    const message = form.querySelector(".container-message");
    const button = form.querySelector('[type="submit"]');
    const cacheButton = form.querySelector('[data-clean-build-cache]');
    const runtime = form.elements.runtimeProviderID;
    const list = form.querySelector("[data-storage-images]");
    let images = [], busy = false, cacheSupported = false;
    const setBusy = value => {
      busy = value;
      runtime.disabled = value;
      button.disabled = value;
      cacheButton.disabled = value || !cacheSupported;
    };
    const renderStorage = storage => {
      images = storage.images || [];
      cacheSupported = storage.supportsBuildCacheCleanup === true;
      form.querySelector('[data-build-cache]').hidden = !cacheSupported;
      const orphanEstimate = Number.isFinite(storage.orphanSnapshotBytes)
        ? `<p>Orphaned snapshots: <strong>${(storage.orphanSnapshotBytes / 1073741824).toFixed(2)} GiB</strong> estimated.</p>` : "";
      list.innerHTML = orphanEstimate + images.map((image, index) => {
        const bytes = image.snapshotBytes ?? image.imageBytes;
        return `<label class="container-check"><input type="checkbox" name="image" value="${index}"><span>${escapeHTML(image.reference)}${Number.isFinite(bytes) ? ` — ${(bytes / 1073741824).toFixed(2)} GiB` : ""}</span></label>`;
      }).join("");
      message.textContent = (storage.failures || []).join("\n") || "Only selected images will be removed. Apple container also removes orphaned cache data.";
    };
    const load = async () => {
      setBusy(true); list.innerHTML = "";
      message.textContent = "Inspecting storage…";
      let ready = false;
      try {
        const result = await safeSpaceRequest("containerStorage", { runtimeProviderID: runtime.value });
        if (!dialog.isConnected) return;
        renderStorage(result.storage); ready = true;
      } catch (error) { message.textContent = error.message || String(error); }
      finally {
        setBusy(false);
        button.disabled = !ready;
        cacheButton.disabled = !ready || !cacheSupported;
      }
    };
    const clean = async buildCache => {
      if (busy || (buildCache ? cacheButton.disabled : button.disabled)) return;
      const selected = Array.from(form.querySelectorAll('input[name="image"]:checked'), input => images[Number(input.value)].reference);
      setBusy(true); message.textContent = "Cleaning storage…";
      try {
        const result = await safeSpaceRequest(buildCache ? "cleanContainerBuildCache" : "cleanContainerStorage", { runtimeProviderID: runtime.value, images: buildCache ? [] : selected });
        if (dialog.isConnected) {
          renderStorage(result.storage);
          if (!result.storage.failures?.length) message.textContent = buildCache ? "Old build-cache cleanup completed." : `Cleanup completed. Removed ${result.storage.deleted.length} selected image(s).`;
        }
      } catch (error) { message.textContent = error.message || String(error); }
      finally { setBusy(false); }
    };
    form.addEventListener("submit", event => { event.preventDefault(); return clean(false); });
    cacheButton.addEventListener("click", () => clean(true));
    runtime.addEventListener("change", load);
    await load();
  }

  function openDeleteContainer(id) {
    const workspace = findSafeSpace(id);
    if (!workspace || workspace.runtimeKind === "wsl" || containerPreparing(workspace)) return;
    const managed = managedContainer(workspace);
    const title = managed ? `Delete ${workspace.name}?` : `Remove ${workspace.name} from Outer Shell?`;
    const detail = managed
      ? state.hostPlatform === "windows"
        ? "The container and its runtime-managed persistent data will be permanently deleted. Saved Dockerfiles and mounted folders will remain on the Windows server. This cannot be undone."
        : "The container, its runtime-managed persistent data, and its Outer Shell configuration will be permanently deleted. Mounted folders will remain on the server. This cannot be undone."
      : "Outer Shell will forget this registration. The externally managed container and all of its data will remain unchanged.";
    const label = managed ? "Delete container" : "Remove registration";
    const dialog = openDialog(`${containerDialogHeader(title)}
      <form><div class="dialog-body"><p>${escapeHTML(detail)}</p><p class="form-message" role="status"></p></div>
      <footer class="dialog-footer"><button class="secondary-button" type="button" data-action="close-dialog">Cancel</button><button class="danger-button" type="submit">${label}</button></footer></form>`);
    const form = dialog.querySelector("form");
    form.addEventListener("submit", async event => {
      event.preventDefault();
      const button = form.querySelector('[type="submit"]');
      if (button.disabled) return;
      const message = form.querySelector(".form-message");
      button.disabled = true;
      button.textContent = managed ? "Deleting…" : "Removing…";
      message.textContent = "";
      try {
        await containerOperation("delete", { workspaceID: id });
        if (dialog.isConnected) closeDialog();
        toast(managed ? "Container deleted." : "Container registration removed.");
        await refreshSafeSpaces({ quiet: true });
      } catch (error) {
        message.textContent = error.message || String(error);
        button.disabled = false;
        button.textContent = label;
      }
    });
    dialog.querySelector('[data-action="close-dialog"]')?.focus();
  }

  function openContainerMenu(id, serviceID, frontendKey, point = null, context = "endpoint") {
    const workspace = findSafeSpace(id);
    if (!workspace) return;
    const app = serviceID ? findSafeSpaceApp(workspace, serviceID, frontendKey) : null;
    if (serviceID && !app) return;
    const name = app ? state.endpointNames[containerBookmarkKey(workspace, app)] || app.displayName || app.serviceID : workspace.name || "Container";
    const running = app ? app.isRunning : safeSpaceIsRunning(workspace);
    const preparing = containerPreparing(workspace);
    const operation = app ? (running ? "stopApp" : "startApp") : (running ? "stop" : "start");
    const attributes = `data-safe-space-id="${escapeHTML(id)}" data-service-id="${escapeHTML(serviceID || "")}" data-frontend-key="${escapeHTML(frontendKey || "")}"`;
    const url = app ? safeSpaceAppNavigationURL(workspace, app) : "";
    showContextMenu(`<div class="context-menu-app-heading">${app ? launcherIconHTML(findItem(containerBookmarkKey(workspace, app))) : ""}<strong>${escapeHTML(name)}</strong></div>
      <section class="context-menu-section">
        ${!app && managedContainer(workspace) ? `<button class="context-menu-item" type="button" data-action="configure-container" data-safe-space-id="${escapeHTML(id)}" role="menuitem" ${preparing ? "disabled" : ""}>${contextMenuGlyph("✎")}<span>Edit container…</span></button><button class="context-menu-item" type="button" data-action="share-container" data-safe-space-id="${escapeHTML(id)}" role="menuitem" ${preparing ? "disabled" : ""}>${contextMenuGlyph("↗")}<span>Share Container…</span></button>` : ""}
        ${app ? pinMenuHTML(containerBookmarkKey(workspace, app), context === "shortcut") + renameEndpointMenuHTML(findItem(containerBookmarkKey(workspace, app))) : ""}
        ${app ? `<a class="context-menu-item" href="${escapeHTML(url)}" data-action="launch-safe-space-app" ${attributes} role="menuitem">${contextMenuGlyph("↗")}<span>Open</span></a>` : `<p class="container-details">${escapeHTML(safeSpaceRuntimeDescription(workspace))}</p>`}
        <button class="context-menu-item" type="button" data-action="container-control" data-operation="${operation}" ${attributes} role="menuitem" ${preparing ? "disabled" : ""}>${contextMenuGlyph(running ? "■" : "▶")}<span>${preparing ? "Preparing…" : running ? "Stop" : "Start"}</span></button>
        ${app && workspace.runtimeKind === "wsl" ? `<button class="context-menu-item" type="button" data-action="uninstall-wsl-app" ${attributes} role="menuitem">${contextMenuGlyph("×")}<span>Uninstall</span></button>` : ""}
        ${app && url !== "#" ? `<button class="context-menu-item" type="button" data-action="copy-container-url" ${attributes} role="menuitem">${contextMenuGlyph("⧉")}<span>Copy URL</span></button>` : ""}
      </section>
      ${!app && workspace.runtimeKind !== "wsl" ? `<section class="context-menu-section"><button class="context-menu-item danger" type="button" data-action="delete-container" data-safe-space-id="${escapeHTML(id)}" role="menuitem" ${preparing ? "disabled" : ""}>${contextMenuGlyph("×")}<span>${managedContainer(workspace) ? "Delete container…" : "Remove from Outer Shell…"}</span></button></section>` : ""}`, point, `${name} actions`);
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
    const label = button.querySelector(":scope > span:not(.context-menu-glyph)") || button;
    const original = label.textContent;
    label.textContent = "Checking for Updates…";
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
      label.textContent = original;
    }
  }

  async function performOuterShellOperation(key, operation, button, reloadAfter = false) {
    const backend = findBackend(key);
    if (!backend) return;
    button.disabled = true;
    const label = button.querySelector(":scope > span:not(.context-menu-glyph)") || button;
    const original = label.textContent;
    label.textContent = `${original}…`;
    try {
      const result = await control(backend, operation);
      if (!result.ok) throw new Error(result.message || "The action failed.");
      closeDialog();
      toast(result.message);
      if (reloadAfter) window.setTimeout(() => window.location.reload(), 1250);
    } catch (error) {
      toast(error.message || String(error), true);
      button.disabled = false;
      label.textContent = original;
    }
  }

  async function uninstallOuterShell(key, button) {
    if (!window.confirm("Uninstall Outer Shell?")) return;
    await performOuterShellOperation(key, "uninstallOuterShell", button);
  }

  function showOperationFailure(title, error) {
    openDialog(`<header class="dialog-header"><h2 id="dialog-title">${escapeHTML(title)}</h2></header>
      <div class="dialog-body"><pre class="operation-error" role="alert">${escapeHTML(error.message || String(error))}</pre></div>
      <footer class="dialog-footer"><button type="button" data-action="close-dialog">Close</button></footer>`);
  }

  async function performControl(key, operation, button) {
    const backend = findBackend(key);
    if (!backend) return;
    button.disabled = true;
    const label = button.querySelector(":scope > span:not(.context-menu-glyph)") || button;
    const original = label.textContent;
    label.textContent = `${original}…`;
    try {
      const result = await control(backend, operation);
      if (!result.ok) throw new Error(result.message || "The action failed.");
      closeDialog();
      toast(result.message);
      await refreshBackends({ quiet: true });
    } catch (error) {
      showOperationFailure(`Could not complete the action for ${backend.displayName}`, error);
      button.disabled = false;
      label.textContent = original;
    }
  }

  async function installBackend(key, button) {
    const backend = findBackend(key);
    if (!backend) return;
    button.disabled = true;
    button.textContent = "Installing…";
    try {
      const operation = backend.serviceScope === "system" ? "runRoot" : "runUser";
      const result = await control(backend, operation);
      if (!result.ok) throw new Error(result.message || `Could not install ${backend.displayName}.`);
      closeDialog();
      toast(result.message || `Installed ${backend.displayName}.`);
      await Promise.all([refreshBackends({ quiet: true }), refreshSafeSpaces({ quiet: true })]);
    } catch (error) {
      showOperationFailure(`Could not install ${backend.displayName}`, error);
      button.disabled = false;
      button.textContent = backend.serviceScope === "system" ? (state.hostPlatform === "windows" && !state.addWorkspace ? "Install as administrator" : "Install as root") : "Install";
    }
  }

  async function uninstallBackend(key, button) {
    const backend = findBackend(key);
    if (!backend || !window.confirm(`Uninstall ${backend.displayName}?`)) return;
    await performControl(key, "uninstall", button);
  }

  async function copyText(text) {
    const area = document.createElement("textarea");
    area.value = text;
    area.style.cssText = "position:fixed;left:-9999px;top:0";
    const focused = document.activeElement;
    document.body.append(area);
    area.select();
    let copied = false;
    try { copied = document.execCommand("copy"); } catch (_) {}
    area.remove();
    focused?.focus();
    if (!copied) {
      try { await navigator.clipboard.writeText(text); copied = true; } catch (_) {}
    }
    toast(copied ? "Copied to the clipboard." : "Could not copy. Select the text and copy it manually.");
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
      const contents = log.error || log.contents || "No logs yet.";
      const description = `${log.path || "No registered log"}${log.truncated ? " · showing latest output" : ""} · ${formatBytes(log.fileSize)}`;
      if (output && output.textContent !== contents) output.textContent = contents;
      if (meta && meta.textContent !== description) meta.textContent = description;
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
      if (state.suspended) { await delay(1000); continue; }
      const controller = new AbortController();
      state.eventAbort = controller;
      const query = new URLSearchParams({
        sinceBackends: state.backendsVersion.toString(),
        sinceLog: state.logVersion.toString(),
        sinceOverview: state.overviewVersion.toString()
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
        if (controller.signal.aborted || state.suspended) continue;
        const event = decodeEvents(buffer);
        state.backendsVersion = event.backendsVersion;
        state.logVersion = event.logVersion;
        if (event.backendsChanged) await refreshBackends({ quiet: true });
        if (event.logChanged) await fetchSelectedLog();
        if (event.overviewChanged || !state.layoutReady || state.safeSpacesError || state.layoutError || state.refreshErrorTimers.safeSpacesError || state.refreshErrorTimers.layoutError) {
          if (state.layoutSaving || state.groupDrag || state.endpointDrag || state.safeSpacesRefreshing || state.layoutFetching) {
            await delay(150);
            continue;
          }
          await Promise.all([refreshLayout(), refreshSafeSpaces({ quiet: true })]);
          state.overviewVersion = event.overviewVersion;
        }
        if (state.backendError || state.refreshErrorTimers.backendError) await refreshBackends({ quiet: true });
      } catch (error) {
        if (error.name !== "AbortError") await delay(1200);
      }
    }
  }

  updatePage();
  window.addEventListener("hashchange", updatePage);
  function beginEndpointDrag(event, touch) {
    if (!state.layoutReady || state.layoutSaving || state.endpointDrag || state.groupDrag || elements.dialogLayer.childElementCount || event.target.closest("button")) return;
    const source = event.target.closest(".overview-row[data-endpoint-key], .overview-shortcut-wrap[data-endpoint-key]");
    if (!source) return;
    const card = source.closest("[data-group-id]");
    const drag = { source, card, key: source.dataset.endpointKey, groupID: card.dataset.groupId, x: (touch || event).clientX, y: (touch || event).clientY, pointerId: touch ? undefined : event.pointerId, touchId: touch?.identifier, armed: !touch, moved: false, target: null };
    state.endpointDrag = drag;
    if (touch) drag.timer = window.setTimeout(() => {
      if (state.endpointDrag === drag) { drag.armed = true; source.classList.add("endpoint-armed"); }
    }, 260);

  }

  function moveEndpointDrag(x, y, event) {
    const drag = state.endpointDrag;
    if (!drag) return;
    const distance = Math.hypot(x - drag.x, y - drag.y);
    if (!drag.armed) { if (distance > 10) finishEndpointDrag(false); return; }
    event.preventDefault();
    if (!drag.moved && distance < 6) return;
    if (!drag.moved && drag.pointerId !== undefined) drag.source.setPointerCapture(drag.pointerId);
    drag.moved = true;
    cancelLongPress();
    clearTextSelection();
    state.suppressLaunchUntil = Date.now() + 900;
    drag.card.classList.add("endpoint-drag-active");
    drag.source.classList.add("endpoint-drag-source");
    drag.card.querySelectorAll(".endpoint-drop-before, .endpoint-drop-area").forEach(node => node.classList.remove("endpoint-drop-before", "endpoint-drop-area"));
    const hit = document.elementFromPoint(x, y);
    const area = hit?.closest("[data-endpoint-area]");
    drag.target = null;
    if (area?.closest("[data-group-id]") === drag.card) {
      const siblings = [...area.querySelectorAll(":scope > [data-endpoint-key]")].filter(node => node !== drag.source);
      const before = siblings.find(node => {
        const bounds = node.getBoundingClientRect();
        return area.dataset.endpointArea === "list" ? y < bounds.top + bounds.height / 2 : y < bounds.top || (y <= bounds.bottom && x < bounds.left + bounds.width / 2);
      });
      drag.target = { area: area.dataset.endpointArea, beforeKey: before?.dataset.endpointKey };
      if (before) before.classList.add("endpoint-drop-before"); else area.classList.add("endpoint-drop-area");
    }
    if (y < 60) window.scrollBy(0, -16);
    else if (y > window.innerHeight - 60) window.scrollBy(0, 16);
  }

  function finishEndpointDrag(commit) {
    const drag = state.endpointDrag;
    if (!drag) return;
    state.endpointDrag = null;
    window.clearTimeout(drag.timer);
    if (drag.pointerId !== undefined && drag.source.hasPointerCapture(drag.pointerId)) drag.source.releasePointerCapture(drag.pointerId);
    if (drag.moved) state.suppressLaunchUntil = Date.now() + 900;
    if (commit && drag.moved && drag.target) moveCardEndpoint(drag.groupID, drag.key, drag.target.area, drag.target.beforeKey);
    drag.card.classList.remove("endpoint-drag-active");
    drag.card.querySelectorAll(".endpoint-armed, .endpoint-drag-source, .endpoint-drop-before, .endpoint-drop-area").forEach(node => node.classList.remove("endpoint-armed", "endpoint-drag-source", "endpoint-drop-before", "endpoint-drop-area"));
    renderOverview(registeredAddresses());
  }

  elements.overview.addEventListener("pointerdown", event => {
    if (event.pointerType !== "touch" && event.isPrimary && event.button === 0) beginEndpointDrag(event);
  });
  elements.overview.addEventListener("pointermove", event => { if (state.endpointDrag?.pointerId === event.pointerId) moveEndpointDrag(event.clientX, event.clientY, event); }, { passive: false });
  elements.overview.addEventListener("pointerup", event => { if (state.endpointDrag?.pointerId === event.pointerId) finishEndpointDrag(true); });
  elements.overview.addEventListener("pointercancel", event => { if (state.endpointDrag?.pointerId === event.pointerId) finishEndpointDrag(false); });
  elements.overview.addEventListener("lostpointercapture", event => { if (state.endpointDrag?.pointerId === event.pointerId) finishEndpointDrag(false); });
  elements.overview.addEventListener("touchstart", event => { if (event.touches.length === 1) beginEndpointDrag(event, event.touches[0]); else finishEndpointDrag(false); }, { passive: true });
  elements.overview.addEventListener("touchmove", event => {
    const drag = state.endpointDrag;
    if (!drag || drag.touchId === undefined) return;
    if (event.touches.length !== 1 || !event.cancelable) { finishEndpointDrag(false); return; }
    const touch = [...event.touches].find(touch => touch.identifier === drag.touchId);
    if (touch) moveEndpointDrag(touch.clientX, touch.clientY, event);
  }, { passive: false });
  elements.overview.addEventListener("touchend", event => { if ([...event.changedTouches].some(touch => touch.identifier === state.endpointDrag?.touchId)) finishEndpointDrag(true); });
  elements.overview.addEventListener("touchcancel", () => finishEndpointDrag(false));

  function beginGroupDrag(handle, x, y, pointerId, touchId) {
    if (!state.layoutReady || state.layoutSaving) return;
    const card = handle.closest("[data-group-id]");
    const drag = { handle, card, id: card.dataset.groupId, pointerId, touchId, x, y, moved: false, target: null, armed: touchId === undefined, armTimer: 0 };
    state.groupDrag = drag;
    if (!drag.armed) {
      drag.armTimer = window.setTimeout(() => {
        if (state.groupDrag !== drag) return;
        drag.armed = true;
        handle.classList.add("is-armed");
      }, 260);
    } else handle.setPointerCapture(pointerId);
  }

  function moveGroupDrag(x, y, event) {
    const drag = state.groupDrag;
    if (!drag) return;
    const distance = Math.hypot(x - drag.x, y - drag.y);
    if (!drag.armed) {
      if (distance > 10) finishGroupDrag(false);
      return;
    }
    event.preventDefault();
    if (!drag.moved && distance < 6) return;
    drag.moved = true;
    drag.card.classList.add("is-group-dragging");
    elements.overview.querySelectorAll(".group-drop-before, .group-drop-after").forEach(card => card.classList.remove("group-drop-before", "group-drop-after"));
    const target = document.elementFromPoint(x, y)?.closest("[data-group-id]");
    drag.target = target && target !== drag.card ? target.dataset.groupId : null;
    if (drag.target) {
      const cards = [...elements.overview.querySelectorAll("[data-group-id]")];
      target.classList.add(cards.indexOf(drag.card) < cards.indexOf(target) ? "group-drop-after" : "group-drop-before");
    }
    if (y < 60) window.scrollBy(0, -16);
    else if (y > window.innerHeight - 60) window.scrollBy(0, 16);
  }

  elements.overview.addEventListener("pointerdown", event => {
    if (event.pointerType === "touch") return;
    const handle = event.target.closest("[data-group-handle]");
    if (!handle || event.target.closest("button, a, input, select, textarea") || event.button !== 0 || !event.isPrimary || state.groupDrag) return;
    beginGroupDrag(handle, event.clientX, event.clientY, event.pointerId);
    event.preventDefault();
  });
  elements.overview.addEventListener("pointermove", event => {
    if (event.pointerId === state.groupDrag?.pointerId) moveGroupDrag(event.clientX, event.clientY, event);
  }, { passive: false });
  elements.overview.addEventListener("pointerup", event => { if (event.pointerId === state.groupDrag?.pointerId) finishGroupDrag(true); });
  elements.overview.addEventListener("pointercancel", event => { if (event.pointerId === state.groupDrag?.pointerId) finishGroupDrag(false); });
  elements.overview.addEventListener("lostpointercapture", event => { if (event.pointerId === state.groupDrag?.pointerId) finishGroupDrag(false); });
  elements.overview.addEventListener("touchstart", event => {
    if (event.touches.length !== 1) { finishGroupDrag(false); return; }
    const handle = event.target.closest("[data-group-handle]");
    if (!handle || event.target.closest("button, a, input, select, textarea") || state.groupDrag) return;
    const touch = event.touches[0];
    beginGroupDrag(handle, touch.clientX, touch.clientY, undefined, touch.identifier);
  }, { passive: true });
  elements.overview.addEventListener("touchmove", event => {
    const drag = state.groupDrag;
    if (!drag || drag.touchId === undefined) return;
    if (event.touches.length !== 1 || !event.cancelable) { finishGroupDrag(false); return; }
    const touch = [...event.touches].find(touch => touch.identifier === drag.touchId);
    if (touch) moveGroupDrag(touch.clientX, touch.clientY, event);
  }, { passive: false });
  elements.overview.addEventListener("touchend", event => {
    if ([...event.changedTouches].some(touch => touch.identifier === state.groupDrag?.touchId)) finishGroupDrag(true);
  });
  elements.overview.addEventListener("touchcancel", () => { if (state.groupDrag?.touchId !== undefined) finishGroupDrag(false); });
  elements.overview.addEventListener("keydown", event => {
    if (event.key === "Escape") { finishGroupDrag(false); return; }
    const handle = event.target.closest("[data-group-handle]");
    if (!handle || event.target !== handle || !["ArrowLeft", "ArrowRight", "ArrowUp", "ArrowDown"].includes(event.key)) return;
    event.preventDefault();
    const id = handle.closest("[data-group-id]").dataset.groupId;
    const order = [...elements.overview.querySelectorAll("[data-group-id]")].map(card => card.dataset.groupId);
    const from = order.indexOf(id), to = from + (["ArrowLeft", "ArrowUp"].includes(event.key) ? -1 : 1);
    if (to < 0 || to >= order.length) return;
    order.splice(from, 1); order.splice(to, 0, id);
    if (saveGroupOrder(order)) {
      renderOverview(registeredAddresses());
      [...elements.overview.querySelectorAll("[data-group-id]")].find(card => card.dataset.groupId === id)?.querySelector("[data-group-handle]").focus();
    }
  });
  elements.add.addEventListener("click", () => openAddDialog());
  document.body.addEventListener("contextmenu", event => {
    const identity = itemIdentityFromTarget(event.target);
    if (!identity) return;
    event.preventDefault();
    cancelLongPress();
    finishEndpointDrag(false);
    clearPressFeedback();
    if (Date.now() < state.suppressLaunchUntil && elements.dialogLayer.childElementCount) return;
    if (identity) openAppMenu(identity, { x: event.clientX, y: event.clientY }, menuContext(event.target));
  });
  document.body.addEventListener("pointerdown", beginLongPress);
  document.body.addEventListener("pointerdown", beginPressFeedback);
  document.body.addEventListener("dragstart", event => {
    if (event.target.closest("[data-drag-app-key], [data-endpoint-key]")) event.preventDefault();
  });
  document.body.addEventListener("pointermove", moveLongPress);
  document.body.addEventListener("pointermove", movePressFeedback);
  document.body.addEventListener("pointerup", event => { cancelLongPress(); clearPressFeedback(); });
  document.body.addEventListener("pointercancel", () => { cancelLongPress(); clearPressFeedback(); });
  document.body.addEventListener("lostpointercapture", () => { cancelLongPress(); clearPressFeedback(); });
  document.body.addEventListener("selectstart", event => {
    if (event.target.closest?.(".context-menu, .overview-shortcut, .overview-row > a") || itemIdentityFromTarget(event.target) || Date.now() < state.suppressLaunchUntil) event.preventDefault();
  });
  document.body.addEventListener("click", async event => {
    const actionElement = event.target.closest("[data-action]");
    if (!actionElement) return;
    if (actionElement.classList.contains("dialog-backdrop") && event.target !== actionElement) return;
    const action = actionElement.dataset.action;
    if (["toggle-group-pin", "move-pin-earlier", "move-pin-later"].includes(action)) { changeGroupPin(actionElement.dataset.endpointKey, action); return; }
    if (Date.now() < state.suppressLaunchUntil && actionElement.closest(".app-sections, .directory-row, .overview-identity")) {
      event.preventDefault();
      return;
    }
    if (action === "agent-guide") { openAddDialog("agent"); return; }
    if (action === "copy-agent-prompt") { await copyText(agentPrompt("[Your request here]")); return; }
    if (action === "example-prompt") { await copyText(agentExamples[Number(actionElement.dataset.example)] || agentExamples[2]); return; }
    if (action === "setup-opencode") { try { await openOpenCodeSetup(); } catch (error) { toast(error.message || String(error)); } return; }
    if (action === "rename-endpoint") { const item = findItem(actionElement.dataset.appKey); if (item) openRenameEndpoint(item); return; }
    if (action === "show-container-command") { const bounds = actionElement.getBoundingClientRect(); openContainerCommand(actionElement.dataset.safeSpaceId, actionElement.dataset.commandId, { x: bounds.left, y: bounds.bottom }); return; }
    if (action === "setup-container-opencode") { openCreateContainer(true); return; }
    if (action === "container-storage") { await openContainerStorage(); return; }
    if (action === "import-container") { openContainerImport(); return; }
    if (action === "create-container") { openCreateContainer(); return; }
    if (action === "configure-container") { openContainerConfiguration(actionElement.dataset.safeSpaceId); return; }
    if (action === "add-container-app") { await toggleContainerCatalogApp(actionElement.dataset.catalogId); return; }
    if (action === "save-container-catalog") { await saveContainerCatalog(); return; }
    if (action === "reload-container-catalog") { if (state.addWorkspace) await openContainerAdd(state.addWorkspace); return; }
    if (action === "delete-container") { openDeleteContainer(actionElement.dataset.safeSpaceId); return; }
    if (action === "share-container") { openShareContainer(actionElement.dataset.safeSpaceId); return; }
    if (action === "edit-bookmark") { const bounds = actionElement.getBoundingClientRect(); openAppMenu(actionElement.dataset.appKey, { x: bounds.left, y: bounds.bottom + 4 }, menuContext(actionElement)); return; }
    if (action === "copy-url") {
      const item = findItem(actionElement.dataset.appKey);
      if (item) { closeDialog(); await copyText(navigationURL(item.frontend)); }
      return;
    }
    if (action === "edit-container" || action === "edit-container-bookmark") {
      const bounds = actionElement.getBoundingClientRect();
      openContainerMenu(actionElement.dataset.safeSpaceId, actionElement.dataset.serviceId, actionElement.dataset.frontendKey, { x: bounds.left, y: bounds.bottom + 4 }, menuContext(actionElement));
      return;
    }
    if (action === "copy-container-url") {
      const workspace = findSafeSpace(actionElement.dataset.safeSpaceId);
      const app = findSafeSpaceApp(workspace, actionElement.dataset.serviceId, actionElement.dataset.frontendKey);
      if (app) { closeDialog(); await copyText(safeSpaceAppNavigationURL(workspace, app)); }
      return;
    }
    if (action === "uninstall-wsl-app") {
      const workspace = findSafeSpace(actionElement.dataset.safeSpaceId);
      const app = findSafeSpaceApp(workspace, actionElement.dataset.serviceId, actionElement.dataset.frontendKey);
      if (!app || !window.confirm(`Uninstall ${app.displayName}?`)) return;
      actionElement.disabled = true;
      try {
        const result = await control(app, "uninstall", {}, true, workspace);
        if (!result.ok) throw new Error(result.message || "Uninstall failed.");
        closeDialog();
        await refreshSafeSpaces({ quiet: true });
      } catch (error) { toast(error.message || String(error), true); actionElement.disabled = false; }
      return;
    }
    if (action === "container-control") {
      actionElement.disabled = true;
      try {
        const workspace = findSafeSpace(actionElement.dataset.safeSpaceId);
        const app = findSafeSpaceApp(workspace, actionElement.dataset.serviceId, actionElement.dataset.frontendKey);
        await safeSpaceRequest(actionElement.dataset.operation, { workspaceID: actionElement.dataset.safeSpaceId, serviceID: actionElement.dataset.serviceId, serviceScope: app?.serviceScope });
        closeDialog();
        await refreshSafeSpaces({ quiet: true });
      } catch (error) { toast(error.message || String(error), true); actionElement.disabled = false; }
      return;
    }
    if (action === "close-dialog") { closeDialog(); return; }
    if (action === "open-container-add") {
      const workspace = findSafeSpace(actionElement.dataset.safeSpaceId);
      if (workspace) await openContainerAdd(workspace);
      return;
    }
    if (action === "open-wsl-add") { const workspace = findSafeSpace(actionElement.dataset.safeSpaceId); if (workspace) await openWorkspaceAdd(workspace, actionElement.dataset.scope); return; }
    if (action === "open-add") { state.addWorkspace = null; state.addScope = actionElement.dataset.scope || (state.backends.some(backend => backend.serviceID === "org.outershell.OuterShell" && backend.serviceScope === "system") ? "system" : "user"); openAddDialog(); return; }
    if (action === "start-safe-space") {
      try { await startSafeSpace(actionElement.dataset.safeSpaceId); }
      catch (error) { toast(error.message || String(error), true); }
      return;
    }
    if (action === "launch-safe-space-app") {
      const workspace = findSafeSpace(actionElement.dataset.safeSpaceId);
      const app = findSafeSpaceApp(workspace, actionElement.dataset.serviceId, actionElement.dataset.frontendKey);
      if (workspace && app && safeSpaceAppIsReady(workspace, app)) return;
      event.preventDefault();
      try { await launchSafeSpaceApp(actionElement.dataset.safeSpaceId, actionElement.dataset.serviceId, actionElement.dataset.frontendKey); }
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
    if (action === "retry-launch") {
      actionElement.disabled = true;
      try { await launch(actionElement.dataset.appKey, actionElement.dataset.appScope, true); }
      catch (error) { toast(error.message || String(error), true); }
      finally { actionElement.disabled = false; }
      return;
    }
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
    if (event.key === "Escape") { finishEndpointDrag(false); finishGroupDrag(false); }
    if (event.key === "Escape" && elements.dialogLayer.childElementCount) closeDialog();
    if (event.key === "ContextMenu" || (event.shiftKey && event.key === "F10")) {
      const identity = itemIdentityFromTarget(event.target);
      if (!identity) return;
      event.preventDefault();
      const bounds = event.target.getBoundingClientRect();
      openAppMenu(identity, { x: bounds.left, y: bounds.bottom }, menuContext(event.target));
    }
  });
  document.addEventListener("dragover", event => {
    if (Array.from(event.dataTransfer?.types || []).includes("Files")) {
      event.preventDefault();
      event.dataTransfer.dropEffect = "copy";
    }
  });
  document.addEventListener("drop", event => {
    const files = Array.from(event.dataTransfer?.files || []);
    if (!files.length || event.target.closest('input[type="file"]')) return;
    event.preventDefault();
    if (files.length !== 1 || !files[0].name.toLowerCase().endsWith(".outershell-container")) {
      toast("Drop one .outershell-container file to import it.", true);
      return;
    }
    openContainerImport(files[0]);
  });
  window.addEventListener("beforeunload", suspendRefreshes);
  window.addEventListener("pagehide", () => { finishEndpointDrag(false); finishGroupDrag(false); suspendRefreshes(); });
  window.addEventListener("pageshow", resumeRefreshes);
  window.addEventListener("online", () => { suspendRefreshes(); resumeRefreshes(); });
  document.addEventListener("visibilitychange", () => {
    if (document.hidden) { finishEndpointDrag(false); finishGroupDrag(false); suspendRefreshes(); }
    else resumeRefreshes();
  });
  state.initialLoadingTimer = window.setTimeout(() => {
    state.showInitialLoading = true;
    render();
  }, 400);
  Promise.all([refreshLayout(), refreshBackends(), refreshSafeSpaces()]).then(() => {
    watchEvents();
    if (window.location.pathname === "/container-storage" &&
        state.providers.some(provider => provider.capabilities?.supportsStorageCleanup)) openContainerStorage();
  });
})();
