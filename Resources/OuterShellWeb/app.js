(() => {
  "use strict";

  document.documentElement.classList.toggle("outerloop-host", /(^|\.)outerlooplocal$/i.test(window.location.hostname));

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
    backends: [],
    endpointNames: {},
    safeSpaces: [],
    providers: [],
    containerDownloadURL: null,
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
    const source = item.app?.iconData
      ? `data:image/png;base64,${escapeHTML(item.app.iconData)}`
      : dataURL(item.frontend?.iconData);
    const content = source
      ? `<img src="${source}" alt="">`
      : `<span class="launcher-fallback" aria-hidden="true">${escapeHTML(initials(item.displayName).slice(0, 1))}</span>`;
    return `<span class="launcher-icon" aria-hidden="true">${content}</span>`;
  }

  function listIconHTML(item) {
    const source = item.app?.iconData
      ? `data:image/png;base64,${escapeHTML(item.app.iconData)}`
      : dataURL(item.frontend?.iconData);
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

  function loadCardPreferences() {
    try {
      const pins = JSON.parse(localStorage.getItem("outer-shell.group-pins.v1") || "{}");
      if (!pins || Array.isArray(pins) || typeof pins !== "object" || Object.values(pins).some(keys => !Array.isArray(keys) || keys.some(key => typeof key !== "string"))) throw new Error("Invalid shortcuts");
      state.groupPins = pins;
    } catch (error) { toast("Could not load saved shortcuts.", true); }

    try {
      const layout = JSON.parse(localStorage.getItem("outer-shell.endpoint-layout.v1") || "null");
      if (layout) {
        for (const value of [layout.pins, layout.order]) {
          if (!value || typeof value !== "object" || Array.isArray(value) || Object.values(value).some(keys => !Array.isArray(keys) || keys.some(key => typeof key !== "string"))) throw new Error("Invalid endpoint layout");
        }
        state.groupPins = layout.pins;
        state.endpointOrder = layout.order;
      }
    } catch (error) { toast("Could not load endpoint ordering.", true); }
    try {
      const order = JSON.parse(localStorage.getItem("outer-shell.group-order.v1") || "[]");
      if (Array.isArray(order)) state.groupOrder = order.filter(key => typeof key === "string");
    } catch (error) { toast("Could not load the saved group order.", true); }

    try {
      const names = JSON.parse(localStorage.getItem("outer-shell.endpoint-names.v1") || "{}");
      if (names && typeof names === "object" && !Array.isArray(names)) state.endpointNames = Object.fromEntries(Object.entries(names).filter(([, name]) => typeof name === "string" && name.trim()));
    } catch (error) { toast("Could not read endpoint names.", true); }
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
    if (socket) return `http+unix://${encodeURIComponent(socket)}${pathAndQuery(frontend)}`;
    if (Number(frontend.port) > 0) return `http://127.0.0.1:${Number(frontend.port)}${pathAndQuery(frontend)}`;
    return String(frontend.url || "");
  }

  function registeredAddresses() {
    const host = launcherItems(true).map(item => ({
      key: hostBookmarkKey(item), name: item.displayName, item,
      kind: addressKind(item.frontend), address: registeredURL(item.frontend),
      socket: item.frontend.socketPath || "", running: endpointRunning(item.primary),
      serviceID: item.backend.serviceID, groupID: "host", groupName: "This server"
    }));
    const containers = state.safeSpaces.flatMap(workspace => (workspace.apps || []).map(app => {
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
    return `<div class="overview-row" data-endpoint-key="${escapeHTML(entry.key)}"><a href="${escapeHTML(target)}" ${overviewLaunchAttributes(entry)} ${entry.item ? "" : `data-app-key="${escapeHTML(entry.key)}"`}><span class="address-status${entry.running ? " is-running" : ""}" aria-label="${entry.running ? "Running" : "Not running"}"></span>${icon}<span>${escapeHTML(entry.name)}${isThisPage ? ' <small class="this-page-label">This page</small>' : ""}</span></a><button class="endpoint-menu" type="button" ${menu} aria-label="Actions for ${escapeHTML(entry.name)}" aria-haspopup="dialog">•••</button></div>`;
  }

  function endpointGroupID(entry) {
    return entry.workspace ? `container:${entry.workspace.id}` : entry.item.backend.serviceScope === "system" ? "root" : "user";
  }

  function groupPinKeys(id, entries) {
    return state.groupPins[id] ?? ["org.outershell.Files", "org.outershell.Top"].map(serviceID => entries.find(entry => entry.serviceID === serviceID)?.key).filter(Boolean);
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
    try { localStorage.setItem("outer-shell.endpoint-layout.v1", JSON.stringify({ pins, order })); }
    catch (error) { toast("Could not save endpoint layout.", true); return false; }
    state.groupPins = pins;
    state.endpointOrder = order;
    return true;
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
    return `<div class="overview-shortcut-wrap" data-endpoint-key="${escapeHTML(entry.key)}"><a class="overview-shortcut" href="${escapeHTML(target)}" ${overviewLaunchAttributes(entry)} ${entry.item ? "" : `data-app-key="${escapeHTML(identity)}"`}>${icon}<span class="shortcut-title"><span class="address-status${entry.running ? " is-running" : ""}" aria-label="${entry.running ? "Running" : "Not running"}"></span>${escapeHTML(entry.name)}</span></a></div>`;
  }

  function renderOverview(entries) {
    if (state.groupDrag || state.endpointDrag) return;
    const sessionUsername = window.outerLoop?.sessionContext?.username;
    const userName = typeof sessionUsername === "string" ? sessionUsername.trim() : "";
    const groups = [
      { id: "user", name: userName || "Your user", note: "User", entries: entries.filter(entry => entry.item && entry.item.backend.serviceScope !== "system") },
      { id: "root", name: "root", note: "Administrator", entries: entries.filter(entry => entry.item && entry.item.backend.serviceScope === "system") },
      ...state.safeSpaces.map(workspace => ({ id: `container:${workspace.id}`, name: workspace.name || "Container", note: `Container · ${safeSpaceState(workspace)}`, workspace, entries: entries.filter(entry => entry.workspace?.id === workspace.id) }))
    ];
    const ranks = new Map(state.groupOrder.map((id, index) => [id, index]));
    groups.sort((a, b) => (ranks.get(a.id) ?? Infinity) - (ranks.get(b.id) ?? Infinity));
    const markup = groups.map(group => {
      const workspace = group.workspace;
      const id = escapeHTML(workspace?.id || "");
      const preparing = workspace && containerPreparing(workspace);
      const pins = groupPinKeys(group.id, group.entries);
      const order = state.endpointOrder[group.id] || [];
      const ranks = new Map(order.map((key, index) => [key, index]));
      const listed = group.entries.filter(entry => !pins.includes(entry.key)).sort((a, b) => (ranks.get(a.key) ?? Infinity) - (ranks.get(b.key) ?? Infinity));
      const controls = workspace ? `<div class="overview-controls">${!safeSpaceIsRunning(workspace) ? `<button type="button" data-action="start-safe-space" data-safe-space-id="${id}" ${preparing ? "disabled" : ""}>${preparing ? "Starting…" : "Start"}</button>` : ""}<button type="button" data-action="edit-container" data-safe-space-id="${id}" aria-label="Manage ${escapeHTML(group.name)}">Manage…</button></div>` : "";
      const addMore = workspace
        ? `<button class="overview-add-more" type="button" data-action="configure-container" data-safe-space-id="${id}" ${preparing || !managedContainer(workspace) ? "disabled" : ""} ${!managedContainer(workspace) ? 'title="This container is managed externally"' : ""}>Add more to Dockerfile…</button>`
        : `<button class="overview-add-more" type="button" data-action="open-add">Add more…</button>`;
      return `<section class="overview-identity" data-group-id="${escapeHTML(group.id)}"><header class="overview-group-handle" tabindex="0" aria-label="Reorder ${escapeHTML(group.name)}. Hold and drag, or use arrow keys." title="Hold and drag to reorder" data-group-handle><div class="overview-group-heading"><h2>${escapeHTML(group.name)}</h2><p>${escapeHTML(group.note)}</p></div>${controls}</header><div class="overview-shortcuts" data-endpoint-area="pins">${pins.map(key => group.entries.find(entry => entry.key === key)).filter(Boolean).map(overviewShortcut).join("")}</div><div class="overview-list" data-endpoint-area="list">${listed.map(overviewEndpoint).join("") || `<p class="overview-empty">${state.loading || (workspace && state.safeSpacesLoading) ? "Loading…" : group.entries.length ? "" : "No registered endpoints."}</p>`}</div>${workspace?.buildProgress ? `<details class="container-build-progress"><summary>${escapeHTML(workspace.buildProgress.detail || workspace.buildProgress.phase)}</summary><pre>${escapeHTML(workspace.buildProgress.log || "")}</pre></details>` : ""}${addMore}</section>`;
    }).join("") + `<button class="overview-add-container" type="button" data-action="create-container"><span aria-hidden="true">+</span>Add container…</button>`;
    if (state.overviewMarkup !== markup) {
      elements.overview.innerHTML = markup;
      state.overviewMarkup = markup;
    }
  }

  function saveGroupOrder(order) {
    try { localStorage.setItem("outer-shell.group-order.v1", JSON.stringify(order)); }
    catch (error) { toast("Could not save the group order.", true); return false; }
    state.groupOrder = order;
    return true;
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
    const containers = state.safeSpaces.flatMap(workspace => (workspace.apps || []).map(app => ({
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
    return socket ? `http+unix://${encodeURIComponent(socket)}${path}` : "#";
  }

  function safeSpaceAppNavigationURL(workspace, app) {
    const target = safeSpaceAppURL(app);
    if (target === "#") return target;
    if (window.outerLoopPageIconObservationSupported === true && !app.iconData && app.iconObservationToken) {
      const callback = new URL("/api/icon-observation", window.location.href);
      callback.search = new URLSearchParams({ token: app.iconObservationToken }).toString();
      return `outerloop://observe-page-icon?${new URLSearchParams({ url: target, callback: callback.href })}`;
    }
    if (window.outerLoopFriendlyNavigationSupported !== true) return target;
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
    const dialog = openDialog(`${containerDialogHeader("Rename endpoint")}<form><div class="dialog-body"><label class="field">Name<input name="name" required value="${escapeHTML(item.displayName)}" autocomplete="off"></label><p class="field-note">Saved in this browser. Used in endpoint lists and pinned shortcuts.</p><p class="container-message" role="status"></p></div><footer class="dialog-footer"><button type="button" data-action="close-dialog">Cancel</button><button type="submit" class="primary-button">Rename</button></footer></form>`);
    const form = dialog.querySelector("form");
    form.elements.name.select();
    form.addEventListener("submit", event => {
      event.preventDefault();
      const name = form.elements.name.value.trim();
      const message = form.querySelector(".container-message");
      if (!name) { message.textContent = "Enter a name."; return; }
      const names = { ...state.endpointNames, [key]: name };
      try { localStorage.setItem("outer-shell.endpoint-names.v1", JSON.stringify(names)); }
      catch (error) { message.textContent = "Could not save the name. Please try again."; return; }
      state.endpointNames = names;
      closeDialog();
      render();
    });
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

  async function safeSpaceRequest(operation, values = {}, signal) {
    const requestID = makeRequestID();
    const response = await fetch("/api/safe-spaces", {
      method: "POST",
      cache: "no-store",
      headers: { "Content-Type": "application/json" },
      signal,
      body: JSON.stringify({ requestID, operation, ...values })
    });
    const result = await response.json();
    if (!response.ok || result.error) throw new Error(result.error || `Outer Shell returned HTTP ${response.status}.`);
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
      if (app.isRunning !== true) await safeSpaceRequest("startApp", { workspaceID, serviceID });
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
      const { buffer } = await requestBuffer("/api/backends", { signal: controller.signal });
      if (state.refreshAbort.backendError !== controller) return;
      const result = decodeBackends(buffer);
      state.backends = result.backends;
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
    state.eventAbort?.abort();
    for (const key of ["backendError", "safeSpacesError"]) {
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
    refreshBackends({ quiet: true });
    refreshSafeSpaces({ quiet: true });
  }

  function showStatus(message = "") {
    elements.status.textContent = message;
    elements.status.hidden = !message;
  }

  function updateStatus() {
    if (!state.stopped) showStatus([...new Set([state.backendError, state.safeSpacesError].filter(Boolean))].join("\n"));
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
    if (backend.supportsRoot && backend.serviceScope !== "system" && !backend.rootOnly) {
      management.push(`<button class="context-menu-item" type="button" data-action="control" data-operation="${backend.hasRootSupport ? "removeRootSupport" : "addRootSupport"}" data-backend-key="${escapeHTML(backendKey(backend))}" role="menuitem">${contextMenuGlyph("◇")}<span>${backend.hasRootSupport ? "Remove root support" : "Add root shortcut"}</span></button>`);
    }
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

  function runtimeOptions(selected = "") {
    return state.providers.map(provider => `<option value="${escapeHTML(provider.id)}" ${provider.id === selected ? "selected" : ""} ${provider.isAvailable ? "" : "disabled"}>${escapeHTML(provider.name)}${provider.isAvailable ? "" : " (unavailable)"}</option>`).join("");
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

  function openCreateContainer() {
    const provider = state.providers.find(value => value.isAvailable);
    const dialog = openDialog(`${containerDialogHeader("New container")}
      <form class="container-form"><div class="dialog-body form-grid">
        <label class="field full">Name<input name="name" required autocomplete="off"></label>
        <label class="field full">Runtime<select name="runtimeProviderID">${runtimeOptions(provider?.id)}</select></label>
        <label class="field full">Base image<input name="baseImage" value="${escapeHTML(provider?.defaultBaseImage || "")}" required spellcheck="false"></label>
        <label class="field">Max CPUs<input name="cpus" type="number" min="1" step="1" value="4" required></label>
        <label class="field">Max Memory (GB)<input name="memoryInGB" type="number" min="1" step="1" value="8" required></label>
        <p class="field-note full">The container is built on this server. You can edit its Dockerfile and runtime configuration after creation.</p>
        <p class="container-message full" role="status">${provider ? "" : "No container runtime is available on this server."}</p>
      </div><footer class="dialog-footer"><button type="button" data-action="close-dialog">Cancel</button><button class="primary-button" type="submit" ${provider ? "" : "disabled"}>Create</button></footer></form>`);
    const form = dialog.querySelector("form");
    form.addEventListener("submit", async event => {
      event.preventDefault();
      const data = new FormData(form);
      const button = form.querySelector('[type="submit"]');
      button.disabled = true;
      const message = form.querySelector(".container-message");
      message.textContent = "Creating container…";
      try {
        await containerOperation("create", { name: String(data.get("name")).trim(), runtimeProviderID: data.get("runtimeProviderID"), baseImage: String(data.get("baseImage")).trim(), cpus: Number(data.get("cpus")), memoryInGB: Number(data.get("memoryInGB")) });
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

  function openContainerConfiguration(id) {
    const workspace = findSafeSpace(id);
    if (!workspace || !managedContainer(workspace)) return;
    const recipe = workspace.recipe;
    if (!recipe || typeof recipe.containerfile !== "string") { toast("The container configuration is not available yet.", true); return; }
    const infrastructure = (workspace.mounts || []).filter(mount => mount.isInfrastructure);
    const tabs = [["dockerfile", "Dockerfile"], ["mounts", "Folder mounts"], ["environment", "Environment"], ["ports", "Ports"], ["runtime", "Runtime"]];
    const dialog = openDialog(`${containerDialogHeader(`Edit ${workspace.name}`)}
      <form class="container-form" novalidate><div class="dialog-body">
        <label class="field">Name<input name="name" value="${escapeHTML(workspace.name)}" required></label>
        <div class="configuration-tabs" role="tablist" aria-label="Container configuration">${tabs.map(([key, label], index) => `<button type="button" role="tab" id="config-tab-${key}" aria-controls="config-panel-${key}" aria-selected="${index === 0}" data-config-tab="${key}">${label}</button>`).join("")}</div>
        <section role="tabpanel" id="config-panel-dockerfile" aria-labelledby="config-tab-dockerfile" data-config-panel="dockerfile"><label class="field">Dockerfile<textarea name="dockerfile" class="dockerfile-editor" spellcheck="false" autocapitalize="off" autocorrect="off">${escapeHTML(recipe.containerfile)}</textarea></label><p class="field-note">Save stores the definition. Save &amp; rebuild applies it to the container.</p></section>
        <section role="tabpanel" id="config-panel-mounts" aria-labelledby="config-tab-mounts" data-config-panel="mounts" hidden><p class="field-note">Server folders are paths on the machine running Outer Shell. They must already exist.</p><div data-rows="mounts">${(workspace.mounts || []).filter(mount => !mount.isInfrastructure).map(mount => configurationRow("mounts", mount)).join("")}</div><button type="button" data-add-row="mounts">Add folder mount</button>${infrastructure.length ? `<details class="managed-mounts"><summary>Managed mounts (${infrastructure.length})</summary>${infrastructure.map(mount => `<p><code>${escapeHTML(mount.hostPath)} → ${escapeHTML(mount.guestPath)}</code></p>`).join("")}</details>` : ""}</section>
        <section role="tabpanel" id="config-panel-environment" aria-labelledby="config-tab-environment" data-config-panel="environment" hidden><div data-rows="environment">${(recipe.environment || []).map(value => configurationRow("environment", value)).join("")}</div><button type="button" data-add-row="environment">Add variable</button></section>
        <section role="tabpanel" id="config-panel-ports" aria-labelledby="config-tab-ports" data-config-panel="ports" hidden><p class="field-note">Publish a container TCP port on this server.</p><div data-rows="ports">${(recipe.publishedPorts || []).map(value => configurationRow("ports", value)).join("")}</div><button type="button" data-add-row="ports">Add port</button></section>
        <section role="tabpanel" id="config-panel-runtime" aria-labelledby="config-tab-runtime" data-config-panel="runtime" hidden><p>${escapeHTML(safeSpaceRuntimeDescription(workspace))} · ${escapeHTML(workspace.cpus)} CPUs · ${escapeHTML(workspace.memoryInGB)} GB</p><label class="field">Runtime<select name="runtimeProviderID">${runtimeOptions(workspace.runtime?.providerID)}</select></label><p class="field-note">Changing runtime saves the configuration and rebuilds the container with the selected provider.</p><button type="submit" value="runtime">Save &amp; change runtime</button></section>
        <p class="container-message" role="status">${recipe.needsRebuild ? "Saved changes need a rebuild." : ""}</p>
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
        message.textContent = action === "save" ? "Saved. Rebuild to apply configuration changes." : "Request completed. Build status appears under All endpoints.";
      } catch (error) { message.textContent = `${saved ? "Some changes were saved. " : ""}${error.message}`; }
      finally { controls.forEach(control => { control.disabled = false; }); }
    });
  }

  function containerTransferRequest(transferID, offset, length) {
    if (!/^[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}$/i.test(transferID)) throw new Error("Invalid container transfer identifier.");
    const bytes = new Uint8Array(48);
    const view = new DataView(bytes.buffer);
    bytes.set([0x4f, 0x53, 0x43, 0x54]);
    view.setUint16(4, 1, true); view.setUint16(6, 1, true);
    bytes.set(transferID.replaceAll("-", "").match(/../g).map(value => parseInt(value, 16)), 8);
    view.setBigUint64(24, BigInt(offset), true); view.setBigUint64(32, BigInt(length), true);
    return bytes;
  }

  function decodeContainerTransfer(buffer, offset, total) {
    if (buffer.byteLength < 40) throw new Error("Incomplete container transfer response.");
    const reader = new PayloadReader(buffer);
    if (reader.u32(0) !== 0x5443534f || reader.view.getUint16(4, true) !== 1) throw new Error("Invalid container transfer response.");
    if (reader.view.getUint16(6, true)) throw new Error(reader.stringRef(32) || "Container download failed.");
    const bytes = reader.bytesRef(24);
    const next = Number(reader.u64(8));
    if (Number(reader.u64(16)) !== total || next !== offset + bytes.length || next > total || (next < total && next <= offset)) throw new Error("Container download size changed or transfer made no progress.");
    return { bytes, next };
  }

  function openShareContainer(id) {
    const workspace = findSafeSpace(id);
    if (!workspace || !managedContainer(workspace)) return;
    const dialog = openDialog(`${containerDialogHeader("Share Container")}
      <form class="container-form"><div class="dialog-body">
        <p>Create a portable archive of ${escapeHTML(workspace.name)} with its Dockerfile and build-context files.</p>
        <label class="container-check"><input name="persistent" type="checkbox">Include persistent data</label>
        <label class="container-check"><input name="mounts" type="checkbox">Include mounted folders from this server</label>
        <p class="field-note">The archive includes configured environment values. Only include data you intend to share. Without a browser file picker, the download is assembled in browser memory.</p>
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
      let writer;
      try {
        if (typeof window.showSaveFilePicker === "function") {
          const handle = await window.showSaveFilePicker({ suggestedName: `${workspace.name.replace(/[^a-zA-Z0-9_-]/g, "-") || "Container"}.outershell-container` });
          writer = await handle.createWritable();
        }
        message.textContent = "Preparing archive…";
        const result = await containerOperation("prepareShare", { workspaceID: id, includePersistentData, includeMountedFolders });
        const total = Number(result.byteCount);
        if (!Number.isSafeInteger(total) || total <= 0) throw new Error("Invalid shared container size.");
        const parts = [];
        let offset = 0;
        while (offset < total) {
          const response = await fetch("/api/safe-spaces", { method: "POST", headers: { "Content-Type": "application/octet-stream" }, body: containerTransferRequest(result.transferID, offset, 4 * 1024 * 1024) });
          if (!response.ok) throw new Error(`Container download returned HTTP ${response.status}.`);
          const chunk = decodeContainerTransfer(await response.arrayBuffer(), offset, total);
          if (writer) await writer.write(chunk.bytes);
          else parts.push(chunk.bytes);
          offset = chunk.next;
          message.textContent = `Downloading archive… ${Math.round(offset / total * 100)}%`;
        }
        if (writer) { await writer.close(); writer = null; message.textContent = "Container archive saved."; }
        else {
          const file = new File(parts, result.fileName || "Container.outershell-container", { type: "application/octet-stream" });
          if (state.containerDownloadURL) URL.revokeObjectURL(state.containerDownloadURL);
          state.containerDownloadURL = URL.createObjectURL(file);
          const output = form.querySelector(".container-download");
          output.replaceChildren();
          const link = document.createElement("a"); link.href = state.containerDownloadURL; link.download = file.name; link.textContent = `Download ${file.name}`; output.append(link);
          if (typeof navigator.canShare === "function" && navigator.canShare({ files: [file] })) {
            const share = document.createElement("button"); share.type = "button"; share.textContent = "Share…";
            share.addEventListener("click", async () => {
              try { await navigator.share({ files: [file], title: workspace.name }); }
              catch (error) { if (error.name !== "AbortError") message.textContent = error.message; }
            });
            output.append(share);
          }
          message.textContent = "Archive ready.";
        }
      } catch (error) {
        if (writer) { try { await writer.abort(); } catch {} }
        message.textContent = error.name === "AbortError" ? "Cancelled." : error.message;
      } finally { button.disabled = false; }
    });
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
        ${app && url !== "#" ? `<button class="context-menu-item" type="button" data-action="copy-container-url" ${attributes} role="menuitem">${contextMenuGlyph("⧉")}<span>Copy URL</span></button>` : ""}
      </section>`, point, `${name} actions`);
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
      if (state.suspended) { await delay(1000); continue; }
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
        if (controller.signal.aborted || state.suspended) continue;
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

  loadCardPreferences();
  updatePage();
  window.addEventListener("hashchange", updatePage);
  function beginEndpointDrag(event, touch) {
    if (state.endpointDrag || state.groupDrag || elements.dialogLayer.childElementCount || event.target.closest("button")) return;
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
    if (action === "rename-endpoint") { const item = findItem(actionElement.dataset.appKey); if (item) openRenameEndpoint(item); return; }
    if (action === "create-container") { openCreateContainer(); return; }
    if (action === "configure-container") { openContainerConfiguration(actionElement.dataset.safeSpaceId); return; }
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
    if (action === "container-control") {
      actionElement.disabled = true;
      try {
        await safeSpaceRequest(actionElement.dataset.operation, { workspaceID: actionElement.dataset.safeSpaceId, serviceID: actionElement.dataset.serviceId });
        closeDialog();
        await refreshSafeSpaces({ quiet: true });
      } catch (error) { toast(error.message || String(error), true); actionElement.disabled = false; }
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
  window.addEventListener("beforeunload", suspendRefreshes);
  window.addEventListener("pagehide", () => { finishEndpointDrag(false); finishGroupDrag(false); suspendRefreshes(); });
  window.addEventListener("pageshow", resumeRefreshes);
  window.addEventListener("online", () => { suspendRefreshes(); resumeRefreshes(); });
  document.addEventListener("visibilitychange", () => {
    if (document.hidden) { finishEndpointDrag(false); finishGroupDrag(false); suspendRefreshes(); }
    else resumeRefreshes();
  });
  state.safeSpacesTimer = window.setInterval(() => {
    if (document.hidden || state.suspended) return;
    if (!state.safeSpaceBusy.size) refreshSafeSpaces({ quiet: true });
    if (state.backendError || state.refreshErrorTimers.backendError) refreshBackends({ quiet: true });
  }, 2000);

  Promise.all([refreshBackends(), refreshSafeSpaces()]).then(watchEvents);
})();
