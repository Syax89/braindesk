# Immagine di prova per brainshell: compilatore nativo, Xvfb, JWM, strumenti
# per pilotare l'interfaccia (xdotool) e catturare lo schermo (ImageMagick).
# Contiene anche le librerie armel + qemu-user per eseguire il binario ARM.
FROM debian:trixie
ENV DEBIAN_FRONTEND=noninteractive
RUN dpkg --add-architecture armel && apt-get update && apt-get install -y --no-install-recommends \
      gcc libc6-dev make pkg-config libx11-dev libxext-dev libstb-dev \
      xvfb xauth xdotool imagemagick jwm wmctrl \
      x11-utils x11-apps x11-xserver-utils xterm \
      fonts-dejavu-core fonts-material-design-icons-iconfont fonts-ipaexfont-gothic \
      procps strace time ca-certificates curl \
      qemu-user libc6:armel libx11-6:armel libxext6:armel \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
