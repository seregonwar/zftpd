/**
 * @file pal_notification.h
 * @brief System notification abstraction
 */

#ifndef PAL_NOTIFICATION_H
#define PAL_NOTIFICATION_H

int pal_notification_init(void);
void pal_notification_shutdown(void);
void pal_notification_send(const char *message);

#endif /* PAL_NOTIFICATION_H */
