/*
MIT License

Copyright (c) 2026 Seregon

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

/**
 * @file ftp_types.h
 * @brief Core type definitions for FTP server
 *
 * @author SeregonWar
 * @version 1.0.0
 * @date 2026-02-13
 *
 */

#ifndef FTP_TYPES_H
#define FTP_TYPES_H

#include "ftp_config.h"
#include "ftp_crypto.h"
#include <netinet/in.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <sys/types.h>

/*===========================================================================*
 * ERROR CODES
 *===========================================================================*/

/**
 * FTP server error codes
 * @note All errors are negative values
 * @note Zero indicates success
 */
typedef enum {
  FTP_OK = 0,                  /**< Operation successful */
  FTP_ERR_INVALID_PARAM = -1,  /**< Invalid parameter (NULL pointer, etc) */
  FTP_ERR_OUT_OF_MEMORY = -2,  /**< Memory allocation failed */
  FTP_ERR_SOCKET_CREATE = -3,  /**< Socket creation failed */
  FTP_ERR_SOCKET_BIND = -4,    /**< Socket bind failed */
  FTP_ERR_SOCKET_LISTEN = -5,  /**< Socket listen failed */
  FTP_ERR_SOCKET_ACCEPT = -6,  /**< Socket accept failed */
  FTP_ERR_SOCKET_SEND = -7,    /**< Socket send failed */
  FTP_ERR_SOCKET_RECV = -8,    /**< Socket receive failed */
  FTP_ERR_THREAD_CREATE = -9,  /**< Thread creation failed */
  FTP_ERR_FILE_OPEN = -10,     /**< File open failed */
  FTP_ERR_FILE_READ = -11,     /**< File read failed */
  FTP_ERR_FILE_WRITE = -12,    /**< File write failed */
  FTP_ERR_FILE_STAT = -13,     /**< File stat failed */
  FTP_ERR_DIR_OPEN = -14,      /**< Directory open failed */
  FTP_ERR_PATH_INVALID = -15,  /**< Invalid path (traversal attempt) */
  FTP_ERR_PATH_TOO_LONG = -16, /**< Path exceeds maximum length */
  FTP_ERR_NOT_FOUND = -17,     /**< File or directory not found */
  FTP_ERR_PERMISSION = -18,    /**< Permission denied */
  FTP_ERR_TIMEOUT = -19,       /**< Operation timed out */
  FTP_ERR_MAX_SESSIONS = -20,  /**< Maximum sessions reached */
  FTP_ERR_AUTH_FAILED = -21,   /**< Authentication failed */
  FTP_ERR_PROTOCOL = -22,      /**< Protocol violation */
  FTP_ERR_UNKNOWN = -99,       /**< Unknown error */
} ftp_error_t;

/*===========================================================================*
 * FTP REPLY CODES (RFC 959)
 *===========================================================================*/

/**
 * FTP reply codes
 * @note Standard codes from RFC 959
 */
typedef enum {
  /* Positive Preliminary (1xx) */
  FTP_REPLY_150_FILE_OK = 150, /**< File status okay */

  /* Positive Completion (2xx) */
  FTP_REPLY_200_OK = 200,                /**< Command okay */
  FTP_REPLY_211_SYSTEM_STATUS = 211,     /**< System status */
  FTP_REPLY_212_DIR_STATUS = 212,        /**< Directory status */
  FTP_REPLY_213_FILE_STATUS = 213,       /**< File status */
  FTP_REPLY_214_HELP = 214,              /**< Help message */
  FTP_REPLY_215_SYSTEM_TYPE = 215,       /**< NAME system type */
  FTP_REPLY_220_SERVICE_READY = 220,     /**< Service ready */
  FTP_REPLY_221_GOODBYE = 221,           /**< Closing connection */
  FTP_REPLY_225_DATA_OPEN = 225,         /**< Data connection open */
  FTP_REPLY_226_TRANSFER_COMPLETE = 226, /**< Transfer complete */
  FTP_REPLY_227_PASV_MODE = 227,         /**< Entering passive mode */
  FTP_REPLY_230_LOGGED_IN = 230,         /**< User logged in */
  FTP_REPLY_234_AUTH_OK = 234,           /**< AUTH mechanism accepted */
  FTP_REPLY_250_FILE_ACTION_OK = 250,    /**< File action okay */
  FTP_REPLY_257_PATH_CREATED = 257,      /**< Path created */

  /* Positive Intermediate (3xx) */
  FTP_REPLY_331_NEED_PASSWORD = 331, /**< Need password */
  FTP_REPLY_350_PENDING = 350,       /**< Pending further info */

  /* Transient Negative (4xx) */
  FTP_REPLY_421_SERVICE_UNAVAIL = 421,      /**< Service not available */
  FTP_REPLY_425_CANT_OPEN_DATA = 425,       /**< Can't open data connection */
  FTP_REPLY_426_TRANSFER_ABORTED = 426,     /**< Transfer aborted */
  FTP_REPLY_450_FILE_UNAVAILABLE = 450,     /**< File unavailable */
  FTP_REPLY_451_LOCAL_ERROR = 451,          /**< Local error */
  FTP_REPLY_452_INSUFFICIENT_STORAGE = 452, /**< Insufficient storage */

  /* Permanent Negative (5xx) */
  FTP_REPLY_500_SYNTAX_ERROR = 500,      /**< Syntax error */
  FTP_REPLY_501_SYNTAX_ARGS = 501,       /**< Syntax error in args */
  FTP_REPLY_502_NOT_IMPLEMENTED = 502,   /**< Not implemented */
  FTP_REPLY_503_BAD_SEQUENCE = 503,      /**< Bad command sequence */
  FTP_REPLY_504_NOT_IMPL_PARAM = 504,    /**< Not impl for parameter */
  FTP_REPLY_530_NOT_LOGGED_IN = 530,     /**< Not logged in */
  FTP_REPLY_532_NEED_ACCOUNT = 532,      /**< Need account */
  FTP_REPLY_550_FILE_ERROR = 550,        /**< File unavailable */
  FTP_REPLY_551_PAGE_TYPE_UNKNOWN = 551, /**< Page type unknown */
  FTP_REPLY_552_STORAGE_EXCEEDED = 552,  /**< Storage exceeded */
  FTP_REPLY_553_FILENAME_INVALID = 553,  /**< Filename not allowed */
} ftp_reply_code_t;

