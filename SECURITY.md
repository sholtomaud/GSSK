# Security Policy

## Supported Versions

| Version | Supported |
|---------|-----------|
| 3.x     | Yes       |
| < 3.0   | No        |

## Reporting a Vulnerability

**Do not open a public GitHub issue for security vulnerabilities.**

Please report security issues by emailing the maintainer directly:

- **Email**: sholto.maud@gmail.com
- **Subject**: `[GSSK Security] <brief description>`

Include:
- A description of the vulnerability and its potential impact
- Steps to reproduce or a proof-of-concept (JSON model, fuzz corpus entry, or C snippet)
- The GSSK version affected (`GSSK_GetVersionString`)

You will receive an acknowledgement within **48 hours** and a status update within **7 days**. If the issue is confirmed, a patch release will be prepared under coordinated disclosure (typically 14–30 days from report to public advisory).

## Security Scope

GSSK is a pure computation library. It:

- Parses untrusted JSON input via a bundled copy of cJSON
- Performs floating-point arithmetic (no system calls, no network, no file I/O beyond the CLI)
- Allocates memory proportional to model size

Relevant threat classes:
- **JSON parser bugs**: heap overflows, use-after-free in cJSON
- **Model-driven allocation**: extremely large `n` values causing OOM
- **Numerical safety**: NaN/Inf propagation (mitigated by `ERR_DIVERGENCE` checks)

Out of scope: CLI argument injection (caller responsibility), denial-of-service via enormous models in public APIs (rate-limit at the API layer).

## Security Testing

The project runs continuous security testing in CI:

- **AddressSanitizer + UBSan** (`make test-asan`): detects buffer overflows and undefined behaviour
- **Valgrind** (`make test-valgrind`): leak and error checking (Linux)
- **LibFuzzer** (`make fuzz-run`): coverage-guided fuzzing of the JSON parser path

To run locally:

```bash
CC=clang make test-asan        # requires clang
make fuzz-run FUZZ_TIMEOUT=60  # 60 s fuzz run
```
