async function changeMode() {
  const mode = document.getElementById("modeSelect").value;
  await fetch("/mode", {
    method: "POST",
    body: new URLSearchParams({ mode }),
    headers: { "Content-Type": "application/x-www-form-urlencoded" },
  });
  updateUI(mode);
}

async function sendData() {
  const text = document.getElementById("textInput").value;
  await fetch("/send", {
    method: "POST",
    body: new URLSearchParams({ data: text }),
    headers: { "Content-Type": "application/x-www-form-urlencoded" },
  });
}

async function send(cmd) {
  await fetch("/send", {
    method: "POST",
    body: new URLSearchParams({ data: cmd }),
    headers: { "Content-Type": "application/x-www-form-urlencoded" },
  });
}

async function updateUI(mode = null) {
  if (!mode) {
    const resp = await fetch("/getMode");
    mode = await resp.text();
    document.getElementById("modeSelect").value = mode;
  }
  document.getElementById("keyboard").style.display =
    mode === "keyboard" ? "block" : "none";
  document.getElementById("mouse").style.display =
    mode === "mouse" ? "block" : "none";
}

window.onload = () => updateUI();
