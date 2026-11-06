#include <Library/ConfigurationMapLib.h>

STATIC
EFI_CONFIGURATION_ENTRY_DESCRIPTOR
gConfigurationDescriptor[] = {
  // Configuration Map
  {"NumCpusFuseAddr", 0x5C04C},
  {"EnableShell", 0x1},
  {"InitialPagePoolCount", 0x700},
  {"SharedIMEMBaseAddr", 0x146AA000},
  {"DloadCookieAddr", 0x01FD3000},
  {"DloadCookieValue", 0x10},
  {"NumCpus", 8},
  {"NumActiveCores", 8},
  {"MaxLogFileSize", 0x400000},
  {"UefiMemUseThreshold", 0xE1},
  {"USBHS1_Config", 0x0},
  {"UsbFnIoRevNum", 0x00010001},
  {"PwrBtnShutdownFlag", 0x0},
  {"Sdc1GpioConfigOn", 0x1E92},
  {"Sdc2GpioConfigOn", 0x1E92},
  {"Sdc1GpioConfigOff", 0xA00},
  {"Sdc2GpioConfigOff", 0xA00},
  {"EnableSDHCSwitch", 0x1},
  {"EnableUfsIOC", 1},
  {"UfsSmmuConfigForOtherBootDev", 1},
  {"UEFIExitUfsSSURequired", 1},
  {"SecurityFlag", 0xC4},
  {"DetectRetailUserAttentionHotkey", 0x00},
  {"DetectRetailUserAttentionHotkeyCode", 0x17},
  {"EnableOEMSetupAppInRetail", 0x0},
  {"EnableLogFsSyncInRetail", 0x1},
  {"ShmBridgememSize", 0xA00000},
  {"EnableMultiThreading", 1},
  {"MaxCoreCnt", 8},
  {"EarlyInitCoreCnt", 2},
  {"EnableUefiSecAppDebugLogDump", 0x0},
  {"AllowNonPersistentVarsInRetail", 0x1},
  {"EnableDisplayThread", 0x0},
  {"EnableDisplayImageFv", 0x1},
  {"DDRInfoNotifyFlag", 0x0},
  {"EnableVariablePolicyEngine", 0},
  {"EnableACPIFallback", 0x0},
  {"EnableMultiCoreFvDecompression", 1},
  {"EUDEnableAddr", 0x88E2000},
  {"MinidumpTALoadingCfg", 0x0}
};

VOID
GetConfigurationMap (
  OUT EFI_CONFIGURATION_ENTRY_DESCRIPTOR **ConfigurationDescriptor,
  OUT UINT8                               *ConfigurationDescriptorCount)
{
  // Pass Data
  *ConfigurationDescriptor      = gConfigurationDescriptor;
  *ConfigurationDescriptorCount = ARRAY_SIZE (gConfigurationDescriptor);
}
