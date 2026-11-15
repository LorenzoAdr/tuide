# AUR package: tuide-bin

Repackages the official release AppImage for Arch Linux. The squashfs image inside the
AppImage is extracted at build time, so the installed program needs neither FUSE nor
AppImage support. Installs `/opt/tuide`, the `tuide` command, a desktop entry and an icon.

## First publication

1. Create an account at https://aur.archlinux.org and add your SSH public key in *My Account*.
2. Publish:

   ```bash
   git clone ssh://aur@aur.archlinux.org/tuide-bin.git aur-tuide-bin
   cp packaging/aur/PKGBUILD packaging/aur/.SRCINFO aur-tuide-bin/
   cd aur-tuide-bin
   git add PKGBUILD .SRCINFO
   git commit -m "tuide-bin 0.1.6"
   git push
   ```

## New release

1. Set `pkgver`, reset `pkgrel=1`, and replace the AppImage checksum with the line from the
   release's `SHA256SUMS`.
2. On Arch: `makepkg -f && makepkg --printsrcinfo > .SRCINFO`, then commit both files here and
   in the AUR repository.