/*===========================================================================*
 * SESSION STATE
 *===========================================================================*/

/**
 * FTP session state machine
 * @note Atomic transitions to prevent race conditions
 */
typedef enum {
  FTP_STATE_INIT = 0,          /**< Initial state */
  FTP_STATE_CONNECTED = 1,     /**< TCP connected */
  FTP_STATE_AUTHENTICATED = 2, /**< User logged in */
  FTP_STATE_TRANSFERRING = 3,  /**< Active transfer */
  FTP_STATE_TERMINATING = 4,   /**< Closing session */
} ftp_session_state_t;

/**
 * Data connection mode
 */
typedef enum {
  FTP_DATA_MODE_NONE = 0,    /**< No data connection */
  FTP_DATA_MODE_ACTIVE = 1,  /**< Active mode (PORT) */
  FTP_DATA_MODE_PASSIVE = 2, /**< Passive mode (PASV) */
} ftp_data_mode_t;

/**
 * Transfer type (TYPE command)
 */
typedef enum {
  FTP_TYPE_ASCII = 'A',  /**< ASCII mode */
  FTP_TYPE_BINARY = 'I', /**< Binary/Image mode */
} ftp_transfer_type_t;

/**
 * Transfer mode (MODE command)
 */
typedef enum {
  FTP_MODE_STREAM = 'S',   /**< Stream mode (default) */
  FTP_MODE_BLOCK = 'B',    /**< Block mode (not implemented) */
  FTP_MODE_COMPRESS = 'C', /**< Compressed mode (not implemented) */
} ftp_transfer_mode_t;

/**
 * File structure (STRU command)
 */
typedef enum {
  FTP_STRU_FILE = 'F',   /**< File structure (default) */
  FTP_STRU_RECORD = 'R', /**< Record structure (not implemented) */
  FTP_STRU_PAGE = 'P',   /**< Page structure (not implemented) */
} ftp_file_structure_t;

/*===========================================================================*
 * SESSION STATISTICS
 *===========================================================================*/

/**
 * Per-session statistics
 * @note Cache-aligned to prevent false sharing
 */
typedef struct {
  atomic_uint_fast64_t bytes_sent;         /**< Total bytes sent */
  atomic_uint_fast64_t bytes_received;     /**< Total bytes received */
  atomic_uint_fast32_t files_sent;         /**< Files sent count */
  atomic_uint_fast32_t files_received;     /**< Files received count */
  atomic_uint_fast32_t commands_processed; /**< Commands processed */
  atomic_uint_fast32_t errors;             /**< Error count */
} ftp_session_stats_t;

/*===========================================================================*
 * SESSION STRUCTURE
 *===========================================================================*/

/**
 * FTP client session
 *
 * MEMORY LAYOUT: Optimized for cache-line alignment
 * SIZE: Approximately 2KB per session
 * THREAD SAFETY: Access from single thread (session thread)
 *
 * @note Structure members ordered to minimize padding
 */
