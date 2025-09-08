# Introduction

Power profiling is a profiler functionality that allows power consumption
measurements to be taken during Lemonade execution. It is only available
for Windows machines. Power profiling can be done using the 3rd party tool HWiNFO or the
private AMD AGT tool (AMD platforms only).  Instructions for each are below.

## Power Profiling with HWiNFO

To capture power data during the lemonade execution sequence with the HWiNFO tool:
1) Install the [HWiNFO](https://www.hwinfo.com/) tool
   - On the HWiNFO executable, select the Properties->Compatibility->"Run this program as an administrator" checkbox
   - The standard installation is at C:\Program Files\HWiNFO64\HWiNFO64.exe.  If you install it elsewhere,
     then set the HWINFO_PATH environment variable to point to the executable.
3) Run Lemonade from a CMD or PowerShell prompt with Administrator privileges.

You may then use the `--power-hwinfo` flag.  For example:

OGA iGPU:
```bash
                     lemonade --power-hwinfo -i microsoft/Phi-3-mini-4k-instruct oga-load --device igpu --dtype int4 oga-bench -p 1024
```

This generates a PNG file that is stored in the current folder.  This file
contains a figure plotting the power usage over the Lemonade tool sequence.  Learn more by running `lemonade -h`.

## Power Profiling with AGT

Please note that this tool only works on AMD platforms and is only available to AMD employees.

To capture power data during the lemonade execution sequence with the AGT tool:
1) Install the [AGT](https://amd.atlassian.net/wiki/spaces/AVD/pages/780420097/AGT+ATITOOL) tool
by downloading and unzipping the file.
2) Set the AGT_PATH environment variable to point to the agt_internal.exe executable.
3) Run Lemonade from a CMD or PowerShell prompt with Administrator privileges.

You may then use the `--power-agt` flag.  For example:

OGA iGPU:
```bash
                     lemonade --power-agt -i microsoft/Phi-3-mini-4k-instruct oga-load --device igpu --dtype int4 oga-bench -p 1024
```

This generates a PNG file that is stored in the current folder.  This file
contains a figure plotting the power usage over the Lemonade tool sequence.  Learn more by running `lemonade -h`.


## Troubleshooting

If you are getting error messages when trying to run power pofiling you may need to
set your Windows User Account Control to `Never notify`.  This should
be done ONLY when actually running the tool.  For security reasons, it is not
recommended to leave the setting at this value.  However, it may be needed for
Lemonade to automatically launch HWiNFO.

You may also need to set the ExecutionPolicy to RemoteSigned (or Unrestricted). In Windows PowerShell: `Set-ExecutionPolicy RemoteSigned`.
If you find you need this, then please restore it to its original setting after gathering
power measurements.
