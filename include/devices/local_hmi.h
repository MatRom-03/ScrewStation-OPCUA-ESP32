/**
 * @file local_hmi.h
 * @brief Local HMI: LCD 1602 I2C + navigation encoder.
 * =================================================================
 * Independent task ("operator panel") that reads the machine image (g_plant)
 * and writes Settings, without ever delaying the 10 ms polling cycle.
 */
#ifndef LOCAL_HMI_H
#define LOCAL_HMI_H

/* HMI task (core 0, low priority). */
void local_hmi_task(void *arg);

#endif /* LOCAL_HMI_H */
