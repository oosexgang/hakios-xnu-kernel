  INF MdeModulePkg/Core/Dxe/DxeMain.inf
  INF MdeModulePkg/Universal/PCD/Dxe/Pcd.inf

  INF Binaries/redwood/QcomPkg/Drivers/EnvDxe/EnvDxe.inf

  INF MdeModulePkg/Universal/ReportStatusCodeRouter/RuntimeDxe/ReportStatusCodeRouterRuntimeDxe.inf
  INF MdeModulePkg/Universal/StatusCodeHandler/RuntimeDxe/StatusCodeHandlerRuntimeDxe.inf
  INF ArmPkg/Drivers/CpuDxe/CpuDxe.inf
  INF MdeModulePkg/Core/RuntimeDxe/RuntimeDxe.inf
  INF MdeModulePkg/Universal/SecurityStubDxe/SecurityStubDxe.inf

  INF Binaries/redwood/QcomPkg/Drivers/ShmBridgeDxe/ShmBridgeDxeLA.inf
  INF Binaries/redwood/QcomPkg/Drivers/TzDxe/ScmDxeLA.inf
  INF Binaries/redwood/QcomPkg/Drivers/TzDxe/TzDxeLA.inf

  INF MdeModulePkg/Universal/WatchdogTimerDxe/WatchdogTimer.inf
  INF MdeModulePkg/Universal/CapsuleRuntimeDxe/CapsuleRuntimeDxe.inf
  INF MdeModulePkg/Universal/Variable/RuntimeDxe/VariableRuntimeDxe.inf
  INF EmbeddedPkg/EmbeddedMonotonicCounter/EmbeddedMonotonicCounter.inf
  INF MdeModulePkg/Universal/ResetSystemRuntimeDxe/ResetSystemRuntimeDxe.inf
  INF EmbeddedPkg/RealTimeClockRuntimeDxe/RealTimeClockRuntimeDxe.inf
  INF EmbeddedPkg/MetronomeDxe/MetronomeDxe.inf
  INF MdeModulePkg/Universal/HiiDatabaseDxe/HiiDatabaseDxe.inf
  INF ArmPkg/Drivers/ArmGicDxe/ArmGicDxe.inf
  INF ArmPkg/Drivers/TimerDxe/TimerDxe.inf

  INF Binaries/redwood/QcomPkg/Drivers/ChipInfoDxe/ChipInfoDxe.inf
  INF Binaries/redwood/QcomPkg/Drivers/PlatformInfoDxe/PlatformInfoDxe.inf
  INF Binaries/redwood/QcomPkg/Drivers/ProjectInfoDxe/ProjectInfoDxe.inf
  INF Binaries/redwood/QcomPkg/Drivers/DALSYSDxe/DALSYSDxe.inf
  INF Binaries/redwood/QcomPkg/Drivers/HALIOMMUDxe/HALIOMMUDxe.inf
  INF Binaries/redwood/QcomPkg/Drivers/HWIODxe/HWIODxe.inf

  INF MdeModulePkg/Universal/DevicePathDxe/DevicePathDxe.inf
  INF MdeModulePkg/Universal/Disk/DiskIoDxe/DiskIoDxe.inf
  INF MdeModulePkg/Universal/Disk/PartitionDxe/PartitionDxe.inf
  INF FatPkg/EnhancedFatDxe/Fat.inf
  INF MdeModulePkg/Universal/Disk/UnicodeCollation/EnglishDxe/EnglishDxe.inf

  INF Binaries/redwood/QcomPkg/Drivers/ClockDxe/ClockDxe.inf
  INF Binaries/redwood/QcomPkg/Drivers/SdccDxe/SdccDxe.inf
  INF Binaries/redwood/QcomPkg/Drivers/I2CDxe/I2CDxe.inf
  INF Binaries/redwood/QcomPkg/Drivers/SPMIDxe/SPMIDxe.inf
  INF Binaries/redwood/QcomPkg/Drivers/TLMMDxe/TLMMDxe.inf
  INF Binaries/redwood/QcomPkg/Drivers/PmicDxe/PmicDxeLa.inf
  INF Binaries/redwood/QcomPkg/Drivers/UFSDxe/UFSDxe.inf
  INF Binaries/redwood/QcomPkg/Drivers/CmdDbDxe/CmdDbDxe.inf
  INF Binaries/redwood/QcomPkg/Drivers/PwrUtilsDxe/PwrUtilsDxe.inf
  INF Binaries/redwood/QcomPkg/Drivers/RpmhDxe/RpmhDxe.inf
  INF Binaries/redwood/QcomPkg/Drivers/NpaDxe/NpaDxe.inf
  INF Binaries/redwood/QcomPkg/Drivers/ULogDxe/ULogDxe.inf
  INF Binaries/redwood/QcomPkg/Drivers/VcsDxe/VcsDxe.inf
  INF Binaries/redwood/QcomPkg/Drivers/ICBDxe/ICBDxe.inf
  INF Binaries/redwood/QcomPkg/Drivers/SmemDxe/SmemDxe.inf
  INF MdeModulePkg/Universal/BdsDxe/BdsDxe.inf
  INF EmbeddedPkg/SimpleTextInOutSerial/SimpleTextInOutSerial.inf
  INF MdeModulePkg/Universal/PrintDxe/PrintDxe.inf
  INF MdeModulePkg/Universal/Console/ConPlatformDxe/ConPlatformDxe.inf
  INF MdeModulePkg/Universal/Console/ConSplitterDxe/ConSplitterDxe.inf
  INF MdeModulePkg/Universal/Console/GraphicsConsoleDxe/GraphicsConsoleDxe.inf

