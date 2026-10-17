# **MeloCafe - Wii U emulator**

This is the code repository of MeloCafe, a Cemu based Wii U emulator that is able to run most Wii U games and homebrew in a playable state.
It's written in C/C++ and is being actively developed with new features and fixes.

MeloCafe is currently only available for iOS/iPadOS platforms.

### Links:
 - [Cemu Open Source Announcement](https://www.reddit.com/r/cemu/comments/wwa22c/cemu_20_announcement_linux_builds_opensource_and/)
 - [Official Website](https://melo.cafe)
 - [Official Discord](https://discord.gg/HjCDPTpC3W)

### Low-memory devices (page table mode)

MeloCafe normally reserves one contiguous 4GiB block of address space for the Wii U memory map. If that reservation
fails (for example on 3GB devices such as the iPhone SE), it automatically falls back to *page table mode*: only the
~1.9GiB of guest ranges that actually exist are reserved, and guest accesses go through a page table.
The AArch64 recompiler supports this mode too, at a small cost per memory access.

- `MELOCAFE_PAGETABLE=1` forces page table mode (useful for testing on devices where the 4GiB reservation works).
- `MELOCAFE_PAGETABLE=0` disables the automatic fallback.
- Limitations: graphic packs that enlarge RAM ranges and the overlay arena (448MiB) may be unavailable in this mode.

#### Other relevant repositories:
 - [Cemu's Community Graphic Packs](https://github.com/cemu-project/cemu_graphic_packs)

## Download & Install

You can download the latest MeloCafe releases for iOS from the [GitHub Releases](https://github.com/stossy11/MeloCafe/releases/).

Just like MeloNX, the `Increased Memory Limit` Entitlement is needed for the best experience.
 
Without `Increased Memory Limit` crashes may occur (especially when using JIT).

### Recommended Guide (Plumeimpactor)

> [SideStore](https://sidestore.io/) is recommended (optional) for an on-device Sideloader, and should be installed prior performing this install.

#### **Make sure to read the FAQ and Info before continuing.**

#### 1. Sideload Application
Download and install MeloCafe using [PlumeImpactor](https://github.com/claration/Impactor/releases) on a computer.
- [Download **MeloCafe** From Releases](https://github.com/stossy11/MeloCafe/releases)
- Open PlumeImpactor > Click Settings > Click Login
- Login with the same Apple ID you are using for SideStore (or AltStore).
- Import the MeloCafe .ipa you downloaded earlier.
- Plug in your iDevice.
- Select your iDevice from the dropdown at the top of the window.
- Click Install.

#### 2. Load Into SideStore (Optional)
To have MeloCafe show inside SideStore (or AltStore), You must re-install it:
- Open **SideStore** on your iDevice
- Select the **My Apps** tab > Tap the **+** button.
- Select the **MeloCafe** .ipa (You may need to download it again.)
- Wait for it to Sideload, then it should show up Inside **SideStore**.
- Now You can Refresh **MeloCafe** and Update it without needing a computer.

#### 5. Enable JIT (Optional)
- Enable JIT using your preferred method, on iOS 26 [StikDebug](https://github.com/StephenDev0/StikDebug) is required.



## Build Instructions

To compile Cemu yourself on Windows, Linux or macOS, view [BUILD.md](/BUILD.md).

## Issues

Issues with the emulator should be filed using [GitHub Issues](https://github.com/stossy11/MeloCafe/issues). 


The Cemu bug trackers can be found at [Github Issues](https://github.com/cemu-project/Cemu/issues) and [bugs.cemu.info](https://bugs.cemu.info) which may contain relevant issues.

## Contributing

Pull requests are very welcome. For easier coordination you can chat with us about your contribution on [Discord](https://discord.gg/HjCDPTpC3W).
Before submitting a pull request, please read and follow our code style guidelines listed in [CODING_STYLE.md](/CODING_STYLE.md).

If coding isn't your thing, testing games and making detailed bug reports or updating the (usually outdated) compatibility wiki is also appreciated!


#### AI generated contributions:

We ask that all code submitted is written and understood by a human. You can use AI for planning, designing, reviewing and for asking questions about the codebase, but the code itself needs to be written by you. As a small exception you can use intellisense-style AI code autocompletion for pure boilerplate code as long as it's only a small part of your submission. To further clarify, when we ask for "human written" that excludes letting an AI write the code and then paraphrasing it. In other words, we are asking for human effort.

Why this policy exists:

We have relatively low reviewing capacity and requiring human-written code increases the quality and trustworthyness of submitted pull requests. There are also general concerns with AI usage in emulation:
- LLMs tend to make up solutions that work on the surface but are generally not accurate in the emulation sense
- There is evidence that LLMs have been trained on leaked proprietary SDKs and we cannot verify the origin of the knowledge. This is especially a problem for core emulation logic

Please keep these points in mind when contributing to Cemu. Contributions that do not follow this policy may be rejected.

## License
Cemu (and by extension MeloCafe) is licensed under [Mozilla Public License 2.0](/LICENSE.txt). Exempt from this are all files in the dependencies directory for which the licenses of the original code apply as well as some individual files in the src folder, as specified in those file headers respectively.
