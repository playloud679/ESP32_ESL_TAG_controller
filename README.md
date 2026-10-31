# esl-nemr-esp32

Pilotare i cartellini ESL NEMR 400×300 BWR via BLE con ESP32.

Riferimento tecnico per usare questo modello di cartellino elettronico (ESL)
e-paper in qualsiasi progetto: industriale (cartellini di magazzino, stato
macchine, kanban, segnaletica), domotico (dashboard, meteo, calendario,
targhette), commerciale (prezzi, avvisi) o di altro tipo.

Le sezioni 1-5 descrivono il dispositivo e sono **verificate sull'hardware
reale**. Le sezioni 6-9 sono indicazioni di progetto: dove una cosa non è
stata provata è scritto **[non verificato]**.

> Il formato dell'immagine e la risoluzione sono stati ricavati con immagini
> di test e foto del display. Il campo `typ2` dei metadati e un impacchettamento
> per colonne portano a risultati sbagliati su questo modello (vedi sezioni 2 e 4).

---

## 1. Il dispositivo

| Proprietà | Valore |
|---|---|
| Etichetta prodotto | `ESL_BWR4.4_BLE` |
| Nome BLE | `NEMR` + 8 cifre esadecimali dell'ID (es. `NEMR94205569`) |
| Indirizzo BLE | `ff:ff:` + ID (es. `ff:ff:94:20:55:69`), random static. L'ID è anche stampato sotto il codice a barre (`94.20.55.69`) |
| Display | e-paper **400×300**, 3 colori: bianco / nero / rosso |
| Alimentazione | batteria interna. La tensione è nell'advertising |
| Aggiornamento | solo a schermo intero. Nessun aggiornamento parziale o veloce noto |
| Immagine | **persistente senza alimentazione** (e-paper): consuma solo quando cambia |

Riconoscimento generico di un tag della famiglia (vale per più tag insieme):
manufacturer data che inizia con il magic `53 50`, nome che inizia con `NEMR`.

---

## 2. Advertising e metadati

Manufacturer specific data (7 byte), esempio `53 50 4B 1E 81 01 40`:

| Byte | Campo | Decodifica |
|---|---|---|
| 0-1 | magic | `53 50` |
| 2 | screen type | bit0 mirror · bit1-2 colore (0=BW, 1=BWR, 2=BWY) · bit3-4 tecnologia (0=TFT, 1=EPA, 2=EPA-1) · **bit5-7 pixelSize** |
| 3 | volt10 | tensione batteria ×10 (`1E` = 3,0 V) |
| 4 | sw | versione firmware |
| 5 | hw | versione hardware |
| 6 | typ2 | bit3-7 deviceType |

Tabella `pixelSize` (risoluzione): 0=212×104, 1=128×296, **2=400×300**,
3=640×384, 4=960×640, 5=196×96, 6=640×480, 7=250×122.

**Su questo modello la risoluzione corretta è quella di `pixelSize`.**
`typ2` vale 8, che nelle tabelle dei tipi NEMR corrisponde a 250×128: su questo modello è sbagliato.
Con quella risoluzione il tag accetta i dati ma mostra solo rosso.

Usi pratici dell'advertising, senza bisogno di connettersi:
- **inventario** dei tag presenti (indirizzo, RSSI);
- **monitoraggio della batteria** (`volt10`) per programmare le sostituzioni;
- **presenza/prossimità** di massima tramite RSSI [non verificato come metodo affidabile].

---

## 3. Protocollo GATT

Servizio `0000fef0-0000-1000-8000-00805f9b34fb`:

| Caratteristica | Uso | Proprietà |
|---|---|---|
| `FEF1` | comandi; le risposte arrivano come **notify** su FEF1 | write, write-no-response, notify |
| `FEF2` | dati dell'immagine | write, write-no-response, notify |

Valori multi-byte little-endian. Sequenza completa di un aggiornamento:

| # | Scrittura | Risposta (notify) |
|---|---|---|
| 1 | FEF1 `01` | `01 LL HH`: blockSize (vale 244) |
| 2 | FEF1 `02` + size u32 + `00 00 00` (8 byte) | `02 00` = ok, altrimenti errore |
| 3 | FEF1 `03` | `05 00 …` = pronto |
| 4 | FEF2 chunkIndex u32 + fino a blockSize−4 byte di dati | `05 st n0 n1 n2 n3`: `st == 08` → finito, altrimenti `n` = indice del prossimo chunk |
| — | FEF1 `04` (annulla) | `04 00` |

- Il chunk successivo è sempre quello indicato dal tag (offset = `idx * (blockSize-4)`).
- Dopo la disconnessione il tag ridisegna lo schermo.

### Regole di robustezza (verificate)

1. **Pausa di ~300 ms dopo aver abilitato le notify**: senza, la prima
   risposta va persa.
