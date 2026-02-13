/**
 * @file ftp_commands.h
 * @brief FTP command handlers (RFC 959)
 * 
 * @author SeregonWar
 * @version 1.0.0
 * @date 2025-02-13
 * 
 * PROTOCOL: RFC 959 (File Transfer Protocol)
 * EXTENSIONS: RFC 3659 (MLST, MLSD, SIZE, MDTM)
 * 
 * SAFETY CLASSIFICATION: Embedded systems, production-grade
 * STANDARDS: MISRA C:2012, CERT C, ISO C11
 */

#ifndef FTP_COMMANDS_H
#define FTP_COMMANDS_H

#include "ftp_types.h"

/*===========================================================================*
 * AUTHENTICATION AND CONTROL
 *===========================================================================*/

/**
 * @brief USER command - Specify user name
 * 
 * @param session Client session
 * @param args    User name
 * 
 * @return FTP_OK on success, negative error code on failure
 * 
 * @pre session != NULL
 * @pre args != NULL (validated by protocol layer)
 */
ftp_error_t cmd_USER(ftp_session_t *session, const char *args);

/**
 * @brief PASS command - Specify password
 * 
 * @param session Client session
 * @param args    Password (optional for anonymous)
 * 
 * @return FTP_OK on success, negative error code on failure
 */
ftp_error_t cmd_PASS(ftp_session_t *session, const char *args);

/**
 * @brief QUIT command - Terminate session
 * 
 * @param session Client session
 * @param args    (unused)
 * 
 * @return FTP_OK (session will be closed after reply)
 */
ftp_error_t cmd_QUIT(ftp_session_t *session, const char *args);

/**
 * @brief NOOP command - No operation
 * 
 * @param session Client session
 * @param args    (unused)
 * 
 * @return FTP_OK
 */
ftp_error_t cmd_NOOP(ftp_session_t *session, const char *args);

/*===========================================================================*
 * NAVIGATION
 *===========================================================================*/

/**
 * @brief CWD command - Change working directory
 * 
 * @param session Client session
 * @param args    Directory path
 * 
 * @return FTP_OK on success, negative error code on failure
 */
ftp_error_t cmd_CWD(ftp_session_t *session, const char *args);

/**
 * @brief CDUP command - Change to parent directory
 * 
 * @param session Client session
 * @param args    (unused)
 * 
 * @return FTP_OK on success, negative error code on failure
 */
ftp_error_t cmd_CDUP(ftp_session_t *session, const char *args);

/**
 * @brief PWD command - Print working directory
 * 
 * @param session Client session
 * @param args    (unused)
 * 
 * @return FTP_OK
 */
ftp_error_t cmd_PWD(ftp_session_t *session, const char *args);

/*===========================================================================*
 * DIRECTORY LISTING
 *===========================================================================*/

/**
 * @brief LIST command - List directory contents (detailed)
 * 
 * @param session Client session
 * @param args    Directory path (optional, defaults to CWD)
 * 
 * @return FTP_OK on success, negative error code on failure
 */
ftp_error_t cmd_LIST(ftp_session_t *session, const char *args);

/**
 * @brief NLST command - Name list (simple)
 * 
 * @param session Client session
 * @param args    Directory path (optional)
 * 
 * @return FTP_OK on success, negative error code on failure
 */
ftp_error_t cmd_NLST(ftp_session_t *session, const char *args);

/**
 * @brief MLSD command - Machine-readable directory listing (RFC 3659)
 * 
 * @param session Client session
 * @param args    Directory path (optional)
 * 
 * @return FTP_OK on success, negative error code on failure
 */
ftp_error_t cmd_MLSD(ftp_session_t *session, const char *args);

/**
 * @brief MLST command - Machine-readable file info (RFC 3659)
 * 
 * @param session Client session
 * @param args    File path (optional)
 * 
 * @return FTP_OK on success, negative error code on failure
 */
ftp_error_t cmd_MLST(ftp_session_t *session, const char *args);

/*===========================================================================*
 * FILE TRANSFER
 *===========================================================================*/

/**
 * @brief RETR command - Retrieve (download) file
 * 
 * @param session Client session
 * @param args    File path
 * 
 * @return FTP_OK on success, negative error code on failure
 * 
 * @note Uses zero-copy sendfile() when available
 */
ftp_error_t cmd_RETR(ftp_session_t *session, const char *args);

/**
 * @brief STOR command - Store (upload) file
 * 
 * @param session Client session
 * @param args    File path
 * 
 * @return FTP_OK on success, negative error code on failure
 */
ftp_error_t cmd_STOR(ftp_session_t *session, const char *args);

/**
 * @brief APPE command - Append to file
 * 
 * @param session Client session
 * @param args    File path
 * 
 * @return FTP_OK on success, negative error code on failure
 */
ftp_error_t cmd_APPE(ftp_session_t *session, const char *args);

/**
 * @brief REST command - Restart transfer
 * 
 * @param session Client session
 * @param args    Byte offset
 * 
 * @return FTP_OK on success, negative error code on failure
 */