!if $(USE_CUSTOM_DISPLAY_DRIVER) == 1
  INF Binaries/redwood/QcomPkg/Drivers/CPRDxe/CPRDxe.inf
!endif
  INF Binaries/redwood/QcomPkg/Drivers/GpiDxe/GpiDxe.inf
  INF Binaries/redwood/QcomPkg/Drivers/SPIDxe/SPIDxe.inf
  INF Binaries/redwood/QcomPkg/Drivers/FeatureEnablerDxe/FeatureEnablerDxe.inf

!if $(USE_CUSTOM_DISPLAY_DRIVER) == 1
  INF Binaries/redwood/QcomPkg/Drivers/DisplayDxe/DisplayDxe.inf
!else
  INF SiliciumPkg/Drivers/SimpleFbDxe/SimpleFbDxe.inf
!endif

  INF Binaries/redwood/QcomPkg/Drivers/ButtonsDxe/ButtonsDxe.inf
  INF Binaries/redwood/QcomPkg/Drivers/ChargerExDxe/ChargerExDxe.inf
  INF Binaries/redwood/QcomPkg/Drivers/IPCCDxe/IPCCDxe.inf
  INF Binaries/redwood/QcomPkg/Drivers/GLinkDxe/GLinkDxe.inf
  INF Binaries/redwood/QcomPkg/Drivers/PmicGlinkDxe/PmicGlinkDxe.inf
  INF Binaries/redwood/QcomPkg/Drivers/QcomChargerDxe/QcomChargerDxeLA.inf
  INF Binaries/redwood/QcomPkg/Drivers/UsbPwrCtrlDxe/UsbPwrCtrlDxe.inf
  INF Binaries/redwood/QcomPkg/Drivers/TsensDxe/TsensDxe.inf
  INF Binaries/redwood/QcomPkg/Drivers/LimitsDxe/LimitsDxe.inf
  INF Binaries/redwood/QcomPkg/Drivers/DDRInfoDxe/DDRInfoDxe.inf
  INF Binaries/redwood/QcomPkg/Drivers/UsbfnDwc3Dxe/UsbfnDwc3Dxe.inf
  INF Binaries/redwood/QcomPkg/Drivers/XhciPciEmulationDxe/XhciPciEmulationDxe.inf
  INF Binaries/redwood/QcomPkg/Drivers/XhciDxe/XhciDxe.inf

  INF MdeModulePkg/Bus/Usb/UsbBusDxe/UsbBusDxe.inf
  INF MdeModulePkg/Bus/Usb/UsbKbDxe/UsbKbDxe.inf
  INF MdeModulePkg/Bus/Usb/UsbMassStorageDxe/UsbMassStorageDxe.inf

  INF Binaries/redwood/QcomPkg/Drivers/UsbMsdDxe/UsbMsdDxe.inf
  INF Binaries/redwood/QcomPkg/Drivers/UsbDeviceDxe/UsbDeviceDxe.inf
  INF Binaries/redwood/QcomPkg/Drivers/UsbConfigDxe/UsbConfigDxe.inf
  INF Binaries/redwood/QcomPkg/Drivers/UsbInitDxe/UsbInitDxe.inf
  INF Binaries/redwood/QcomPkg/Drivers/UCDxe/UCDxe.inf
  INF Binaries/redwood/QcomPkg/Drivers/RNGDxe/RngDxe.inf
  INF Binaries/redwood/QcomPkg/Drivers/PILDxe/PILDxe.inf
  INF Binaries/redwood/QcomPkg/Drivers/PILProxyDxe/PILProxyDxe.inf
