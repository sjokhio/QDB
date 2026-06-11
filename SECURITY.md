# Security Policy

## Supported Versions

QDB is currently pre-1.0.  Security fixes are applied to the `main` branch only.

Once v1.0 is released, this table will be updated with the supported version range.

| Version | Supported |
|---------|-----------|
| main    | Yes       |

## Reporting a Vulnerability

**Do not open a public GitHub issue for security vulnerabilities.**

Please report security vulnerabilities by emailing the maintainers at:

> **security@your-org.example**  *(replace with your actual address before publishing)*

Include in your report:

- A description of the vulnerability and its potential impact
- Steps to reproduce or a proof-of-concept
- The QDB version or commit hash you tested against
- Any suggested mitigation, if you have one

You will receive an acknowledgement within 72 hours.  We aim to triage and confirm vulnerabilities within 7 days and to release a fix within 30 days of confirmation, depending on severity and complexity.

We ask that you follow responsible disclosure: please allow us time to release a fix before disclosing the vulnerability publicly.

## Scope

The following are in scope:

- Memory safety issues (buffer overflow, use-after-free, out-of-bounds read/write)
- Crashes triggered by a crafted database file
- Data corruption bugs that could cause silent data loss
- File-handling issues that could lead to privilege escalation or unintended file access

The following are out of scope for v1 (no network, no auth):

- Denial-of-service via resource exhaustion on trusted local input
- Issues requiring an already-compromised system
