##
# Copyright (c) Microsoft Corporation.
# SPDX-License-Identifier: BSD-2-Clause-Patent
##

import datetime
import logging
import os
import uuid

from io import StringIO
from pathlib import Path

from edk2toolext.environment import shell_environment
from edk2toolext.environment.uefi_build import UefiBuilder
from edk2toolext.invocables.edk2_platform_build import BuildSettingsManager
from edk2toolext.invocables.edk2_pr_eval import PrEvalSettingsManager
from edk2toolext.invocables.edk2_setup import (RequiredSubmodule, SetupSettingsManager)
from edk2toolext.invocables.edk2_update import UpdateSettingsManager
from edk2toolext.invocables.edk2_parse import ParseSettingsManager
from edk2toollib.utility_functions import RunCmd

# ####################################################################################### #
#                                Common Configuration                                     #
# ####################################################################################### #
class CommonPlatform ():
    PackagesSupported = ("redwoodPkg")
    ArchSupported = ("AARCH64")
    TargetsSupported = ("DEBUG", "RELEASE")
    Scopes = ('redwood', 'gcc_aarch64_linux', 'edk2-build')
    WorkspaceRoot = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
    PackagesPath = (
        "Platforms/Xiaomi",
        "Common/Mu",
        "Common/Mu_OEM_Sample",
        "Mu_Basecore",
        "Silicon/Qualcomm",
        "Silicon/Silicium",
        "Silicium-ACPI",
        "Silicium-ACPI/Platforms/Xiaomi",
        "Silicium-ACPI/Silicon/Qualcomm"
    )

# ####################################################################################### #
#                         Configuration for Update & Setup                                #
# ####################################################################################### #
class SettingsManager (UpdateSettingsManager, SetupSettingsManager, PrEvalSettingsManager, ParseSettingsManager):

    def GetPackagesSupported (self):
        return CommonPlatform.PackagesSupported

    def GetArchitecturesSupported (self):
        return CommonPlatform.ArchSupported

    def GetTargetsSupported (self):
        return CommonPlatform.TargetsSupported

    def GetRequiredSubmodules (self):
        return [
            RequiredSubmodule ("Binaries", True),
            RequiredSubmodule ("Common/Mu", True),
            RequiredSubmodule ("Common/Mu_OEM_Sample", True),
            RequiredSubmodule ("Mu_Basecore", True),
            RequiredSubmodule ("Silicium-ACPI", True),
            RequiredSubmodule ("Silicon/Silicium/OpensslPkg/Library/OpensslLib/openssl", True)
        ]

    def SetArchitectures (self, list_of_requested_architectures):
        unsupported = set(list_of_requested_architectures) - set(self.GetArchitecturesSupported())

        if (len(unsupported) > 0):
            errorString = ("Unsupported Architecture Requested: " + " ".join(unsupported))
            logging.critical (errorString)
            raise Exception (errorString)

        self.ActualArchitectures = list_of_requested_architectures

    def GetWorkspaceRoot (self):
        return CommonPlatform.WorkspaceRoot

    def GetActiveScopes (self):
        return CommonPlatform.Scopes

    def FilterPackagesToTest (self, changedFilesList: list, potentialPackagesList: list) -> list:
        build_these_packages = []
        possible_packages = potentialPackagesList.copy ()

        for f in changedFilesList:
            if "BaseTools" in f:
                if os.path.splitext(f) not in [".txt", ".md"]:
                    build_these_packages = possible_packages
                    break

            if "platform-build-run-steps.yml" in f:
                build_these_packages = possible_packages
                break

        return build_these_packages

    def GetPlatformDscAndConfig (self) -> tuple:
        return ("redwoodPkg/redwood.dsc", {})

    def GetName (self):
        return "redwood"

    def GetPackagesPath (self):
        return CommonPlatform.PackagesPath

