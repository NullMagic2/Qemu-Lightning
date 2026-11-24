# Qemu-Lightning

Qemu-Lightning helps you run Windows virtual machines on your PC. On Linux, its desktop manager brings VM setup, accelerated graphics, clipboard sharing and file transfers into one place.

## Get started

Choose a download from the [releases page](https://github.com/NullMagic2/Qemu-Lightning/releases).

| Your system | What to download |
| --- | --- |
| Ubuntu 26.04 | The Ubuntu 26.04 AMD64 package |
| Ubuntu 24.04 or Linux Mint 22.x | The Noble AMD64 package |
| Windows x64 host | The native emulator ZIP. Extract it and run the emulator from PowerShell. |
| Windows x64 guest | Use the desktop graphics setup wizard inside the VM. The WDDM 2.2 preview is a separate download. |

On Linux, install the downloaded package with `sudo apt install ./qemu-lightning_*.deb` from its download directory, then open **Qemu-Lightning** from the applications menu. Create a VM and select your Windows installation ISO.

The Windows host build uses standard emulated graphics. The Lightning accelerated display and its guest driver currently require a Linux host. The guest graphics wizard requires the Windows Driver Kit signing tools and test signing in the guest. Native Direct3D 12 support is experimental.

## License

QEMU is GPLv2. Original, separately licensed Lightning components and packaging may use MIT. See [the license notes](LICENSE) for details.
