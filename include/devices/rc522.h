/*
 * rc522.h — MFRC522 RFID reader (SPI): detection, UID, optional content
 * =====================================================================
 * Decision D4: the kit identifier is the card UID — any ISO 14443-A card
 * is acceptable. The content (first 16 useful bytes) is read IF the card
 * allows it: NTAG/Ultralight direct read, MIFARE Classic with factory key.
 */
#ifndef RC522_H
#define RC522_H

#include <stdbool.h>
#include <stddef.h>

/* Initializes the SPI bus and the reader; automatic detection.
   Returns true if an MFRC522 responds (otherwise: simulation mode). */
bool rc522_init(void);

/* Has the reader been detected at startup? */
bool rc522_detected(void);

/* Attempts ONE complete read (periodic call, ~100 ms):
   - uid_str  : hex UID ("A1B2C3D4" or 14 characters if 7 bytes)
   - data_str : 16 ASCII characters from block 4 (page 4 NTAG / block 4 Classic),
                empty string if the content is not readable.
   Returns true if a card was read. */
bool rc522_read_badge(char *uid_str, size_t uid_size,
                      char *data_str, size_t data_size);

#endif /* RC522_H */
