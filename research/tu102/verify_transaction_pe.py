"""Binary-format verification plus explicit experimental-mode manifest.

Mode describes the selected compile-time build. PE validity does not establish
device safety, successful firmware execution, cleanup, or Windows performance.
"""
import argparse
import json
from pathlib import Path
from verify_preflight_pe import verify

p=argparse.ArgumentParser(description=__doc__)
p.add_argument("source")
p.add_argument("destination")
p.add_argument("--report",required=True)
p.add_argument("--mode",choices=("TU102-CANDIDATE-CHECK","TU102-UNLOCK-EXPERIMENTAL","TU102-UNLOCK-AMD-DIRECT","TU102-UNLOCK-INTERNAL-AUTO"),required=True)
a=p.parse_args()
r=verify(a.source,a.destination,True)
enabled=a.mode!="TU102-CANDIDATE-CHECK"
automatic=a.mode=="TU102-UNLOCK-INTERNAL-AUTO"
direct=a.mode in ("TU102-UNLOCK-AMD-DIRECT","TU102-UNLOCK-INTERNAL-AUTO")
r.update(mode=a.mode,unlock_transaction_source_present=True,
         gpu_mmio="experimental read/write after U" if enabled else "read-only",
         unlock_implemented=enabled,manual_confirmation_required=enabled,
         hardware_execution_compiled_in=enabled,hardware_tested=False,
         simulated_core_cases=510,simulated_efi_dma_cases=49,
         dma_allocation_policy="device DAC enabled and read back; PCI_IO AllocateBuffer argument0; formal Map success/full-size/high-address required; opaque token includingNULL passed unchanged to Unmap, cleanup success required",
         simulated_handoff_cases=20,simulated_entry_refusal_cases=27,
         simulated_complete_entry_cases=17,
         simulated_extra_selector_latch_checks=330,
         simulated_display_cases=69,
         simulated_specific_driver_release_cases=42,
         decode_restoration="after U and exclusive ownership: only cleared original IO/MEM bits, all other256-byte config identical; standard Attributes API, no bus-master enable",
         driver_release="experimental after U: exact target nvgop-tu1xx one driver, no children, rechecked identity/image and native cold state; any post-release failure stops" if enabled else False,
         display_policy="exact physical display; framebuffer aliases or positive text-output/driver-child identity for one zero-framebuffer non-PCI console; no inference from Info pointer",
         selector_policy="experimental immutable entry latch; full exact PROM and cold guards retained",
         dma="experimental on manual U" if enabled else False,
         gpu_mmio_writes="experimental on manual U" if enabled else False,
         pci_attribute_changes="experimental on manual U" if enabled else False,
         pci_config_writes="indirect via EFI PCI Attributes after U" if enabled else False,
         raw_pci_config_writes=False,
         firmware_execution="experimental on manual U" if enabled else False,
         windows_handoff_implemented=True,windows_handoff_hardware_tested=False,
         installed=False,verified_outcome="PE structure only; hardware behavior unverified")
if direct:
    r.update(dma="EXPERIMENTAL DIRECT PHYSICAL DMA after U, NOT UEFI-mapped; no IOMMU disable",
             dma_allocation_policy="owned verified-WB4.25-8GiB AllocateAddress pages; exact native/CPU/IVRS/root-code; SME/SNP off and AMD IOMMU control disabled required",
             simulated_amd_direct_cases=27,
             formal_UEFI_mapping=False,
             direct_DMA_assumption="GPU host-physical identity reachability on this exact B850/Zen5 profile; NOT hardware-validated",
             additional_reads="AMD SYSCFG MSR and exact IVRS-named IOMMU control/EFR via host CPU_IO2.Mem.Read; no writes, raw-MMIO or root-relative fallback",
             cache_visibility="WBINVD at firmware-start boundaries as in published direct wrapper; mocked in host tests",
             phase_guard="repeat environment guard before BME and after FWSEC engines reset before Booter",
             windows_handoff_hardware_tested=False)
if automatic:
    r.update(manual_confirmation_required=False,
             arm_policy="installer consent plus exact local GPT ESP/marker; immediate silent start; no keyboard polling/reset, countdown or skip; all core guards retained",
             log_policy="fixed private EFI/TU102Unlock/TRANSACTION.TXT truncated each boot; no root/directory creation",
             simulated_internal_auto_cases=29,simulated_same_ESP_handoff_cases=2,
             automatic_error_recovery="pre-mutation failures show error and load Windows; before mutation pin original Boot0000, arm one-shot Windows BootNext and watchdog120s; unsafe errors cold-reset to Windows once; successful handoff removes BootNext/watchdog",
             recovery_hardware_tested=False,
             hardware_tested=False,automatic_boot_hardware_tested=False,
             proven_core_artifact_sha256="269bc7cdbeb7e86a49d311d3795108b962ecf5bcee17267447353bc0575cae5a",
             core_manual_validation="CAPTURE21: Windows Tensor issue rate1.99244x, arithmetic passed; automatic entry pending",
             gpu_mmio="read/write after automatic guards",gpu_mmio_writes="after automatic guards",
             firmware_execution="after automatic guards",pci_attribute_changes="after automatic guards",
             pci_config_writes="indirect PCI Attributes after automatic guards",
             driver_release="same exact local nvgop guard as measured manual profile; automatic consent",
             dma="DIRECT PHYSICAL DMA after automatic guards; NOT UEFI-mapped; IOMMU unchanged",
             verified_outcome="PE and mocked automatic entry; same local core measured manually; automatic boot pending")
Path(a.report).write_text(json.dumps(r,indent=2)+"\n",encoding="utf-8")
print(json.dumps(r))