typedef struct ftp_session {
  /* Control connection - accessed frequently (cache-hot) */
  int ctrl_fd;                  /**< Control socket descriptor */
  struct sockaddr_in ctrl_addr; /**< Client address */

  /* Data connection */
  int data_fd;                  /**< Data socket descriptor */
  int pasv_fd;                  /**< Passive listener socket */
  struct sockaddr_in data_addr; /**< Data connection address */
  ftp_data_mode_t data_mode;    /**< Active/Passive/None */

  /* Session state - atomic for thread-safe state queries */
  atomic_int state; /**< Current session state */

  /* Transfer parameters */
  ftp_transfer_type_t transfer_type;   /**< ASCII or Binary */
  ftp_transfer_mode_t transfer_mode;   /**< Stream/Block/Compress */
  ftp_file_structure_t file_structure; /**< File/Record/Page */
  off_t restart_offset;                /**< REST command offset */

  /* File system state */
  char root_path[FTP_PATH_MAX];   /**< Server root directory */
  char cwd[FTP_PATH_MAX];         /**< Current working directory */
  char rename_from[FTP_PATH_MAX]; /**< RNFR source path */

  /* Authentication */
  uint8_t auth_attempts; /**< Failed auth attempts */
  uint8_t authenticated; /**< Login status (0/1) */
  uint8_t user_ok;       /**< USER accepted (0/1) */
  uint8_t _padding1[1];  /**< Alignment padding */

  /* Control channel input buffering */
  char ctrl_rxbuf[FTP_CMD_BUFFER_SIZE]; /**< Buffered control input */
  uint16_t ctrl_rx_len;                 /**< Valid bytes in ctrl_rxbuf */
  uint16_t ctrl_rx_off;                 /**< Read offset into ctrl_rxbuf */

  /* Thread management */
  pthread_t thread;    /**< Session thread handle */
  uint32_t session_id; /**< Unique session ID */

  /* Timing */
  time_t connect_time;  /**< Connection timestamp */
  time_t last_activity; /**< Last command timestamp */

  uint64_t rl_tokens;  /**< Rate limiter tokens (bytes) */
  uint64_t rl_last_ns; /**< Last refill timestamp (ns) */

  /* Encryption (ChaCha20 stream cipher) */
  ftp_crypto_ctx_t crypto; /**< Per-session crypto context  */

  /* Client identification */
  char client_ip[INET_ADDRSTRLEN]; /**< Client IP (text) */
  uint16_t client_port;            /**< Client port */
  uint16_t _padding2;              /**< Alignment padding */

  /* Statistics */
  ftp_session_stats_t stats;

} ftp_session_t;

/*===========================================================================*
 * COMMAND HANDLER
 *===========================================================================*/

/**
 * Command argument requirements
 */
typedef enum {
  FTP_ARGS_NONE = 0,     /**< No arguments allowed */
  FTP_ARGS_REQUIRED = 1, /**< Arguments required */
  FTP_ARGS_OPTIONAL = 2, /**< Arguments optional */
} ftp_args_req_t;

/**
 * Command handler function pointer
 *
 * @param session Client session context
 * @param args    Command arguments (NULL if no args)
 *
 * @return FTP_OK on success, negative error code on failure
 *
 * @pre session != NULL
 * @pre args != NULL if command requires arguments
 */
typedef ftp_error_t (*ftp_cmd_handler_t)(ftp_session_t *session,
                                         const char *args);

/**
 * Command table entry
 */
typedef struct {
  const char *name;          /**< Command name (uppercase) */
  ftp_cmd_handler_t handler; /**< Handler function */
  ftp_args_req_t args_req;   /**< Argument requirements */
} ftp_command_entry_t;

/*===========================================================================*
 * SERVER CONTEXT
 *===========================================================================*/

/**
 * Global server context
 *
 * @note Single instance, initialized at startup
 * @note Thread-safe: atomic operations for shared state
 */
typedef struct {
  /* Server socket */
  int listen_fd;                  /**< Listening socket */
  struct sockaddr_in listen_addr; /**< Server bind address */
  uint16_t port;                  /**< Server port */
  uint16_t _padding;              /**< Alignment padding */

  /* Server state */
  atomic_int running;                   /**< Server running flag */
  atomic_uint_fast32_t active_sessions; /**< Active session count */

  /* Session management */
  ftp_session_t sessions[FTP_MAX_SESSIONS]; /**< Session pool */
  pthread_mutex_t session_lock;             /**< Session pool lock */

  /* Default paths */
  char root_path[FTP_PATH_MAX]; /**< Server root directory */

  /* Statistics */
  _Alignas(64) struct {
    atomic_uint_fast64_t total_connections;
    atomic_uint_fast64_t total_bytes_sent;
    atomic_uint_fast64_t total_bytes_received;
    atomic_uint_fast32_t total_errors;
  } stats;

} ftp_server_context_t;

/*===========================================================================*
 * COMPILE-TIME SIZE CHECKS
 *===========================================================================*/

/* Session structure size will vary by platform and alignment */
/* Typical size: 2-4KB per session */

/* Ensure reply codes fit in uint16_t */
_Static_assert(FTP_REPLY_553_FILENAME_INVALID <= UINT16_MAX,
               "Reply codes must fit in uint16_t");

#endif /* FTP_TYPES_H */
