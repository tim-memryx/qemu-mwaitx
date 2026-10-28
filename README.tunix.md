# Tunix development patches

`tunix_dev_patches` tracks QEMU v11.1.2 for the Tunix hobby OS. It includes:

* Intel VT-d compatibility-format interrupt blocking according to GSTS.CFIS and
  EIME, including fault reason 0x25. Architected register state already carries
  CFIS through reset/migration. Valid remappable messages keep SID validation.
* Native AMD MONITORX/MWAITX exposure, gated by host CPUID, KVM's MWAIT
  interception-disable capability, `-overcommit cpu-pm=on`, and
  `-cpu host,migratable=off`. Explicit `mwaitx=off` is respected. TCG and
  migratable CPU profiles cannot enable it; live migration is blocked even
  when invariant TSC is masked. No host kernel patch is needed on the
  qualified Ryzen 9950X / Linux 7.2.12 host.
* AMD-Vi fault records retain full requester IDs, domain IDs and addresses;
  cached permission denials report faults, page walks preserve cumulative parent
  permissions, and event-log overflow is recorded with interrupts masked.
  Interrupt-table bounds are checked and rejected interrupts produce attributed
  events while respecting IG/SupIOPF suppression.
* Host-only retirement test fixtures. They do not modify guest CPUID, manufacture
  hardware completions, or relax translation/interrupt isolation.

The fixtures are mutable QOM properties, intended only for non-migrating tests:

| Device/property | Default | Behavior |
| --- | --- | --- |
| intel-iommu / `tunix-qi-stall` | false | Retains an accepted queue tail without fetching descriptors. Clearing resumes real descriptor execution; system reset clears it. |
| intel-iommu / `tunix-fault-overflow` | false | Sets architected primary fault-log overflow and generates its normal fault event, leaving translation operational. Getter reads the real sticky bit; guest W1C or setting false clears it. |
| amd-iommu / `tunix-qi-stall` | false | Stops command consumption; clearing resumes actual commands and completions. DMA/IRQ translation continues; reset clears the control. |
| amd-iommu / `tunix-fault-overflow` | false | Sets the real event-log overflow bit and signals its enabled event interrupt; normal W1C or setting false clears it. |
| edu / `tunix-dma-delay-ms` | 100 | Delay for the next DMA timer, clamped to 60,000 ms. Changing it does not reschedule an already-issued command. |
| edu / `tunix-dma-completions` | 0, read-only | Actual DMA timer callbacks that retire a RUN command. |
| edu / `tunix-dma-failures` | 0, read-only | PCI DMA transactions returning a result other than MEMTX_OK. |
| edu / `tunix-posted-delay-ms` | 0 | One-shot delay of the next idle DMA RUN doorbell and its captured payload, clamped to 60,000 ms. RUN still reads idle until the identity read flushes it or the timer expires. Resets itself to zero when consumed. |
| edu / `tunix-posted-flushes` | 0, read-only | Pending doorbells actually flushed by an identity-register read. |
| edu / `tunix-readback-failure` | 0 | Nonzero makes identity reads return all ones without flushing a held doorbell. |

Fixture controls/counters are not migration state. No guest register programs
them. Normal EDU behavior retains its upstream 100-ms delay. Disabling PCI bus
mastering blocks a late timer's DMA before it reaches VT-d; the failure counter
lets Tunix tests distinguish that outcome from an unexecuted timer or a fabricated
IOMMU fault. EDU has no qualified FLR; Tunix tests idle/sanitization or quarantine.

`tests/qtest/intel-iommu-test` covers compatibility/CFIS/EIME/SID behavior, stalled
and resumed queued completion, delayed DMA data, and rejection after BME removal.
It also checks held-doorbell visibility, real readback-triggered DMA, cancellation
of the original posted timer, and failed readback with a later BME-blocked command.
These fixtures model a missing retirement proof, not arbitrary PCIe fabric timing.
Run with `QTEST_QEMU_BINARY` pointing at the corresponding built emulator.

`python3 tests/tunix-mwaitx.py /path/to/qemu-system-x86_64` checks native CPU
policy and migration refusal on an MWAITX-capable AMD/KVM host. Tunix's
`make test-mwaitx USER_TEST_CPUS=2` separately qualifies actual CPL3 execution,
bounded timer/store waits and KVM exit counts. CPU enumeration alone is not
execution evidence. KVM control semantics are documented at
https://docs.kernel.org/virt/kvm/api.html#kvm-cap-x86-disable-exits.

## Two-engine qualification topology

On Q35, `-device intel-iommu,id=vi,intremap=on,tunix-split-devfn=24`
creates a second, independent VT-d engine for the root port at `00:03.0`
and its subtree. Create that port with `-device pcie-root-port,id=rp2,addr=3,chassis=2`.
Specify the IOMMU before the PCI test devices. The default value, 256, retains
upstream single-engine routing. Other values must name an existing root port on
pcie.0. Routing follows bridge objects, including nested bridges; it does not
infer ownership from firmware-assigned bus numbers.

