# Clawdmeter with Claude Code in a Docker AI Sandbox

The Clawdmeter app runs on the host: it owns the Bluetooth link to the device
and polls usage. When Claude Code runs in a
[Docker Sandbox](https://docs.docker.com/ai/sandboxes/) (`sbx run claude …`),
the hooks the app installs in the host's `~/.claude/settings.json` never run:
sandboxes don't pick up user-level configuration from the host, and the
sandbox has its own `~/.config`. Usage meters still work, but prompts, tool
activity and stops never reach the app, so Clawd doesn't react and prompts
don't reset the device's idle timer.

This guide connects the two, for a Linux or macOS host (on Windows the app's
state folder is `%LOCALAPPDATA%\Clawdmeter`). The app's state folder is
mounted into the sandbox, and a project-level hook appends hook events to the
app's `events.log`.

## What you get

| Works | Doesn't |
| --- | --- |
| Usage meters (polled by the host app, as before) | Attention alerts (questions, permission prompts, idle notifications) |
| Prompts: Clawd notices you, the screen wakes and the idle timer resets | Clearing an alert by prompting in the sandbox |
| Tool activity (`a`), active sessions (`n`), the long-turn wave on Stop | |

Alerts need the `attention` and `clear-<session>` files that the app's own
hook writes; this hook only writes `events.log`.

## Requirements

- The [`sbx` CLI](https://docs.docker.com/ai/sandboxes/install/).
- The Clawdmeter app installed, running and paired on the host, with Claude
  Code signed in on the host too: the app reads the OAuth token from the
  host's Claude Code config (`~/.claude/.credentials.json`, or the Keychain on
  macOS) to poll usage. Use the same account as in the sandbox.
- `python3` in the sandbox (the Claude Code sandbox image has it).

## 1. Add the hook script to the app's state folder

On the host, save this as `~/.config/clawdmeter/sandbox-hook.py`. It lives
next to `events.log`, so one copy serves every sandbox that mounts the folder.

```python
# Clawdmeter hook for Claude Code in a Docker Sandbox: appends
# "<ts> <hook_event_name> <session_id>" to the events.log next to this file,
# the line the app's own `clawdmeter hook event` writes. Only the event name
# and session id are logged, never the prompt or the tool call. Does nothing
# outside a sandbox, where the app's own hooks already log. Rolls the log to
# events.log.1 past 256 KB, as the app's hook does. Never fails.
import json, os, re, sys, time

def clean(v, default):
    if v is None:
        return default
    s = v if isinstance(v, str) else json.dumps(v)
    return re.sub(r"[^A-Za-z0-9_.-]", "", s)

try:
    if os.environ.get("SANDBOX_NAME"):
        data = json.loads(sys.stdin.read() or "null") or {}
        if isinstance(data, dict):
            log = os.path.join(os.path.dirname(os.path.abspath(__file__)), "events.log")
            if os.path.isfile(log) and os.path.getsize(log) > 256 * 1024:
                os.replace(log, log + ".1")
            line = f"{int(time.time())} {clean(data.get('hook_event_name'), 'unknown')} {clean(data.get('session_id'), '-')}\n"
            with open(log, "a") as f:
                f.write(line)
except Exception:
    pass
```

## 2. Run the sandbox with the state folder mounted

Pass the folder as an extra workspace after the project. Extra workspaces are
mounted read-write at the same absolute path as on the host:

```bash
sbx run claude ~/projects/my-project ~/.config/clawdmeter
```

Mounts are chosen when a sandbox is created; if you already have one for the
project, create a new one with the extra path. Check from inside it
(`sbx exec -it <sandbox-name> bash`) that the host's files are visible:

```bash
ls /home/<you>/.config/clawdmeter/    # your host path: config, events.log, sandbox-hook.py, …
```

## 3. Add the hooks to the project

The sandbox only reads project-level Claude Code configuration, so the hooks
go into the project's `.claude/settings.local.json` on the host. That file
lives in the mounted project, so it survives the sandbox being recreated.
Run this on the host in the project folder; it keeps existing settings and
doesn't add duplicates when run again:

```bash
python3 - <<'EOF'
import json, os, shlex
p = ".claude/settings.local.json"
os.makedirs(".claude", exist_ok=True)
s = json.load(open(p)) if os.path.exists(p) else {}
cmd = "python3 -I " + shlex.quote(os.path.expanduser("~/.config/clawdmeter/sandbox-hook.py"))
hooks = s.setdefault("hooks", {})
for ev in ("UserPromptSubmit", "Stop", "PostToolUse"):
    groups = hooks.setdefault(ev, [])
    if not any(h.get("command") == cmd for g in groups for h in g.get("hooks", [])):
        groups.append({"hooks": [{"type": "command", "command": cmd, "timeout": 5}]})
json.dump(s, open(p, "w"), indent=2)
print("hooks added to", p)
EOF
```

The command uses the host's absolute path, which is also the path inside the
sandbox. Keep `settings.local.json` out of git (Claude Code's convention for
personal settings); if your `.gitignore` doesn't cover it, add it to
`.git/info/exclude`.

The same hooks also run when you use Claude Code on the host in this project.
There the script does nothing, because `SANDBOX_NAME` is only set inside a
sandbox, and the app's own hooks log the events.

Start a new Claude Code session in the sandbox, or open `/hooks` once in a
running one, so it picks up the hooks.

## 4. Check it

1. On the host, follow the log:
   ```bash
   tail -f ~/.config/clawdmeter/events.log
   ```
2. Send a prompt in the sandbox. A line like
   `1792435842 UserPromptSubmit bc18865c-…` appears, then `PostToolUse` lines
   while Claude works and `Stop` when it's done.
3. On the device, Clawd points when you prompt, and a dark screen wakes up.
   The first hook event the device receives after booting (a prompt, or a
   Stop) only sets a baseline, so after a reboot or a fresh flash the
   reactions start from the next event.

To see the device side, open its serial monitor
(`pio device monitor -b 115200`): every event it acts on prints
`hook event 'p' (#N)`.

## Plain Docker

Without `sbx`, the same works in any container: bind-mount the host's
`~/.config/clawdmeter` at the same absolute path
(`-v "$HOME/.config/clawdmeter:$HOME/.config/clawdmeter"`), set
`SANDBOX_NAME` to any value in the container, and add the hooks as in step 3.

## Troubleshooting

| Symptom | Check |
| --- | --- |
| No new lines in the host's `events.log` | Is `~/.config/clawdmeter` mounted (step 2)? Are the hooks in the project's `.claude/settings.local.json`, and was the session started after adding them? Test by hand in the sandbox: `echo '{"hook_event_name":"UserPromptSubmit","session_id":"test"}' \| python3 -I <host-home>/.config/clawdmeter/sandbox-hook.py` |
| Lines appear, but the device doesn't react | Firmware v2.2.2 or older ([#237](https://github.com/HermannBjorgvin/Clawdmeter/issues/237)); or it's the first event after a reboot; or the app isn't connected (check its Device page). |
| Events appear twice | The hook runs both in the sandbox's user settings and in the project settings. Keep only the project entry. |
| Alerts from the sandbox never show | Expected: see [What you get](#what-you-get). |
