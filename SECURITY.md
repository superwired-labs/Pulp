# Security Policy



## Supported Versions



Only the latest stable release of **PULP** receives security updates.



| Version | Supported          |

| ------- | ------------------ |

| 1.0.x   | :white\_check\_mark: |

| < 1.0   | :x:                |



## Reporting a Vulnerability



As a low-level native library handling high-throughput telemetry, security and memory safety are critical. If you discover a potential vulnerability (such as a buffer overflow, memory leak, thread safety issue, or denial of service), **please do not open a public GitHub issue**.



Instead, report it privately via email to:



**`fgauthier@superwired-labs.com`**



### Please include in your report:

- **Type of issue:** (e.g., Out-of-bounds read/write, thread-safety violation, CPU crash).

- **Proof of Concept (PoC):** Minimal reproducible C code or input string (e.g., malformed log line/IP).

- **Impact:** Description of how the flaw could be exploited or affect host processes.

- **Environment details:** Windows version, compiler settings, and CPU instruction set used.



### Disclosure Process & Response Timeline:

1. **Acknowledgment:** You will receive a private confirmation within **48 hours**.

2. **Assessment:** We will evaluate the report and determine a fix timeline within **5 business days**.

3. **Patch Release:** Security patches will be merged and released promptly alongside a disclosure notice crediting your research (unless requested otherwise).



Thank you for helping keep PULP secure, reliable, and production-ready.

