# Clawdmeter with Claude Code in a Docker AI Sandbox

The Clawdmeter app runs on the host: it owns the Bluetooth link to the device
and polls usage. When Claude Code runs in a
[Docker Sandbox](https://docs.docker.com/ai/sandboxes/) (`sbx run claude …`),
the hooks the app installs in the host's `~/.claude/settings.json` never run:
sandboxes don't pick up user-level configuration from the host, and the
sandbox has its own `~/.config`. Usage meters still work, but prompts, tool
activity, stops and attention alerts never reach the app, so Clawd doesn't
react and prompts don't reset the device's idle timer.

This guide connects the two, for a Linux or macOS host (on Windows the app's
state folder is `%LOCALAPPDATA%\Clawdmeter`). The app's state folder is
mounted into the sandbox, and a project-level hook mirrors `clawdmeter hook`:
it writes the same `events.log` lines plus the `attention` and
`clear-<session>` files, so everything the host hook does crosses over.

## What you get

Everything the app's own hooks do:

- Usage meters (polled by the host app, as before)
- Prompts: Clawd notices you, the screen wakes and the idle timer resets
- Tool activity (`a`), active sessions (`n`), the long-turn wave on Stop
- Attention alerts: permission prompts, AskUserQuestion, idle and MCP
  requests alert on the device, and responding in the sandbox clears them

One difference to host sessions stays: hook events from the sandbox are
unattributed. The session's transcript path is a sandbox path, not a host
Claude Code config dir, so the app can't tie the activity to an account. It
still polls when you prompt, just through the usage endpoint rather than its
probe message.

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
It takes the same modes as `clawdmeter hook`: `event`, `alert event` and
`clear event`.

```python
# Clawdmeter hook for Claude Code in a Docker Sandbox: mirrors
# `clawdmeter hook <mode>`, the hook the app installs on the host, writing
# the same files in its own directory. `event` appends
# "<ts> <hook_event_name> <session_id> [<account>]" to events.log (rolling
# it to events.log.1 past 256 KB); `alert` writes `attention`
# ({"k","m","s"}); `clear` touches `clear-<session id>`. Only event names,
# the session id and the config dir are recorded, never the prompt or the
# tool call. Does nothing outside a sandbox, where the app's own hooks
# already run. Never fails.
import json, os, re, sys, time

DIR = os.path.dirname(os.path.abspath(__file__))
MISSING = object()


def present(v):
    # jq's `//`: missing, null and false all count as absent.
    return None if v is None or v is False else v


def clean(v, default):
    v = present(v)
    if v is None:
        return default
    s = v if isinstance(v, str) else json.dumps(v, separators=(",", ":"))
    return re.sub(r"[^A-Za-z0-9_.-]", "", s)


def account_of(d):
    # The account's Claude Code config dir: the parent of "projects" in
    # transcript_path. In a sandbox this is a sandbox path, which matches
    # no host account — the app counts the event as unattributed.
    tp = d.get("transcript_path")
    if not isinstance(tp, str):
        return None
    while True:
        parent = os.path.dirname(tp)
        if os.path.basename(tp) == "projects":
            return parent if parent and parent.isprintable() else None
        if parent == tp:
            return None
        tp = parent


def log_event(d):
    name = clean(d.get("hook_event_name"), "unknown")
    sid = clean(d.get("session_id"), "-")
    acct = account_of(d)
    if acct:
        line = f"{int(time.time())} {name} {sid or '-'} {acct}\n"
    else:
        line = f"{int(time.time())} {name} {sid}\n"
    log = os.path.join(DIR, "events.log")
    if os.path.isfile(log) and os.path.getsize(log) > 256 * 1024:
        os.replace(log, log + ".1")
    with open(log, "a") as f:
        f.write(line)


def jkey(v, k):
    # jq field access: null.x is null; indexing a non-object is an error.
    if v is None:
        return None
    if not isinstance(v, dict):
        return MISSING
    return v.get(k)


def alert_body(d):
    m = present(d.get("message"))
    if m is None:
        # .tool_input.questions[0].question, with jq's typing.
        ti = jkey(d, "tool_input")
        if ti is MISSING:
            return None
        qs = jkey(ti, "questions")
        if qs is MISSING:
            return None
        if qs is None or (isinstance(qs, list) and not qs):
            first = None
        elif isinstance(qs, list):
            first = qs[0]
        else:
            return None
        q = jkey(first, "question")
        if q is MISSING:
            return None
        m = present(q)
        if m is None:
            m = ""
    ev, nt = d.get("hook_event_name"), d.get("notification_type")
    if ev == "PreToolUse" and isinstance(ev, str):
        kind = "question"
    elif not isinstance(nt, str) or nt == "permission_prompt":
        kind = "permission"
    elif nt == "idle_prompt":
        kind = "idle"
    elif nt == "elicitation_dialog":
        kind = "elicitation"
    else:
        kind = "other"
    body = json.dumps(
        {"k": kind, "m": m, "s": d.get("session_id")},
        ensure_ascii=False, separators=(",", ":"))
    return body.replace("\x7f", "\\u007f") + "\n"


def write_alert(d):
    body = alert_body(d)
    if body is None:
        return
    tmp = os.path.join(DIR, "attention.%d" % os.getpid())
    with open(tmp, "w") as f:
        f.write(body)
    try:
        os.replace(tmp, os.path.join(DIR, "attention"))
    except OSError:
        os.remove(tmp)
        raise


def clear_name(d):
    sid = d.get("session_id")
    if isinstance(sid, bool):
        sid = "true" if sid else None
    elif isinstance(sid, (int, float)):
        sid = json.dumps(sid, separators=(",", ":"))
    if not isinstance(sid, str):
        return None
    if not sid or sid in (".", "..") or any(c in sid for c in '/\\\0\n'):
        return None
    return "clear-" + sid


def touch_clear(d):
    name = clear_name(d)
    if name is None:
        return
    path = os.path.join(DIR, name)
    open(path, "a").close()
    os.utime(path)


try:
    action = sys.argv[1] if len(sys.argv) > 1 else ""
    if action not in ("alert", "clear", "event"):
        action = ""
    log_too = len(sys.argv) > 2 and sys.argv[2] == "event"
    data = "" if sys.stdin.isatty() else sys.stdin.read()
    if (action or log_too) and os.environ.get("SANDBOX_NAME"):
        try:
            v = json.loads(data)
        except ValueError:
            v = MISSING
        if v is not MISSING and (isinstance(v, dict) or v is None):
            d = v if isinstance(v, dict) else {}
            if log_too:
                log_event(d)
            if action == "alert":
                write_alert(d)
            elif action == "clear":
                touch_clear(d)
            elif action == "event":
                log_event(d)
except Exception:
    pass
```

The `attention` and `clear-<session>` files travel the same mount as
`events.log`: they are regular files plus a rename, which the workspace
passthrough carries like any other write. The app compares their
modification times, so this needs the sandbox clock to be roughly right —
`sbx` keeps it synced to the host.

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
base = "python3 -I " + shlex.quote(os.path.expanduser("~/.config/clawdmeter/sandbox-hook.py"))
hooks = s.setdefault("hooks", {})
# Drop the arg-less command an earlier version of this guide installed.
for groups in hooks.values():
    for g in groups:
        g["hooks"] = [h for h in g.get("hooks", []) if h.get("command") != base]
for ev, groups in list(hooks.items()):
    hooks[ev] = [g for g in groups if g.get("hooks")]
# The same five hooks the app installs (`clawdmeter hook <mode>`).
specs = [
    ("UserPromptSubmit", None, "clear event"),
    ("PostToolUse", None, "clear event"),
    ("Stop", None, "event"),
    ("Notification", None, "alert event"),
    ("PreToolUse", "AskUserQuestion", "alert event"),
]
for ev, matcher, mode in specs:
    cmd = base + " " + mode
    groups = hooks.setdefault(ev, [])
    if not any(h.get("command") == cmd for g in groups for h in g.get("hooks", [])):
        g = {"hooks": [{"type": "command", "command": cmd, "timeout": 5}]}
        if matcher:
            g["matcher"] = matcher
        groups.append(g)
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
4. Attention alerts work the same way: when Claude asks for permission or
   asks a question, the device shows its attention card (and chimes, if you
   enabled that); answering in the sandbox clears it, as does a tap on the
   device.

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
| No new lines in the host's `events.log` | Is `~/.config/clawdmeter` mounted (step 2)? Are the hooks in the project's `.claude/settings.local.json`, and was the session started after adding them? Test by hand in the sandbox: `echo '{"hook_event_name":"UserPromptSubmit","session_id":"test"}' \| python3 -I <host-home>/.config/clawdmeter/sandbox-hook.py event` |
| Lines appear, but the device doesn't react | Firmware v2.2.2 or older ([#237](https://github.com/HermannBjorgvin/Clawdmeter/issues/237)); or it's the first event after a reboot; or the app isn't connected (check its Device page). |
| Events appear twice | The hook runs both in the sandbox's user settings and in the project settings. Keep only the project entry. |
| An alert never shows | The app drops an `attention` file older than 60 s — compare `date` in the sandbox with the host clock. Idle alerts also need `alert_on` in the app config to include `idle`, on the host too. |
| An alert never clears | The clear must be newer than the alert's modification time; again, compare the sandbox clock with the host's. |