2. **MTU ≥ 247** (`BLEDevice::setMTU(247)` prima di connettersi): i chunk sono da 244 byte.
3. Connettersi con l'oggetto `BLEAdvertisedDevice` preso dalla scansione
   (indirizzo random): connettersi con un indirizzo scritto a mano può fallire.
4. **Durante il ridisegno il tag non fa advertising (~20-30 s)**: per un nuovo
   invio, prima fare una scansione e riprovare dopo qualche secondo.
5. Prevedere timeout per ogni risposta (2 s per i comandi, ~10 s per i chunk)
   e qualche nuovo tentativo per chunk.

### Tempi misurati (ESP32-C3, tag a ~1 m)

| Fase | Durata |
|---|---|
| Scansione per trovare il tag | fino a 10 s (di solito < 1 s) |
| Connessione e servizi | ~1 s |
| Trasferimento 30400 byte | **~13 s** |
| Ridisegno e-paper (tag irraggiungibile) | ~15-25 s |

Conseguenza: **al massimo un aggiornamento ogni ~40 s per tag** e, in
pratica, molto meno spesso per non consumare la batteria. L'e-paper conviene
quando il contenuto cambia di rado: minuti, ore, eventi.

---

## 4. Formato dell'immagine

```
W = 400, H = 300
ROW_BYTES = 50                          # per righe, 8 pixel per byte, bit7 = pixel a SINISTRA
size      = 30400                       # valore per il comando 02: (H+7)/8*W*2, accettato dal tag
BW  plane : offset 0,     15000 byte,   bit 1 = BIANCO, 0 = NERO
RED plane : offset 15000, 15000 byte,   bit 1 = ROSSO,  0 = nessun rosso
30000..30399 : padding a zero
pixel (x,y): ofs = y*50 + x/8 ; bit = 0x80 >> (x%8)
```

- Il rosso ha la precedenza sul bianco/nero. Sotto i pixel rossi il piano B/W
  va messo a bianco.
- Il piano rosso parte da **15000** (subito dopo il B/W), **non** da 15200: con
  15200 tutto il rosso scende di 4 righe.
- Orientamento: con il codice a barre in basso, (0,0) è in alto a sinistra.
  Nessuno specchiamento.
- Colori intermedi (grigi, rosa) si ottengono solo con il **dithering** (es.
  Bayer 8×8): sull'e-paper rende bene per sfumature e sfondi.

Codice di riferimento (C++, con due `GFXcanvas1` 400×300, `cBlack` e `cRed`):

```cpp
static const uint32_t ROW_BYTES = 50, RED_OFS = 50 * 300;
static uint8_t frame[30400];
memset(frame, 0, sizeof(frame));
for (int y = 0; y < 300; y++)
  for (int x = 0; x < 400; x++) {
    uint32_t ofs = y * ROW_BYTES + x / 8;
    uint8_t bit = 0x80 >> (x % 8);
    bool red = cRed.getPixel(x, y);
    if (red) frame[RED_OFS + ofs] |= bit;
    if (red || !cBlack.getPixel(x, y)) frame[ofs] |= bit;
  }
```

Come convertire un'immagine RGB in tre colori (soglie usate con successo):
rosso se `r>150 && r>g+b`, nero se la luminosità media `< 120`, altrimenti bianco.

---

## 5. Ambiente di sviluppo (ESP32)

- Qualsiasi ESP32 con BLE (testato su **ESP32-C3 Super Mini**). Core Arduino
  `esp32:esp32` ≥ 3.3: la libreria `BLE` usa NimBLE con l'API classica.
- `Adafruit GFX Library` per testo, forme e font (`Fonts/FreeSans*`, solo ASCII:
  le lettere accentate vanno evitate o fornite con un font personalizzato).
- Compilazione e caricamento:
  ```sh
  P=/dev/$(ls /dev | grep -E '^cu\.(usbmodem|usbserial)' | head -1)
  arduino-cli compile -b esp32:esp32:esp32c3:CDCOnBoot=cdc -u -p $P <sketch>
  ```
  Su C3 e S3 con USB nativa serve `CDCOnBoot=cdc` perché `Serial` esca sulla
  USB. Per i log BLE aggiungere `,DebugLevel=info`.
- RAM: frame da 30 KB più due canvas da 15 KB. Va bene anche su C3, che ha la
  RAM più ridotta.
- Preprocessore Arduino: genera i prototipi prima dei tipi definiti nello
  sketch. Usare tipi base nelle firme o mettere i tipi in un `.h`.
- Problemi della porta USB (compare e scompare, o non c'è): **cavo o porta**,
  non il firmware.

---

## 6. Architetture d'uso

### A. Autonomo
L'ESP32 genera il contenuto e lo invia: orologio, contatori, dati da
sensori collegati. Semplice, nessuna infrastruttura.

### B. Gateway BLE in rete (consigliato per casa e industria)
L'ESP32 fa solo da ponte tra rete e BLE; il contenuto lo genera un server.

```
Server / Home Assistant / PLC / ERP
        │  (Wi-Fi: HTTP, MQTT)  oppure  (USB seriale)
        ▼
   ESP32 gateway ── BLE ──► tag 1, tag 2, … tag N
```

Payload consigliato: l'immagine già nel formato del tag (30400 byte, sezione
4), più l'indirizzo del tag. Il gateway diventa generico e la grafica si fa
dove è più comoda (Python/Pillow, browser, Node-RED). Varianti:
- **MQTT**: `esl/<id>/image` (binario) → il gateway invia e pubblica
  `esl/<id>/status` (ok/errore, tensione, RSSI). Si integra facilmente con Home
  Assistant e con gli SCADA [non verificato].
