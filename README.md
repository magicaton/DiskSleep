# DiskSleep

A Windows CLI utility that allows you to put a hard disk to sleep and lock it so that other programs cannot wake it up.

## Warning

> **Use at your own risk.** I have no specialized knowledge of how to properly interact with hard drives at the hardware level. This tool replicates the behavior observed in other disk management software (hdparm, CrystalDiskInfo, etc.) using standard Win32 and ATA pass-through APIs. There is no guarantee that these operations are safe for your specific hardware.

> **Every spin-up cycle wears your drive.** Hard drives have a limited number of start/stop cycles (typically 300,000–600,000 over the drive's lifetime). For longevity, it's best to keep a drive either always running or always stopped — frequent toggling shortens its lifespan.

> **Drives spin up automatically on boot and wake from sleep/hibernate.** Any HDD spins up as soon as it receives power — this is inherent hardware behavior, not something the OS initiates. There is no built-in way to prevent this.

## Requirements

- Windows 10/11 (x64). May work on Windows 7, but untested.
- **Administrator privileges** — required for ATA pass-through, PnP management, and volume offline/online operations. The tool auto-elevates via UAC by default.

## How It Works

The primary commands are `sleep` and `wake`.

### Putting a disk to sleep

```
disksleep sleep D:
```

The default sleep sequence is: **lock volume → dismount → take offline → send STANDBY IMMEDIATE → disable PnP device**.

The key step is **PnP disable** — it removes the disk from Windows device tree so that no software can access it and accidentally wake it up. Without this step (using `--no-pnp`), the volume is only taken offline, which is less reliable: some software that accesses disks via direct device paths can still wake the drive.

However, some drives wake themselves up spontaneously some time after being PnP-disabled. If you observe this, try `--apm` (enables APM level 1, letting the drive manage its own power state) or `--no-pnp` (skip PnP disable entirely) to find what works for your disk.

**Important: PnP disable removes the drive letter.** After `sleep`, the drive letter (e.g., `D:`) disappears and Windows no longer knows about the disk. To wake the disk later, you need to identify it by one of:

- **Mapping (recommended):** save the drive letter mapping *before* sleeping:
  ```
  disksleep map set D: --file
  disksleep sleep D:
  disksleep wake D:
  ```
- **PnP instance ID:** note the ID shown during sleep (e.g., `SCSI\DISK&VEN_...`) and use it directly:
  ```
  disksleep wake "SCSI\DISK&VEN_WDC&PROD_WD40EFPX-68C6CN\4&12345&0&000000"
  ```
- **After the fact:** find disabled disks with `disksleep list --pnp` or Windows Device Manager.

### Waking a disk

```
disksleep wake D:
```

The wake sequence is: **enable PnP device → bring volume online → spin up**.

If the drive letter was mapped with `map set`, it will be resolved automatically. Otherwise, use the PnP instance ID directly.

### Automatic sleep on boot / wake from sleep

Most HDDs spin up on their own as soon as power is applied, so the drive will be running again after every boot or resume from sleep. As a workaround, you can create a **Task Scheduler** task that runs the sleep command automatically.

## Usage

```
disksleep <command> <target> [options]
```

`<target>` is a drive letter (`D` or `D:`), volume GUID path (`\\?\Volume{...}\`), or PnP instance ID (`SCSI\DISK&VEN_...\...`).

### Commands

| Command                                     | Description                                                                       |
|---------------------------------------------|-----------------------------------------------------------------------------------|
| `sleep <target>`                            | Full sleep sequence (HDD only)                                                    |
| `wake <target>`                             | Full wake sequence (HDD only)                                                     |
| `info <target>`                             | Show disk info: type, model, size, GUID, APM, power state, PnP ID                 |
| `list [--pnp]`                              | Enumerate all physical disks. `--pnp` also shows PnP IDs and disabled devices     |
| `offline <target>`                          | Lock + dismount + take volume offline                                             |
| `online <target>`                           | Bring volume online                                                               |
| `apm <target> get\|enable [1-254]\|disable` | Query or configure APM                                                            |
| `pnp <target> enable\|disable [--persist]`  | Enable/disable PnP device                                                         |

### Sleep options

| Option              | Description                                                                       |
|---------------------|-----------------------------------------------------------------------------------|
| `--no-dismount`     | Skip volume dismount                                                              |
| `--no-offline`      | Skip taking volume offline                                                        |
| `--no-standby`      | Skip STANDBY IMMEDIATE / APM step                                                 |
| `--apm`             | Use APM level 1 instead of STANDBY IMMEDIATE                                      |
| `--no-pnp`          | Skip PnP disable                                                                  |
| `--force`           | Skip state pre-check (proceed even if already asleep)                             |
| `--no-pnp-recovery` | Don't attempt PnP recovery if disk is inaccessible                                |
| `--persist`         | PnP disable survives reboot (default is session-only)                             |

### Wake options

| Option        | Description                                  |
|---------------|----------------------------------------------|
| `--no-pnp`    | Skip PnP enable                              |
| `--no-online` | Skip bringing volume online                  |
| `--no-spinup` | Skip forced spin-up                          |
| `--apm`       | Disable APM after waking (if it was enabled) |
| `--force`     | Skip state pre-check                         |

### Mapping commands

Save drive letter → volume GUID + PnP instance ID mappings so that drives can be identified after PnP disable.

```
disksleep map set <letter|all> --file|--reg
disksleep map rm  [letter|all] --file|--reg
disksleep map sync             --file|--reg
disksleep map show
```

`sync` refreshes the GUIDs and PnP IDs for all existing entries from the current system state.

`--file` stores mappings in `DiskSleep_map.ini` next to the executable. `--reg` stores them in `HKCU\Software\DiskSleep\Map`. File mappings take priority over registry.

### Global options

| Option         | Description                                              |
|----------------|----------------------------------------------------------|
| `--no-elevate` | Don't auto-elevate via UAC; fail if not running as admin |
| `--version`    | Show version and exit                                    |

## Build

Requires Visual Studio with the C++ Desktop workload (x64). The project ships with `PlatformToolset = v143` (VS 2022), but any toolset from **v140** (VS 2015) onward will work — just retarget via *Project Properties → General → Platform Toolset* or edit `<PlatformToolset>` in `DiskSleep.vcxproj`.

Open `src\DiskSleep.sln`, select the **Release** configuration, and build. Or from a Developer Command Prompt:

```
msbuild src\DiskSleep.sln -p:Configuration=Release
```

The output binary is placed in `bin\<Configuration>\DiskSleep.exe`.

### noCRT build

The noCRT configurations produce a standalone binary with **no C Runtime dependency** — it links only against system DLLs (kernel32, advapi32, shell32, setupapi, cfgmgr32).

**The noCRT build's stability is not guaranteed.** It uses custom replacements for CRT functions (via `ntdll.dll` and compiler intrinsics) which may not handle all edge cases. The standard (CRT) build is recommended for general use.

## Future Plans

A few ideas for the future, I can't guarantee that I'll get around to them.

- **GUI / Windows Explorer integration** — some form of graphical interface or shell extension for sleep/wake without the command line.
- **PUIS (Power-Up In Standby) support** — ATA feature that configures a drive to stay spun down on power-up, which would eliminate the need for Task Scheduler workarounds. Requires hardware support and may not work with all controllers.

## License

[0BSD](LICENSE)
