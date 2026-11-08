# Phone remote control

Control the browser from a phone, tablet or computer on the same local
network, with any web browser: a trackpad, an address bar, a text box that
types into the focused field, and the browser's buttons. Nothing to install.

## Use

1. Menu → **Remote control: Off** switches it on. A notice shows the address
   to open on the phone (for example `http://192.168.1.20:17780`) and a
   six-digit PIN.
2. Open that address on the phone and enter the PIN. The notice on the TV
   closes by itself once the phone is paired.
3. On the phone:
   - **Trackpad**: drag to move the pointer, tap to click, two fingers to
     scroll.
   - **Address bar**: *Go* opens the address (or a search) in the current
     tab, *+Tab* in a new tab.
   - **Text box**: *Send* types the text into the field focused on the page
     (or into the open on-screen keyboard); *⏎* sends it and presses Enter.
   - **Buttons**: back, forward, reload, Esc (the Circle button), previous /
     next / new / close tab, page up / down, fullscreen, menu.

On a computer (mouse or trackpad):

- Drag on the pad with the mouse to move the pointer, click the pad to
  click, and use the wheel (or two fingers on a laptop trackpad) to scroll.
- **Capture mouse** turns the computer's mouse into the TV pointer (pointer
  lock): move, left click to click, right click for Esc, the side buttons for
  back / forward, wheel to scroll. Esc releases it.
- Keyboard shortcuts while no text box has the focus: arrows scroll,
  Space / PgDn and Shift+Space / PgUp page, Backspace or Alt+← back, Alt+→
  forward, `[` and `]` switch tabs, M menu, F fullscreen, Esc close, Enter
  click, `/` jumps to the address bar, I to the text box. In the address bar
  Ctrl+Enter opens a new tab.

The page adapts to the screen: one column on an upright phone or tablet,
the pad beside the controls on a phone held sideways, a landscape tablet or
a computer, and a compact layout on short landscape screens.

Menu → **Show remote PIN** shows the address and PIN again, **Disconnect
phones** forgets the paired phones (they must enter the PIN again) and
**Remote control: On** switches it off.

## Security

- **Off by default**, and off again at every launch. While off, nothing
  listens.
- **A new PIN every time** it is switched on. After 5 wrong PINs every PIN
  is refused until the remote control is switched off and on again, so the
  PIN cannot be guessed over the network. A paired phone gets a random
  128-bit token that every request carries; switching off forgets them all.
- **No encryption.** The page and the commands travel as plain HTTP over the
  local network: anyone on the same Wi-Fi could read what is typed from the
  phone. Do not type passwords from the phone. (A self-signed HTTPS
  certificate would only trade this for a browser warning on every phone.)
- The server accepts at most 32 connections, 64 KiB per request and 256
  queued commands; it serves only the phone page and the JSON API below.

## Protocol

Port 17780 on all interfaces (`--remote-port` on the host build). Requests
other than `GET /` and `POST /api/pair` need the token in an `X-PW-Token`
header (401 otherwise).

| Request | Body | Answer |
| --- | --- | --- |
| `GET /` | | the phone page |
| `POST /api/pair` | `{"pin": "123456"}` | 200 `{"token"}`, 403 wrong PIN, 429 locked |
| `GET /api/status` | | `{url, title, loading, tabs, active_tab, focus, text_input_focused}` |
| `POST /api/command` | a command or an array of commands | 200, 400 bad command, 503 queue full |

Commands (`core::RemoteCommand`):

```json
{"type": "open", "text": "example.com", "new_tab": false}
{"type": "text", "text": "hola", "submit": true}
{"type": "pointer", "dx": 12, "dy": -4}
{"type": "scroll", "dx": 0, "dy": 300}
{"type": "click"}
{"type": "action", "name": "back"}
```

Action names: `back`, `forward`, `reload`, `prev_tab`, `next_tab`, `new_tab`,
`close_tab`, `page_up`, `page_down`, `fullscreen`, `menu`, `escape`. Text is
limited to 8 KiB, deltas to ±4000 px.

## Code

- `src/core/remote_command.cpp`: command parsing and limits.
- `src/remote/`: the HTTP parser, PIN pairing, the API, the phone page and a
  small poll()-based server thread (`RemoteServer`); `RemoteService` ties them
  together behind `shell::RemoteControl`.
- `ShellController::remote_command()` runs the commands on the shell thread;
  the frontends call `RemoteService::pump()` every frame.

The server runs in the UI process. Listening on the local network from the
console's UI process has not been verified yet; if the console refuses it,
the menu shows "Remote control unavailable" with the reason.
