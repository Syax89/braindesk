Avvio del Brain (uEnv.txt, letto da U-Boot)

  normale   zImage + imx28-pwsh5.dtb            kernel BrainDesk
  prova     zImage.test + imx28-pwsh5.dtb.test  una volta sola, dopo "brain-trial"
  soccorso  zImage.brainux + *.dtb.brainux      kernel originale di Brainux

U-Boot conta gli avvii nel registro HW_RTC_PERSISTENT4 e Linux lo azzera
quando l'avvio e' completo (brain-bootok.service). Se l'avvio non arriva in
fondo per due volte di fila, la terza parte il kernel di soccorso:
basta premere reset tre volte, a qualche secondo l'una dall'altra.
"brain-trial" scrive in HW_RTC_PERSISTENT5 la richiesta di un avvio di prova.
