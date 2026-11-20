#!/bin/sh
# Installa BrainDesk sul Brain (eseguire come utente "user" con sudo).
set -e
cd "$(dirname "$0")"
[ -x brainbar ] || gcc -O2 -o brainbar brainbar.c $(pkg-config --cflags --libs xft x11)
sudo install -m 755 brainbar brain-video brain-view brain-rotate brain-brightness /usr/local/bin/
sudo install -m 755 pxpvideo pxpwin brainshot brain-screenshot brain-ctl brain-tap brain-key early/brain-session boot/brain-trial /usr/local/bin/
for b in brainshell brainshell-x11 brainplay; do [ -x $b ] && sudo install -m 755 $b /usr/local/bin/; done
# avvio rapido: interfaccia prima di systemd, X senza GLX, mappa tastiera in cache
sudo install -m 755 early/brain-preinit early/brain-ui early/brain-shell-session early/brain-xconf \
	early/brain-xapp early/brain-xapp-client /usr/local/sbin/
for f in brain-xapp brain-suspend; do sudo install -m 440 early/$f.sudoers /etc/sudoers.d/$f; done
sudo visudo -c >/dev/null
sudo /usr/local/sbin/brain-xconf
# tasto di accensione gestito da BrainShell
sudo install -d /etc/systemd/logind.conf.d
sudo install -m 644 early/90-brain-logind.conf /etc/systemd/logind.conf.d/
sudo install -m 644 early/brain-xorg.conf /etc/X11/
[ -e /usr/bin/xkbcomp.real ] || sudo dpkg-divert --quiet --add --rename --divert /usr/bin/xkbcomp.real /usr/bin/xkbcomp
sudo install -m 755 early/xkbcomp-cache /usr/bin/xkbcomp
sudo install -m 755 early/brain-zram brain-suspend /usr/local/sbin/
sudo install -m 644 early/brain-zram.service /etc/systemd/system/ && sudo systemctl enable -q brain-zram.service
sudo sed -i 's|^/swapfile|#/swapfile|' /etc/fstab
sudo install -m 644 boot/brain-bootok.service /etc/systemd/system/ && sudo systemctl enable -q brain-bootok.service
# servizi inutili per BrainDesk: brain-config (cpufreq assente, legge gli input per 1 s), getty su tty1 (lo schermo e' della shell)
sudo systemctl mask -q brain-config.service getty@tty1.service
sudo rm -rf /var/log/journal   # giornale solo in RAM
sudo systemctl disable -q apt-daily.timer apt-daily-upgrade.timer man-db.timer dpkg-db-backup.timer fstrim.timer systemd-timesyncd.service 2>/dev/null || true
sudo install -m 644 99-brain-pxp.rules /etc/udev/rules.d/
sudo udevadm control --reload 2>/dev/null; sudo udevadm trigger --name-match=pxp 2>/dev/null || true
sudo install -m 644 xorg.conf /etc/X11/xorg.conf
sudo install -d /usr/local/share/brain/dtb
sudo install -m 644 dtb/*.dtb /usr/local/share/brain/dtb/
sudo install -d /usr/local/share/applications /usr/local/share/brain
sudo install -m 644 brain-video.desktop brain-view.desktop /usr/local/share/applications/
install -m 644 wallpaper.jpg ~/.wallpaper.jpg
[ -f demo.mp4 ] && sudo install -m 644 demo.mp4 /usr/local/share/brain/

install -m 755 xinitrc ~/.xinitrc
install -m 644 jwmrc ~/.jwmrc
install -m 644 jwmrc-shell ~/.jwmrc-shell
install -m 644 bash_profile ~/.bash_profile
mkdir -p ~/Video ~/Immagini ~/Documenti ~/.config
printf '[Default Applications]\nvideo/mp4=brain-video.desktop\nvideo/x-matroska=brain-video.desktop\nvideo/webm=brain-video.desktop\nvideo/x-msvideo=brain-video.desktop\nvideo/mpeg=brain-video.desktop\nimage/jpeg=brain-view.desktop\nimage/png=brain-view.desktop\nimage/gif=brain-view.desktop\n' > ~/.config/mimeapps.list
[ -f demo.mp4 ] && ln -sf /usr/local/share/brain/demo.mp4 ~/Video/demo-mandelbrot.mp4

# niente input method giapponese nella sessione
: > ~/.xprofile

sudo rm -f /etc/systemd/system/getty@tty1.service.d/autologin.conf
sudo usermod -aG audio,video,input user
echo "BrainDesk installato"
