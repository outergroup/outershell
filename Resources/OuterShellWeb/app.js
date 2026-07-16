(() => {
  "use strict";

  const decoder = new TextDecoder();
  const elements = {
    shell: document.querySelector("#app"),
    sections: document.querySelector("#app-sections"),
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
    loading: true,
    busy: false,
    query: "",
    addTab: "catalog",
    backendsVersion: 0n,
    logVersion: 0n,
    logSelection: null,
    eventAbort: null,
    stopped: false
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
    return `<span class="list-icon" aria-hidden="true">${content}</span>`;
  }

  function runningBadgesHTML(item) {
    const badges = [];
    if (item.user && endpointRunning(item.user)) badges.push(`<span class="running-badge user-running-badge" title="Running as you" aria-label="Running as you"></span>`);
    if (item.root && endpointRunning(item.root)) badges.push(`<span class="running-badge root-running-badge" title="Running as root" aria-label="Running as root">✓</span>`);
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
    if (socket) return `http+unix://${encodeURIComponent(socket)}${pathAndQuery(frontend)}`;
    if (frontend.port > 0) return `http://127.0.0.1:${frontend.port}${pathAndQuery(frontend)}`;
    return String(frontend.url || "").trim() || "#";
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
    const addTile = query ? "" : `<button class="launcher-tile add-app-tile" type="button" data-action="open-add">
      <span class="add-app-icon" aria-hidden="true"><span></span></span>
      <span class="launcher-name">Add app</span>
    </button>`;
    elements.sections.classList.toggle("single-column", orderedLists.length === 0);
    elements.sections.innerHTML = `
      <section class="launcher-column" aria-label="Apps">
        <div class="launcher-grid">${iconItems.map(renderLauncherTile).join("")}${addTile}</div>
      </section>
      ${orderedLists.length ? `<section class="list-column" aria-label="App lists">${orderedLists.map(renderListGroup).join("")}</section>` : ""}`;
  }

  function slug(value) {
    return String(value).toLocaleLowerCase().replace(/[^a-z0-9]+/g, "-").replace(/^-|-$/g, "") || "apps";
  }

  function renderLauncherTile(item) {
    const readyURL = endpointReady(item.primary) ? navigationURL(item.frontend) : "#";
    return `<article class="launcher-tile">
      <a class="launcher-link" href="${escapeHTML(readyURL)}" data-action="launch" data-app-key="${escapeHTML(item.identity)}" aria-label="Open ${escapeHTML(item.displayName)}"></a>
      <span class="launcher-icon-row">${launcherIconHTML(item)}${runningBadgesHTML(item)}</span>
      <h2 class="launcher-name">${escapeHTML(item.displayName)}</h2>
      <button class="launcher-menu-button" type="button" data-action="details" data-app-key="${escapeHTML(item.identity)}" aria-label="Options for ${escapeHTML(item.displayName)}" title="App options">⋯</button>
    </article>`;
  }

  function renderListGroup([name, items]) {
    return `<section class="list-group" aria-labelledby="list-${slug(name)}">
      <div class="list-widget">${items.map(renderListRow).join("")}</div>
      <h2 id="list-${slug(name)}" class="list-label">${escapeHTML(name)}</h2>
    </section>`;
  }

  function renderListRow(item) {
    const readyURL = endpointReady(item.primary) ? navigationURL(item.frontend) : "#";
    const badges = runningBadgesHTML(item);
    return `<article class="list-row ${badges ? "has-running-badges" : ""}">
      <a class="list-link" href="${escapeHTML(readyURL)}" data-action="launch" data-app-key="${escapeHTML(item.identity)}" aria-label="Open ${escapeHTML(item.displayName)}"></a>
      ${listIconHTML(item)}
      ${badges}
      <h3 class="list-name">${escapeHTML(item.displayName)}</h3>
      <button class="list-menu-button" type="button" data-action="details" data-app-key="${escapeHTML(item.identity)}" aria-label="Options for ${escapeHTML(item.displayName)}" title="App options">⋯</button>
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

  async function refreshBackends({ quiet = false } = {}) {
    if (!quiet) {
      state.loading = true;
      render();
    }
    try {
      const { buffer } = await requestBuffer("/api/backends");
      const result = decodeBackends(buffer);
      state.backends = result.backends;
      showStatus(result.error);
    } catch (error) {
      showStatus(error.message || String(error));
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

  async function launch(identity) {
    let item = findItem(identity);
    if (!item) return;
    if (!endpointReady(item.primary)) {
      toast(`Starting ${item.displayName}…`);
      const action = await control(item.backend, "start");
      if (!action.ok) throw new Error(action.message || `Could not start ${item.displayName}.`);
      for (let attempt = 0; attempt < 30; attempt += 1) {
        await delay(500);
        await refreshBackends({ quiet: true });
        item = findItem(identity);
        if (item && endpointReady(item.primary)) break;
      }
    }
    if (!item || !endpointReady(item.primary)) throw new Error(`Timed out waiting for ${item?.displayName || "the app"}.`);
    window.location.assign(navigationURL(item.frontend));
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
    elements.dialogLayer.replaceChildren();
    const fragment = elements.dialogTemplate.content.cloneNode(true);
    const backdrop = fragment.querySelector(".dialog-backdrop");
    const dialog = fragment.querySelector(".dialog");
    dialog.className = `dialog ${className}`.trim();
    dialog.innerHTML = bodyHTML;
    backdrop.addEventListener("click", event => {
      if (event.target === backdrop) closeDialog();
    });
    elements.dialogLayer.append(fragment);
    window.setTimeout(() => dialog.querySelector("button, input, select, textarea")?.focus(), 0);
    return dialog;
  }

  function closeDialog() {
    elements.dialogLayer.replaceChildren();
    state.logSelection = null;
    restartEventWatch();
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
      const finish = value => { elements.dialogLayer.replaceChildren(); resolve(value); };
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

  function openDetails(identity) {
    const item = findItem(identity);
    if (!item) return;
    const backend = item.backend;
    const running = endpointRunning(item.primary);
    const controlButtons = backend.canControl ? `
      <div class="action-group">
        <button class="primary-button" type="button" data-action="control" data-operation="${running ? "restart" : "start"}" data-backend-key="${escapeHTML(backendKey(backend))}">${running ? "Restart" : "Start"}</button>
        ${running ? `<button class="secondary-button" type="button" data-action="control" data-operation="stop" data-backend-key="${escapeHTML(backendKey(backend))}">Stop</button>` : ""}
        ${backend.logFiles.length ? `<button class="secondary-button" type="button" data-action="logs" data-backend-key="${escapeHTML(backendKey(backend))}">View logs</button>` : ""}
        ${backend.scriptPath ? `<button class="secondary-button" type="button" data-action="copy-script" data-script="${escapeHTML(backend.scriptPath)}">Copy script path</button>` : ""}
      </div>` : "";
    const rootButton = backend.supportsRoot && !backend.rootOnly ? `<button class="secondary-button" type="button" data-action="control" data-operation="${backend.hasRootSupport ? "removeRootSupport" : "addRootSupport"}" data-backend-key="${escapeHTML(backendKey(backend))}">${backend.hasRootSupport ? "Remove root support" : "Add root support"}</button>` : "";
    openDialog(`
      <header class="dialog-header"><div class="dialog-title-wrap"><h2 id="dialog-title">App details</h2></div><button class="dialog-close" type="button" data-action="close-dialog" aria-label="Close">×</button></header>
      <div class="dialog-body">
        <div class="detail-summary">${iconHTML(item)}<div><h3>${escapeHTML(item.displayName)}</h3><p>${escapeHTML(backend.serviceID)}</p></div></div>
        <dl class="detail-list">
          <div class="detail-row"><dt>Status</dt><dd><span class="running-dot ${running ? "" : "stopped-dot"}"></span>${escapeHTML(backend.status || (running ? "running" : "stopped"))}</dd></div>
          <div class="detail-row"><dt>Scope</dt><dd>${escapeHTML(backend.serviceScope || "user")}</dd></div>
          <div class="detail-row"><dt>Endpoint</dt><dd>${escapeHTML(navigationURL(item.frontend))}</dd></div>
          ${backend.scriptPath ? `<div class="detail-row"><dt>Script</dt><dd>${escapeHTML(backend.scriptPath)}</dd></div>` : ""}
        </dl>
        ${controlButtons}
        ${rootButton ? `<div class="action-group">${rootButton}</div>` : ""}
        ${backend.canUninstall ? `<div class="danger-zone"><button class="danger-button" type="button" data-action="uninstall" data-backend-key="${escapeHTML(backendKey(backend))}">Uninstall ${escapeHTML(item.displayName)}</button></div>` : ""}
      </div>`);
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
    if (!backend || !backend.logFiles.length) return;
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
  elements.refresh.addEventListener("click", () => refreshBackends());
  document.body.addEventListener("click", async event => {
    const actionElement = event.target.closest("[data-action]");
    if (!actionElement) return;
    if (actionElement.classList.contains("dialog-backdrop") && event.target !== actionElement) return;
    const action = actionElement.dataset.action;
    if (action === "close-dialog") { closeDialog(); return; }
    if (action === "open-add") { openAddDialog(); return; }
    if (action === "add-tab") { openAddDialog(actionElement.dataset.tab); return; }
    if (action === "details") { openDetails(actionElement.dataset.appKey); return; }
    if (action === "launch") {
      const item = findItem(actionElement.dataset.appKey);
      if (item && endpointReady(item.primary)) return;
      event.preventDefault();
      try { await launch(actionElement.dataset.appKey); } catch (error) { toast(error.message || String(error), true); }
      return;
    }
    if (action === "install") { await installBackend(actionElement.dataset.backendKey, actionElement); return; }
    if (action === "control") { await performControl(actionElement.dataset.backendKey, actionElement.dataset.operation, actionElement); return; }
    if (action === "uninstall") { await uninstallBackend(actionElement.dataset.backendKey, actionElement); return; }
    if (action === "copy-script") { await copyText(actionElement.dataset.script); return; }
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
  });
  window.addEventListener("beforeunload", () => {
    state.stopped = true;
    state.eventAbort?.abort();
  });

  refreshBackends().then(watchEvents);
})();
