const fields = ["message", "hostname", "platform", "time"];

function readString(view, bytes, index) {
  const record = index * 8;
  const offset = view.getUint32(record, true);
  const length = view.getUint32(record + 4, true);
  if (offset > bytes.byteLength || length > bytes.byteLength - offset) {
    throw new Error("The server returned an invalid greeting.");
  }
  return new TextDecoder().decode(bytes.slice(offset, offset + length));
}

async function refresh() {
  const error = document.querySelector("#error");
  error.textContent = "";
  try {
    const response = await fetch("/api/hello", { cache: "no-store" });
    if (!response.ok) throw new Error(`Request failed (${response.status})`);
    const bytes = await response.arrayBuffer();
    if (bytes.byteLength < 32) throw new Error("The server returned a short greeting.");
    const view = new DataView(bytes);
    fields.forEach((field, index) => {
      document.querySelector(`#${field}`).textContent = readString(view, bytes, index);
    });
  } catch (requestError) {
    error.textContent = requestError instanceof Error ? requestError.message : String(requestError);
  }
}

document.querySelector("#refresh").addEventListener("click", refresh);
refresh();
