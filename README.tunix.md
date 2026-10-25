# Tunix development patches

`tunix_dev_patches` tracks QEMU v11.1.2 for the Tunix hobby OS. It includes:

* Intel VT-d compatibility-format interrupt blocking according to GSTS.CFIS and
  EIME, including fault reason 0x25. Architected register state already carries
  CFIS through reset/migration. Valid remappable messages keep SID validation.
* Host-only retirement test fixtures. They do not modify guest CPUID, manufacture
  hardware completions, or relax translation/interrupt isolation.

The fixtures are mutable QOM properties, intended only for non-migrating tests:

| Device/property | Default | Behavior |
| --- | --- | --- |
| intel-iommu / `tunix-qi-stall` | false | Retains an accepted queue tail without fetching descriptors. Clearing resumes real descriptor execution; system reset clears it. |
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
