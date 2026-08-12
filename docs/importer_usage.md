# dopolnite cevovoda

1. on_seg_proc se preimenuje on_seg_check - namenjen je iskanju semantičnih napak v segmentu
2. doda se nova metoda on_block_end - namenjena je zapisu segmentov v datoteko, bazo podatkov, vrsto, itd.
3. Parametri klica so :
   - vektor indeksov
4. on_block_end se kliče vsakih n segmentov, pri čemer se n pridobi iz strukture importer_config.block_size