- **HTTP**: `POST /tag/<id>` con il corpo binario [non verificato].
- **Seriale**: il PC invia `<id>` + 30400 byte su USB. È la soluzione più
  semplice per un banco di prova [non verificato].

Su ESP32 con poca RAM serve un solo buffer da 30 KB per invio: gli
aggiornamenti vanno in coda e si fanno uno alla volta.

### C. Più tag
- Si fa una sola connessione alla volta, quindi gli aggiornamenti sono
  sequenziali: ~15 s per tag (più la scansione).
- Una scansione periodica mantiene la mappa id → ultimo advertising (RSSI,
  tensione, ultimo avvistamento).
- Tag fuori portata o in ridisegno: rimettere in coda e riprovare.
- Copertura di aree grandi: più gateway, ognuno per i tag che vede con il
  segnale migliore [non verificato].

---

## 7. Linee guida di design per e-paper 400×300 BWR

- **Contrasto massimo**: nero su bianco per il testo principale, il rosso per
  stati e allarmi (es. "FERMO", "SCORTA BASSA") o come accento.
- Testo leggibile a distanza: font bold da 24 pt in su, eventualmente scalato
  ×2 (`setTextSize(2)`).
- Il rosso e-paper è più lento e più scuro: usarlo a superfici piene, non per
  dettagli sottili.
- Mettere sempre sul tag l'**ora dell'ultimo aggiornamento**: un e-paper fermo
  sembra aggiornato anche quando non lo è.
- Lasciare qualche pixel di margine ai bordi: la cornice copre un po' l'area utile.
- Preparare i layout in anteprima sul PC (Pillow, o la stessa logica GFX
  compilata per il computer) prima di inviarli: ogni prova sul tag costa ~30 s.

Esempi di impiego:

| Settore | Contenuto tipico | Frequenza |
|---|---|---|
| Magazzino | codice articolo, descrizione, ubicazione, codice a barre/QR, scorta | a evento |
| Produzione | stato macchina, ordine in corso, pezzi fatti / da fare | 1-15 min |
| Uffici e sale | prenotazioni e occupazione | a evento |
| Casa | meteo, calendario, rifiuti da esporre, stato allarme | 15-60 min |
| Negozio | prezzo, promo | a evento |

Codici a barre o QR: generarli sul server (es. `qrcode` o `python-barcode`)
e convertirli in nero/bianco puro, senza dithering, controllando che restino
leggibili alla risoluzione del tag [non verificato].

---

## 8. Batteria e affidabilità

- Il consumo dipende dal numero di aggiornamenti: **ridurli al minimo**,
  inviando solo quando il contenuto cambia davvero (confrontare un hash del
  frame con l'ultimo inviato).
- Leggere `volt10` dall'advertising e segnalare quando scende [soglia di
  allarme da definire, non verificata].
- Dopo l'invio non c'è conferma che il ridisegno sia riuscito: `st == 08`
  dice solo che il trasferimento è finito. Per i casi critici, verificare che
  il tag torni a fare advertising dopo ~30 s.
- Connessione BLE e trasferimento non sono cifrati né autenticati: chiunque
  sia in portata può riscrivere il tag. Da tenere presente per usi critici o
  in luoghi pubblici.

---

## 9. Codice di esempio in questo repo

| Cartella | Contenuto |
|---|---|
| `esl_scan/` | Scansione e decodifica dell'advertising dei tag NEMR |
| `esl_clock/` | Rendering con GFX, ora da seriale (`T<epoch>\n`), aggiornamento periodico |
| `esl_poster/` | Grafica complessa: dithering Bayer, primitive, testo con contorno, invio singolo |
| `esl_logo/` | Immagine precalcolata in un header più `logo2esl.py` (immagine qualsiasi → `logo_frame.h` + anteprima PNG) |

Tutti contengono le stesse funzioni da riusare: `request()` (scrive e aspetta
la notify con timeout), `sendFrame()` (protocollo della sezione 3),
`updateTag()` (scansione → connessione → invio → disconnessione).

`logo2esl.py` (serve Pillow):
```sh
python3 logo2esl.py <immagine> logo_frame.h anteprima.png
```
Ritaglia sul contenuto, scala con margini (alto 6, basso 22, lati 8 px) e
produce il frame nel formato della sezione 4. Si può riusare lato server
nell'architettura B, inviando il `bytearray` invece di scrivere l'header.
