##
#  Copyright (c) 2011 - 2022, ARM Limited. All rights reserved.
#  Copyright (c) 2014, Linaro Limited. All rights reserved.
#  Copyright (c) 2015 - 2020, Intel Corporation. All rights reserved.
#  Copyright (c) 2018, Bingxing Wang. All rights reserved.
#  Copyright (c) Microsoft Corporation.
#
#  SPDX-License-Identifier: BSD-2-Clause-Patent
##

################################################################################
#
# Defines Section - statements that will be processed to create a Makefile.
#
################################################################################
[Defines]
  PLATFORM_NAME                  = redwood
  PLATFORM_GUID                  = 62B40F33-4011-4ACB-8348-1A6BB928C8A6
  PLATFORM_VERSION               = 0.1
  DSC_SPECIFICATION              = 0x00010005
  OUTPUT_DIRECTORY               = Build/redwoodPkg
  SUPPORTED_ARCHITECTURES        = AARCH64
  BUILD_TARGETS                  = RELEASE|DEBUG
  SKUID_IDENTIFIER               = DEFAULT
  FLASH_DEFINITION               = redwoodPkg/redwood.fdf
  USE_CUSTOM_DISPLAY_DRIVER      = 0

  #
  # 0 = SM7325
  # 1 = SM7325-AE
  # 2 = SM7325-AF
  #
  SOC_TYPE                       = 0

!include KodiakPkg/KodiakPkg.dsc.inc

[PcdsFixedAtBuild]
  #
  # DDR Memory
  #
  gArmTokenSpaceGuid.PcdSystemMemoryBase|0x80000000

  #
  # UEFI Stack
  #
  gArmPlatformTokenSpaceGuid.PcdCPUCoresStackBase|0x9FF90000
  gArmPlatformTokenSpaceGuid.PcdCPUCorePrimaryStackSize|0x40000

  #
  # SMBIOS
  #
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemManufacturer|"Xiaomi"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemModel|"POCO X5 Pro 5G"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemRetailModel|"redwood"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemRetailSku|"POCO_X5_Pro_5G_redwood"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemBoardModel|"POCO X5 Pro 5G"

  #
  # Simple Frame Buffer
  #
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferWidth|1080
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferHeight|2400
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferColorDepth|32

  #
  # Platform PEI
  #
  gQcomPkgTokenSpaceGuid.PcdPlatformType|"LA"

  #
  # Storage
  #
  gQcomPkgTokenSpaceGuid.PcdStorageIsUfs3|FALSE

[LibraryClasses]
  #
  # Memory Libraries
  #
  MemoryMapLib|redwoodPkg/Library/MemoryMapLib/MemoryMapLib.inf

  #
  # QCOM Libraries
  #
  ConfigurationMapLib|redwoodPkg/Library/ConfigurationMapLib/ConfigurationMapLib.inf

[Components]
  #
  # ACPI Tables
  #
  lisa/AcpiTables.inf