ftp_error_t cmd_REST(ftp_session_t *session, const char *args);

/*===========================================================================*
 * FILE MANAGEMENT
 *===========================================================================*/

/**
 * @brief DELE command - Delete file
 * 
 * @param session Client session
 * @param args    File path
 * 
 * @return FTP_OK on success, negative error code on failure
 */
ftp_error_t cmd_DELE(ftp_session_t *session, const char *args);

/**
 * @brief RMD command - Remove directory
 * 
 * @param session Client session
 * @param args    Directory path
 * 
 * @return FTP_OK on success, negative error code on failure
 */
ftp_error_t cmd_RMD(ftp_session_t *session, const char *args);

/**
 * @brief MKD command - Make directory
 * 
 * @param session Client session
 * @param args    Directory path
 * 
 * @return FTP_OK on success, negative error code on failure
 */
ftp_error_t cmd_MKD(ftp_session_t *session, const char *args);

/**
 * @brief RNFR command - Rename from (step 1)
 * 
 * @param session Client session
 * @param args    Source path
 * 
 * @return FTP_OK on success, negative error code on failure
 */
ftp_error_t cmd_RNFR(ftp_session_t *session, const char *args);

/**
 * @brief RNTO command - Rename to (step 2)
 * 
 * @param session Client session
 * @param args    Destination path
 * 
 * @return FTP_OK on success, negative error code on failure
 * 
 * @pre RNFR must have been called first
 */
ftp_error_t cmd_RNTO(ftp_session_t *session, const char *args);

/*===========================================================================*
 * DATA CONNECTION
 *===========================================================================*/

/**
 * @brief PORT command - Active mode data connection
 * 
 * Format: PORT h1,h2,h3,h4,p1,p2
 * 
 * @param session Client session
 * @param args    Client address and port
 * 
 * @return FTP_OK on success, negative error code on failure
 */
ftp_error_t cmd_PORT(ftp_session_t *session, const char *args);

/**
 * @brief PASV command - Passive mode data connection
 * 
 * @param session Client session
 * @param args    (unused)
 * 
 * @return FTP_OK on success, negative error code on failure
 */
ftp_error_t cmd_PASV(ftp_session_t *session, const char *args);

/*===========================================================================*
 * INFORMATION
 *===========================================================================*/

/**
 * @brief SIZE command - Return file size (RFC 3659)
 * 
 * @param session Client session
 * @param args    File path
 * 
 * @return FTP_OK on success, negative error code on failure
 */
ftp_error_t cmd_SIZE(ftp_session_t *session, const char *args);

/**
 * @brief MDTM command - Return modification time (RFC 3659)
 * 
 * @param session Client session
 * @param args    File path
 * 
 * @return FTP_OK on success, negative error code on failure
 */
ftp_error_t cmd_MDTM(ftp_session_t *session, const char *args);

/**
 * @brief STAT command - Return status
 * 
 * @param session Client session
 * @param args    File/directory path (optional)
 * 
 * @return FTP_OK on success, negative error code on failure
 */
ftp_error_t cmd_STAT(ftp_session_t *session, const char *args);

/**
 * @brief SYST command - Return system type
 * 
 * @param session Client session
 * @param args    (unused)
 * 
 * @return FTP_OK
 */
ftp_error_t cmd_SYST(ftp_session_t *session, const char *args);

/**
 * @brief FEAT command - List features
 * 
 * @param session Client session
 * @param args    (unused)
 * 
 * @return FTP_OK
 */
ftp_error_t cmd_FEAT(ftp_session_t *session, const char *args);

/**
 * @brief HELP command - Return help information
 * 
 * @param session Client session
 * @param args    Command name (optional)
 * 
 * @return FTP_OK
 */
ftp_error_t cmd_HELP(ftp_session_t *session, const char *args);

/*===========================================================================*
 * TRANSFER PARAMETERS
 *===========================================================================*/

/**
 * @brief TYPE command - Set transfer type
 * 
 * Types: A (ASCII), I (Binary/Image)
 * 
 * @param session Client session
 * @param args    Type code
 * 
 * @return FTP_OK on success, negative error code on failure
 */
ftp_error_t cmd_TYPE(ftp_session_t *session, const char *args);

/**
 * @brief MODE command - Set transfer mode
 * 
 * Modes: S (Stream), B (Block), C (Compressed)
 * Note: Only Stream mode supported
 * 
 * @param session Client session
 * @param args    Mode code
 * 
 * @return FTP_OK on success, negative error code on failure
 */
ftp_error_t cmd_MODE(ftp_session_t *session, const char *args);

/**
 * @brief STRU command - Set file structure
 * 
 * Structures: F (File), R (Record), P (Page)
 * Note: Only File structure supported
 * 
 * @param session Client session
 * @param args    Structure code
 * 
 * @return FTP_OK on success, negative error code on failure
 */
ftp_error_t cmd_STRU(ftp_session_t *session, const char *args);

#endif /* FTP_COMMANDS_H */