The secondary register aperture is 0xfed91000; its DMAR DRHD has an explicit
subtree scope. The primary remains at 0xfed90000, with INCLUDE_ALL and sole
IOAPIC ownership. Registers, translation/context/interrupt caches, invalidation
queues and fault state belong to separate IntelIOMMUState objects. Secondary
host controls are at `/machine/peripheral/vi/tunix-secondary`; primary controls
remain at `/machine/peripheral/vi`.

This bounded fixture rejects migration, host device assignment, scalable mode,
ATS and PASID. Hotplug is not qualified. It is not a general multi-segment or
physical-platform model. Both engines use the primary's capability options.
The split-engine qtest checks independent real EDU DMA translations, MSI source
faults, queue progress/stalling, overflow, reset, and the emitted DMAR scopes.

AMD regressions run with `QTEST_QEMU_BINARY=/absolute/path/to/qemu-system-x86_64
/path/to/build/tests/qtest/amd-iommu-test`. They use a real downstream EDU requester
and check non-page-aligned fault addresses, cached read-only translations, parent
permissions, masked overflow, interrupt bounds/suppression and queue consumption.
The host controls are opt-in test fixtures, not hardware isolation qualification.

## AMD two-engine topology

`amd-iommu,id=vi,dma-remap=on,intremap=on,xtsup=on,device-iotlb=off,tunix-split-devfn=24`
selects the same root-port subtree convention as the Intel fixture. Each engine
has separate AMDVIState, device/interrupt translations, command/event queues and
fault controls. The primary owns the IOAPIC; IVRS lists disjoint device scopes
with legacy/extended alternatives for each unit. Register banks are 0xfed80000
and 0xfed84000, PCI identities 00:12.0 and 00:13.0. Defaults retain the original
single-engine device placement. The secondary QOM path is `vi/tunix-secondary`.
Migration, host assignment, external PCI identity and device-IOTLB profiles are
excluded. The root port must exist before machine initialization completes.

`tests/qtest/amd-iommu-test` checks actual DMA using equal IOVAs with independent
physical translations, primary completion while the secondary is stalled, resume,
and isolated fault status. Tunix's `test-device-multi-unit`, `test-device-multi-queue`
and `test-device-multi-overflow` plus their `-uefi` variants use
`IOMMU_TEST_VENDOR=amd` for real guest DMA/MSI, route ownership, completion tokens,
three queue-wrap rounds and secondary quarantine with primary reuse.

## Bounded two-segment topology

Add `tunix-secondary-segment=on` to either split-engine model and place the
second root port on `bus=tunix-pcie.1`. This creates a separate PCI Express host,
ECAM at 0x50000000 (16 MiB, buses 0..15), and a segment-1 MMIO window at
0x40000000 (256 MiB). RAM is limited to 1 GiB. These apertures sit below the
primary OVMF PCI aperture; placing them inside it can shadow firmware-assigned
BARs before the OS boots. MCFG and `_SB.PC01` describe the independent root;
PCI0's resources exclude its windows. The explicit preboot qtest fixture assigns
segment-1 bridge/EDU BARs and enables ACS with readback. CF8 remains segment zero.

The secondary engine owns segment 1, including its AMD IOMMU PCI identity.
DMAR/IVRS describe separate real routing, not relabeled requests. Both segments
may contain `01:00.0`; translations, interrupt tables, queues and faults remain
independent. Only segment zero owns the IOAPIC. This fixture permits remapped MSI
only on the new root, rejects migration/host assignment, and does not implement
hotplug, general firmware resource allocation or physical multi-segment support.

## ACPI embedded-controller fixture

`-device tunix-ec,id=ec0` is an opt-in Q35-only port-I/O EC at 0x62/0x66.
It supplies 256 bytes, RD_EC/WR_EC/QR_EC, ECDT and namespace AML for early
module-level access, `_REG`, `_INI` and `_Q42`. `ecdt=off`, `global-lock=on`
and `stuck=on` select qualification profiles. A QOM write of 0x42 to `query`
raises the fixture's otherwise-unused GPE 7. Read-only QOM `value`, `reads`,
`errors` and `region-ready` distinguish actual EC transactions from serial
messages. Reset clears controller state and counters; migration/hotplug are
unsupported. This does not emulate SMM lock contention or a physical EC.

`tests/qtest/tunix-ec-test` exercises all byte addresses, query delivery, reset,
invalid sequencing and stuck status. Tunix's `test-acpi-ec` and UEFI variant
exercise the firmware/runtime path with both IOMMU vendors. The second-segment
fixture may also be left at reset for Tunix's explicit boot-allocation provider;
`test-pci-boot-allocation` verifies kernel assignments without host config writes.
