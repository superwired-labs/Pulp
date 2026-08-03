\# PULP Roadmap



This document outlines the planned, proposed, and candidate features for future releases of \*\*PULP\*\*. 

Priorities may adjust based on community feedback, sponsor requests, and contributor availability.



\---



\### High Priority / Short-Term

\- \[ ] \*\*Background Flush Timer:\*\* Optional automatic buffer flush on low-traffic applications after N milliseconds.

\- \[ ] \*\*Official Rust Binding:\*\* Safe Rust wrapper around `pulp.dll` (published on crates.io).

\- \[ ] \*\*Enhanced Integration Examples:\*\* Complete sample projects for IIS native C++ modules and C# / .NET P/Invoke integrations.



\---



\### Medium Priority / Exploratory

\- \[ ] \*\*Scalar / Non-AVX2 Fallback:\*\* Graceful fallback path for older CPU architectures.

\- \[ ] \*\*OTLP / FluentBit Exporter:\*\* Standalone bridge tool converting PULP binary archives directly to OpenTelemetry streams.



\---



\### Long-Term / Enterprise Features

\- \[ ] \*\*Linux Kernel/POSIX Port:\*\* POSIX reimplementation for Linux server environments leveraging the kernel's unique features.

\- \[ ] \*\*Multi-Socket NUMA Optimizations:\*\* Lock-free inter-process allocations for hyper-scale (>64 core) architectures.



\---



\### Ideas \& Community Proposals

Have an idea or want to contribute? Feel free to open a \[GitHub Issue](https://github.com/superwired-labs/Pulp/issues) or start a discussion!

