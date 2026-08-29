# PULP: Precompressed Upstream Layer Pipeline

> **Stop burning your cloud budget on raw log ingestion.**  
> A bare-metal C11 engine designed to sit upstream on Windows infrastructure - compressing, anonymizing, and throttling telemetry at line-rate before it hits Splunk, Datadog, or your Elastic SIEM.

---

## Key Performance Indicators

| Metric | PULP Benchmark Value | Impact on Infrastructure |
| :--- | :--- | :--- |
| **Throughput** | **20M+ logs/sec** directly to NVMe | Sustained line-rate ingestion without CPU throttling |
| **Compression Ratio** | **3× to 6× Lossless** (Semantic Dict + LZ4) | 60% to 80% reduction in Egress & Cloud storage costs |
| **Memory Footprint** | **Bounded (from <20 MB)** | Zero-OOM during DDoS or log storms |
| **Engine Footprint** | **~50 KB standalone DLL** | Pure C11 bare-metal, zero runtime dependencies |
| **Privacy / GDPR** | **Inline IPv4/v6 Anonymization** | Anonymized at the source before hitting disk or network |

---

## The FinOps & Economic Impact

Traditional logging agents format redundant JSON/Syslog strings on the hot path, triggering massive SIEM ingestion rates and network egress charges. 

PULP acts as a **Smart Upstream Gateway**, deduplicating and compressing structured fields (URLs, IPs, endpoints, status codes, latencies) right inside the application process memory.

### ROI Scenario: 30 TB / Day Ingestion Stream

* **Uncompressed Raw Stream (Traditional Agent):**
  * AWS Network Egress: 30 TB × $85 = **$2,550 / day**
  * Datadog / SIEM Ingestion: 30 TB × $160 = **$4,800 / day**
  * **Total Egress & Ingestion Cost:** **$7,350 / day** (~$220,000 / month)

* **With PULP (5× Upstream Compression → 6 TB Shipped):**
  * AWS Network Egress: 6 TB × $85 = **$510 / day**
  * Datadog / SIEM Ingestion: 6 TB × $160 = **$960 / day**
  * **Total Egress & Ingestion Cost:** **$1,470 / day** (~$44,000 / month)

=> **Net Monthly Savings: ~$176,000 / month** saved at the source without losing a single telemetry event.

---

## Target Use Cases

### 1. EDR & Endpoint Agents
Prevent security agent crashes. PULP’s zero-allocation hot path and strictly bounded memory (< 20 MB) guarantee that high-volume incident logging will **never trigger Out-Of-Memory (OOM) panic** or degrade host CPU performance under attack conditions.

### 2. Network Appliances & NDR (DPI / NetFlow / IPFIX)
Ingest high-cardinality network metadata (5-tuples, IPs, ports, byte counts) at line-rate. PULP's multi-level cache and 32-byte deterministic binary entries handle multi-million unique items without cache thrashing.

### 3. High-Volume Web & API Gateways (IIS / Custom Services)
Pre-process millions of HTTP access logs per second. Anonymize IP addresses inline to enforce strict GDPR/privacy compliance *before* telemetry ever touches disk or network.

---

## Architectural Highlights

* **Zero-Contention Hot Path:** Mechanical sympathy during production. Telemetry passes through Thread-Local Storage (TLS) buffers to eliminate lock contention.
* **Deterministic RAM occupation:** The hot path speed allow for spikes to be seamlessly integrated in preallocated resources without ever allocating more resource.
* **Crash-Resistant Format:** Binary block structure designed for autonomous extraction—logs remain 100% readable even after a hard system shutdown.
* **Absolute Event Sequencing:** Inter-thread atomic numbering guarantees exact chronological event reconstruction across multi-core systems.

---

## Licensing & Commercial Support

PULP is published under a **Dual Licensing Model**:

* **Open-Source (AGPLv3):** Free for open-source projects, academic evaluation, and community testing.
* **Commercial / OEM License:** Designed for closed-source enterprise integration, proprietary software redistribution (EDR/NDR appliances), and corporate compliance exemption.

---

## Get Started & Request a PoC

Ready to evaluate PULP on your real-world telemetry streams?

* 💻 **GitHub Repository:** [github.com/superwired-labs/Pulp](https://github.com/superwired-labs/Pulp)
* 📧 **Commercial Enquiries & PoC Requests:** Contact François Gauthier at `fgauthier@superwired-labs.com`
* 💬 **LinkedIn:** [François Gauthier on LinkedIn](https://www.linkedin.com/in/superwired-labs/)
