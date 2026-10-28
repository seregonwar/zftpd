#ifndef ZFTPD_INSTANCE_CONTROL_H
#define ZFTPD_INSTANCE_CONTROL_H

/* Outcome of instance_control_replace_previous(). Only OK guarantees that the
 * previous payload released the default ports; either way the caller starts. */
typedef enum {
  INSTANCE_REPLACE_OK = 0,
  /* A zftpd payload survived the cooperative stop and SIGTERM. */
  INSTANCE_REPLACE_CONFLICT = 1
} instance_replace_result_t;

/* Ask the previous payload to shut down and report whether it is gone.
 * Never fails: a survivor is a conflict, not an error. */
instance_replace_result_t instance_control_replace_previous(void);

/* Control listener and identity file for this payload. */
int instance_control_start(void);
void instance_control_stop(void);

/* Ask the payload's main thread to run its normal shutdown sequence. */
void zftpd_shutdown_request(void);

#endif