# ####################################################################################### #
#                         Actual Configuration for Platform Build                         #
# ####################################################################################### #
class PlatformBuilder (UefiBuilder, BuildSettingsManager):
    def __init__ (self):
        UefiBuilder.__init__ (self)

    def AddCommandLineOptions (self, parserObj):
        parserObj.add_argument('-a', "--arch", dest="build_arch", type=str, default="AARCH64", help="Optional - CSV of architecture to build. AARCH64 is used for PEI and DXE and is the only valid option for this platform.")

    def RetrieveCommandLineOptions (self, args):
        if args.build_arch.upper() != "AARCH64":
            raise Exception("Invalid Arch Specified.  Please see comments in DeviceBuild.py::PlatformBuilder::AddCommandLineOptions")

    def GetWorkspaceRoot (self):
        return CommonPlatform.WorkspaceRoot

    def GetPackagesPath (self):
        result = [ shell_environment.GetBuildVars().GetValue("FEATURE_CONFIG_PATH", "") ]

        for a in CommonPlatform.PackagesPath:
            result.append(a)

        return result

    def GetActiveScopes (self):
        return CommonPlatform.Scopes

    def GetName (self):
        return "redwoodPkg"

    def GetLoggingLevel (self, loggerType):
        return logging.INFO
        return super().GetLoggingLevel(loggerType)

    def SetPlatformEnv (self):
        logging.debug ("PlatformBuilder SetPlatformEnv")

        self.env.SetValue ("PRODUCT_NAME", "redwood", "Platform Hardcoded")
        self.env.SetValue ("ACTIVE_PLATFORM", "redwoodPkg/redwood.dsc", "Platform Hardcoded")
        self.env.SetValue ("TARGET_ARCH", "AARCH64", "Platform Hardcoded")
        self.env.SetValue ("TOOL_CHAIN_TAG", "CLANGPDB", "set default to clangpdb")
        self.env.SetValue ("EMPTY_DRIVE", "FALSE", "Default to false")
        self.env.SetValue ("RUN_TESTS", "FALSE", "Default to false")
        self.env.SetValue ("SHUTDOWN_AFTER_RUN", "FALSE", "Default to false")
        self.env.SetValue ("BLD_*_BUILDID_STRING", "Unknown", "Default")
        self.env.SetValue ("BUILDREPORTING", "TRUE", "Enabling build report")
        self.env.SetValue ("BUILDREPORT_TYPES", "PCD DEPEX FLASH BUILD_FLAGS LIBRARY FIXED_ADDRESS HASH", "Setting build report types")
        self.env.SetValue ("BLD_*_MEMORY_PROTECTION", "TRUE", "Default")
        self.env.SetValue ("BLD_*_SHIP_MODE", "FALSE", "Default")
        self.env.SetValue ("BLD_*_ENABLE_SECUREBOOT", self.env.GetValue("ENABLE_SECUREBOOT"), "Default")
        self.env.SetValue ("BLD_*_FD_BASE", self.env.GetValue("FD_BASE"), "Default")
        self.env.SetValue ("BLD_*_FD_SIZE", self.env.GetValue("FD_SIZE"), "Default")
        self.env.SetValue ("BLD_*_FD_BLOCKS", self.env.GetValue("FD_BLOCKS"), "Default")

        return 0

    def PlatformPreBuild (self):
        return 0

    def PlatformPostBuild (self):
        return 0

    def FlashRomImage (self):
        return 0

if __name__ == "__main__":
    import argparse
    import sys

    from edk2toolext.invocables.edk2_platform_build import Edk2PlatformBuild
    from edk2toolext.invocables.edk2_setup import Edk2PlatformSetup
    from edk2toolext.invocables.edk2_update import Edk2Update

    SCRIPT_PATH = os.path.relpath (__file__)

    parser = argparse.ArgumentParser (add_help=False)

    parse_group = parser.add_mutually_exclusive_group()

    parse_group.add_argument ("--update", "--UPDATE", action='store_true', help="Invokes stuart_update")
    parse_group.add_argument ("--setup",  "--SETUP",  action='store_true', help="Invokes stuart_setup")

    args, remaining = parser.parse_known_args()

    new_args = ["stuart", "-c", SCRIPT_PATH]
    new_args = new_args + remaining

    sys.argv = new_args

    if args.setup:
        Edk2PlatformSetup().Invoke()
    elif args.update:
        Edk2Update().Invoke()
    else:
        Edk2PlatformBuild().Invoke()
