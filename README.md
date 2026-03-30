# Porth-IO: Sovereign Logic Layer 🏎️💨  
High-Performance C++23 Framework for Sub-Microsecond Hardware Control

---

## 🎯 Executive Summary (The "Why")

In high-stakes environments like compound semiconductor manufacturing (GaN/InP), hardware interconnects require sub-microsecond response times to prevent catastrophic equipment failure. Standard Linux networking stacks introduce "jitter"—unpredictable delays caused by kernel overhead, context switching, and cache misses.

Porth-IO is a "Sovereign Logic Layer" designed to bypass these bottlenecks. By treating the OS as a secondary concern and the hardware as the primary focus, it provides a deterministic data plane where software logic operates at the physical limits of the interconnect.

---

## 📈 Performance Manifest

Verified on bare-metal isolated CPU cores across 50,000 test cycles.

| Metric                     | Result     | Technical Significance                                                      |
|---------------------------|------------|----------------------------------------------------------------------------|
| Mean Telemetry Latency    | 122.78 ns  | The average time for a signal to traverse the logic layer.                |
| Precision (IQR)           | 0.42 ns    | Proves extreme consistency (low jitter) between execution cycles.         |
| Tail Latency (P99.99)     | 1.3 µs     | The "worst-case" scenario, ensuring safety even during outliers.          |
| Throughput                | Line Rate  | Achieved via zero-copy DMA and lock-free synchronisation.                 |

---

## 🛠️ Technical Architecture (The "How")

### 1. Zero-Copy Kernel Bypass (AF_XDP)

To eliminate the "traffic jam" of the standard network stack, Porth-IO utilises AF_XDP (Express Data Path).

- **The Implementation:** Binds XDP sockets directly to HugePage-backed UMEM.  
- **The Result:** Data is "teleported" from the NIC to application memory without a single `memcpy` or kernel-space transition.  

---

### 2. Mechanical Sympathy & Cache-Line Isolation

The framework is designed to respect the physical architecture of modern CPUs to prevent False Sharing.

- **Hardware Alignment:** Uses `alignas(64)` to ensure independent registers and ring buffer pointers sit on separate cache lines.  
- **Sovereign Guard:** Employs `std::atomic_ref` and explicit memory barriers (Acquire/Release semantics) to bridge the CPU-hardware gap without traditional locks.  

---

### 3. Sovereign Shuttle (DMA Orchestration)

The PorthShuttle acts as the high-speed bridge between software and silicon.

- **Placement New Logic:** Constructs C++ objects directly into hardware-visible, pinned memory regions.  
- **NUMA Awareness:** Automatically pins memory and threads to the same physical CPU socket to eliminate cross-socket interconnect latency.  

---

### 4. Physics-Informed Digital Twin

Uniquely, this project includes a high-fidelity Simulation Engine that models semiconductor physics.

- **Thermal Drift:** Simulates how Indium Phosphide (InP) lattice drift affects signal integrity at high temperatures.  
- **Chaos Engineering:** Injects bit-flipping and bus-hang scenarios to verify the PorthSentinel's sub-microsecond emergency trip responses.  

---

## 🧰 The Stack

- **Language:** C++23 (leveraging `std::expected` for error handling and `std::atomic_ref` for MMIO).  
- **Safety:** RAII management for physical BAR mappings and IOMMU groups via VFIO.  
- **Tooling:** CMake, Google Benchmark, and `libpci` for hardware discovery.  

---

## 📂 Repository Structure

- `include/porth/`: Core Hardware Abstraction Layer (HAL) and lock-free primitives.  
- `enterprise/`: The "Digital Twin" simulation and high-fidelity physics models.  
- `examples/`: End-to-end stress tests and telemetry collection.  
- `scripts/`: Automation for HugePage reservation and RT-priority setup.  

---

## 💡 How to Interpret this Showcase

### For the Hiring Manager:
This project demonstrates the ability to build the "engine" of a high-reliability system. The 122.78ns mean latency is the difference between a successful manufacturing run and a multi-million pound hardware failure. It shows mastery of resource management, safety-critical design, and system predictability.

### For the Technical Lead:
Look at `PorthRingBuffer.hpp` for the SPSC implementation and `PorthRegister.hpp` for how cache-line isolation is handled. The "Hot Path" is entirely allocation-free, utilises serialised hardware clocks (TSC/ARM64 Virtual Timer) for sub-nanosecond telemetry, and enforces strict NUMA locality to prevent non-deterministic latency spikes.

---

## 🚀 Getting Started

- **Optimise System:** Run `./scripts/setup_hugepages.sh` to reserve pinned memory.  
- **Build:** Run `./scripts/build.sh` (requires C++23 compatible compiler).  
- **Run Demo:** `./porth_showcase_demo` to launch the Digital Twin and view real-time telemetry.  

---

Developed by Harri Davies – 2026 Porth-IO Performance Showcase
