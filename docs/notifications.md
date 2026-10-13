# Notifications

How to make pychron send a message when a run fails and when a queue ends.
Written for the lab manager: no programming is needed, only a text editor, a
terminal and about twenty minutes.

## What you get

| Event | When | Says |
|---|---|---|
| `run_failed` | A run fails, or its record could not be saved | The run, the queue, the row, the error |
| `queue_ended` | The queue finishes, is stopped or fails | How it ended, and one line per run |

Messages go to the addresses in `to`, and to the `email` written in the
queue (the person who started it). The recipients can use any mail system:
Gmail, Outlook, a university account. Nothing has to be set up on their side.

## Which way to send

Pychron does not send from a Gmail or Outlook account directly. Both now
need a browser sign-in that an unattended instrument computer cannot do
reliably. Instead it hands each message to a mail service, which delivers it.
You make one free account with the service and give pychron its key.

| You have | Use | Section |
|---|---|---|
| No control over your institution's DNS (most labs) | Brevo | [Set up Brevo](#set-up-brevo) |
| A domain whose DNS records you can edit | Resend or Postmark | [Resend](#resend), [Postmark](#postmark) |
| A mail server your IT department gave you | SMTP | [SMTP](#smtp) |
| A Slack channel, or your own script | Webhook or command | [Other channels](#other-channels) |

If you are not sure, use Brevo.

Free-plan sizes and the menus named below are as of October 2026. A lab
sends a few messages a day, far under any free plan. If a menu has moved,
search the service's help for the words in bold.

## Set up Brevo

### 1. Make the account

1. Decide which address the messages come from. A shared lab address is
   best (`argon-lab@your-university.edu`), so the setup does not leave with
   one person. You must be able to read mail sent to it.
2. Go to <https://www.brevo.com> and sign up for the free plan with that
   address. Brevo asks for a name, a postal address and sometimes a phone
   number before it lets an account send.

### 2. Confirm the sender

1. Click your account name (top right), then **Senders, Domains & Dedicated
   IPs**.
2. On the **Senders** tab, check that your address is listed. If not, click
   **Add a sender**, enter a name (`Argon Lab Pychron`) and the address.
3. Brevo mails that address. Open the message and confirm.

Recipients will see the name you entered. Because your institution's domain
is not set up for Brevo, the address beside it is rewritten to one ending in
`brevosend.com`. That is expected and the messages are delivered. Replies do
not reach you; nobody needs to reply to these.

### 3. Create the key

1. Click your account name, then **SMTP & API**.
2. Open the **API Keys** tab and click **Generate a new API key**. Name it
   after the instrument computer (`pychron-argus`).
3. Copy the key now. It starts with `xkeysib-` and is shown only this once.
   If you lose it, delete it and generate another.
4. On the **Security** tab, find **Blocking unauthorized IP addresses** (or
   **Authorized IPs**). Either add the instrument computer's public address
   or switch the blocking off. Left on, Brevo refuses the computer and the
   test in step 6 fails with `unrecognised IP address`.

The key lets its holder send mail as your lab. Treat it like a password: do
not put it in `notifications.toml`, in email, or in a shared drive.

### 4. Give the key to pychron

Pychron reads the key from an environment variable named
`PYCHRON_MAIL_API_KEY`. Set it for the account that runs pychron on the
instrument computer, in the way for that system. Replace `xkeysib-...` with
your key.

**Windows.** In a Command Prompt:

```bat
setx PYCHRON_MAIL_API_KEY "xkeysib-..."
```

Close every pychron window and terminal and open them again.

**Linux.** Create the file `~/.config/environment.d/pychron.conf` holding one
line:

```
PYCHRON_MAIL_API_KEY=xkeysib-...
```

Then make it private and log out and in again:

```bash
chmod 600 ~/.config/environment.d/pychron.conf
```

**macOS.** Add this line to the end of the file `~/.zshrc`:

```bash
export PYCHRON_MAIL_API_KEY="xkeysib-..."
```

That covers `elctl` and a `pychron-ui` started from a terminal. A
`pychron-ui` started from the Dock or Finder does not read `~/.zshrc`. For
it, also run this once after each restart of the computer, before opening
pychron:

```bash
launchctl setenv PYCHRON_MAIL_API_KEY "xkeysib-..."
```

### 5. Write notifications.toml

In the lab's configuration folder (the one holding `experiment.toml`; `elctl
doctor` prints it as `install folder`), copy `notifications.toml.example` to
`notifications.toml` and make it read:

```toml
[[email]]
provider = "brevo"
api_key_env = "PYCHRON_MAIL_API_KEY"
from = "argon-lab@your-university.edu"     # the sender you confirmed in step 2
to = ["you@your-university.edu"]           # who always gets the messages
```

Note that `api_key_env` is the *name* of the variable, not the key.

### 6. Test

In a new terminal:

```bash
elctl exp notify
```

With more than one install, name it: `elctl --install argus exp notify`.

- `sent: email` and a message titled "pychron: test notification" in your
  inbox within a minute: done. Check the spam folder the first time, and
  mark the message "not spam".
- `failed: email: ...`: find the text in [Troubleshooting](#troubleshooting).

Then test from the program people actually use: in the experiment window,
**Experiment > Executor > Send Test Notification**. The result appears in the event list
of the executor pane. This matters because the window may have been started
without the variable (step 4).

`elctl doctor` also warns when a channel's variable is not set.

## Settings

Every `[[email]]` entry takes:

| Key | Meaning | Default |
|---|---|---|
| `provider` | `"brevo"`, `"resend"` or `"postmark"`. Leave out for SMTP | SMTP |
| `api_key_env` | Variable holding the service's key (with `provider`) | required |
| `from` | The sender address, confirmed with the service | required |
| `to` | Addresses that always get the messages | none |
| `queue_user` | Also send to the `email` written in the queue | `true` |
| `on` | Which events: `"run_failed"`, `"queue_ended"` | both |
| `name` | What the channel is called in the event list and by `doctor` | `"email"` |

Several entries are allowed, for example failures to the manager only:

```toml
[[email]]
name = "everything"
provider = "brevo"
api_key_env = "PYCHRON_MAIL_API_KEY"
from = "argon-lab@your-university.edu"

[[email]]
name = "failures"
provider = "brevo"
api_key_env = "PYCHRON_MAIL_API_KEY"
from = "argon-lab@your-university.edu"
to = ["manager@your-university.edu"]
queue_user = false
on = ["run_failed"]
```

At the top of the file, `timeout = 30` is the seconds allowed per message and
`curl = "curl"` the program that sends it. A misspelled key is reported when
the file is loaded, not ignored.

## Resend

Needs a domain you control: Resend sends only from a domain whose DNS
records you have added.

1. Sign up at <https://resend.com>.
2. **Domains > Add Domain**, then add the DNS records it lists at your
   domain's registrar and wait for "Verified".
3. **API Keys > Create API Key** with "Sending access". The key starts with
   `re_`.
4. Set the variable as in [step 4](#4-give-the-key-to-pychron), and:

```toml
[[email]]
provider = "resend"
api_key_env = "PYCHRON_MAIL_API_KEY"
from = "pychron@your-domain.org"           # any address at the verified domain
to = ["you@your-university.edu"]
```

## Postmark

1. Sign up at <https://postmarkapp.com>. A new account is in a test mode
   that limits who it can send to until Postmark approves it; request
   approval and say the mail is instrument alerts to lab members.
2. **Sender Signatures > Add Domain or Signature**: confirm one address by
   the link Postmark mails to it, or verify a whole domain with DNS records.
3. Open the server Postmark made for you, then **API Tokens**, and copy the
   **Server API token**.
4. Set the variable as in [step 4](#4-give-the-key-to-pychron), and:

```toml
[[email]]
provider = "postmark"
api_key_env = "PYCHRON_MAIL_API_KEY"
from = "argon-lab@your-university.edu"     # the confirmed signature
to = ["you@your-university.edu"]
```

## SMTP

For a mail server IT runs for you, or any service not listed above (Amazon
SES, Mailgun, SendGrid and SMTP2GO all accept SMTP, with a key as the
password). Ask for the server name, the port, and a user name and password.

```toml
[[email]]
url = "smtp://smtp.your-university.edu:587"  # smtps://host:465 for port 465
from = "argon-lab@your-university.edu"
to = ["you@your-university.edu"]
username = "argon-lab"
password_env = "PYCHRON_SMTP_PASSWORD"       # set like the key in step 4
```

`smtp://` insists on an encrypted connection (STARTTLS); `tls = false`
allows a plain one, for a relay inside the building only. Leave out
`username` and `password_env` for a relay that asks for none.

A Gmail account works this way too, with an "app password" (the account
needs 2-Step Verification): `url = "smtps://smtp.gmail.com:465"`, the full
address as `username`. Outlook.com and Microsoft 365 accounts no longer
accept a password over SMTP; use a mail service instead.

## Other channels

A webhook posts each message to a URL; `format = "slack"` suits a Slack
incoming webhook, and the default `"json"` posts every field.

```toml
[[webhook]]
name = "slack"
url = "https://hooks.slack.com/services/T000/B000/XXXX"
format = "slack"
```

A command runs your own program with the message on its standard input and
the fields as `PYCHRON_EVENT`, `PYCHRON_SUBJECT`, `PYCHRON_QUEUE` and so on.

```toml
[[command]]
name = "pager"
argv = ["/usr/local/bin/notify-lab"]
```

Email, webhook and command entries can be mixed in one file. Each is sent
on its own: one failing does not stop the others, and never stops a queue.

## Troubleshooting

Run `elctl exp notify` and match what follows `failed:`.

| Message | Cause | Fix |
|---|---|---|
| `no notifications are configured` | No `notifications.toml` in this lab folder, or it has no entries | Step 5. Check the folder with `elctl doctor` |
| `notifications.toml: ... unknown key` or `missing` | A typo in the file | The message names the entry and the key |
| `PYCHRON_MAIL_API_KEY is not set` | The variable is not set for this program | Step 4, then open a new terminal or restart pychron. On macOS see the note about the Dock |
| `error: 401` with `Key not found`, `API key is invalid` or `valid Server token` | The key is wrong, deleted, or belongs to another service | Generate a new key (step 3) and set it again. Check `provider` matches the service |
| `error: 401` with `unrecognised IP address` | Brevo is blocking computers it does not know | Step 3, item 4 |
| `error: 400`, `403` or `422` mentioning the sender, `from` or a domain | `from` is not an address the service has confirmed | Step 2. `from` must match exactly |
| `Could not resolve host`, `Failed to connect`, `timed out` | No route to the internet from the instrument computer | Try `curl https://api.brevo.com` in a terminal. Ask IT to allow outgoing HTTPS to the service |
| `cannot run curl` or `curl: option --fail-with-body: is unknown` | curl is missing, or older than 7.76 (2021) | Install a current curl, or set `curl = "<path>"` at the top of the file |
| `no recipients (the queue has no email)` | `to` is empty and the queue has no `email` | Add `to = [...]` |
| `sent: email` but nothing arrives | Spam filtering, or a mistyped address | Look in spam. In Brevo, **Transactional > Logs** shows each message and what became of it |
| Works from `elctl`, not from the window | The window was started without the variable | Step 4. Confirm with **Experiment > Executor > Send Test Notification** |

## Looking after it

- **Changing the key.** Generate a new one, set the variable to it, restart
  pychron, test, then delete the old key at the service. Do this when
  someone who knew the key leaves, or if it was ever pasted somewhere
  shared.
- **Changing who is told.** Edit `to`, and restart the experiment window.
  Users get their own queues' messages by writing their address as the
  queue's `email`.
- **A new instrument computer.** Repeat steps 3 to 6 with a key of its own,
  so one computer's key can be withdrawn without touching the others.
- **Turning it off.** Rename `notifications.toml`; the executor pane then
  shows "Notify: off".
