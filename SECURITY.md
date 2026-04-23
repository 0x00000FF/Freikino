# Security Policy

## Supported versions

Freikino is developed on `master`. Only the most recent release and
the current tip of `master` receive security fixes; older tagged
builds are not maintained.

| Version         | Supported |
| --------------- | --------- |
| latest `master` | ✅        |
| latest release  | ✅        |
| older releases  | ❌        |

## Reporting a vulnerability

**Do not open a public GitHub issue for security problems.** Freikino
processes untrusted container data (video, audio, subtitle streams)
and attachments bundled with them (fonts, cover art), so a genuine
vulnerability can be weaponised against anyone who opens a crafted
file.

Preferred channels, in order:

1. **GitHub private vulnerability reporting** — open a private
   advisory at <https://github.com/0x00000FF/Freikino/security/advisories/new>.
   This keeps the discussion private until a fix is ready.
2. **Email** — `ai@cnusec.kr`. Include steps to reproduce, a sample
   input that triggers the issue (if safe to share), the commit hash
   you reproduced against, and any crash logs / minidumps you have.

Please allow up to **7 days** for an initial acknowledgement and up
to **90 days** for a coordinated fix. If a fix can't land in that
window the maintainer will tell you and agree on a new timeline with
you before anything is made public.

## Scope

In scope:

- Memory-safety issues (buffer overruns, use-after-free, uninitialised
  reads) reachable from a parsed container, subtitle, or font payload.
- Sandbox-escape or code-execution paths through third-party
  dependencies (FFmpeg, libass, and any vcpkg-managed library)
  *as integrated into Freikino*.
- Tampering with the post-build binary verification
  (`cmake/verify_binary.ps1`) that would let an unmitigated binary
  ship from this repo's release workflow.

Out of scope:

- Issues requiring the attacker to already have code-execution or
  admin rights on the target machine.
- Issues in upstream third-party libraries that aren't reachable from
  Freikino's code paths (please report those to the respective
  upstream projects).

## What to expect

- Acknowledgement of the report within 7 days.
- A tracking CVE requested (when applicable) once the issue is
  confirmed.
- Public disclosure coordinated with you — typically a commit + patch
  note on the first tagged release that contains the fix, crediting
  the reporter unless you'd prefer to stay anonymous.
