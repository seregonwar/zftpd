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
 * @file ftp_protocol.h
 * @brief FTP protocol parsing and command dispatch
 * 
 * @author SeregonWar
 * @version 1.0.0
 * @date 2026-02-13
 * 
 * PROTOCOL: RFC 959 (File Transfer Protocol)
 * EXTENSIONS: RFC 3659 (MLST, MLSD, SIZE)
 * 
 */

#ifndef FTP_PROTOCOL_H
#define FTP_PROTOCOL_H

#include "ftp_types.h"

/*===========================================================================*
 * COMMAND PARSING
 *===========================================================================*/

/**
 * @brief Parse FTP command line
 * 
 * Splits "COMMAND arguments\r\n" into command and arguments.
 * 
 * EXAMPLES:
 *   "USER anonymous\r\n" -> command="USER", args="anonymous"
 *   "PWD\r\n" -> command="PWD", args=NULL
 *   "CWD /home/user\r\n" -> command="CWD", args="/home/user"
 * 
 * @param line     Input command line (null-terminated)
 * @param command  Output buffer for command name
 * @param args     Output buffer for arguments (may be NULL)
 * @param cmd_size Size of command buffer
 * @param args_size Size of args buffer
 * 
 * @return FTP_OK on success, negative error code on failure
 * @retval FTP_OK Command parsed successfully
 * @retval FTP_ERR_INVALID_PARAM Invalid parameters
 * @retval FTP_ERR_PROTOCOL Malformed command line
 * 
 * @pre line != NULL
 * @pre command != NULL
 * @pre cmd_size > 0
 * 
 * @post command contains uppercase command name
 * @post args is NULL or contains arguments (trimmed)
 * 
 * @note Thread-safety: Safe (no shared state)
 * @note WCET: O(n) where n = strlen(line)
 */
ftp_error_t ftp_parse_command_line(const char *line,
                                    char *command,
                                    char *args,
                                    size_t cmd_size,
                                    size_t args_size);

/**
 * @brief Find command handler by name
 * 
 * @param name Command name (case-insensitive)
 * 
 * @return Pointer to command entry, or NULL if not found
 * 
 * @pre name != NULL
 * 
 * @note Thread-safety: Safe (command table is const)
 * @note WCET: O(n) where n = number of commands (~30)
 */
const ftp_command_entry_t* ftp_find_command(const char *name);

/**
 * @brief Validate command arguments
 * 
 * Checks if arguments match command requirements:
 * - ARGS_NONE: No arguments allowed
 * - ARGS_REQUIRED: Arguments must be present
 * - ARGS_OPTIONAL: Arguments optional
 * 
 * @param cmd  Command entry
 * @param args Arguments string (NULL if none)
 * 
 * @return FTP_OK if valid, negative error code otherwise
 * 
 * @pre cmd != NULL
 */
ftp_error_t ftp_validate_command_args(const ftp_command_entry_t *cmd,
                                       const char *args);

/**
 * @brief Get command table
 * 
 * @param count Output parameter for command count
 * 
 * @return Pointer to command table
 * 
 * @pre count != NULL
 * 
 * @post *count contains number of commands
 */
const ftp_command_entry_t* ftp_get_command_table(size_t *count);

/*===========================================================================*
 * REPLY FORMATTING
 *===========================================================================*/

/**
 * @brief Format FTP reply message
 * 
 * Creates RFC 959-compliant reply: "CODE Message\r\n"
 * 
 * @param code    FTP reply code (200-599)
 * @param message Reply message (NULL for default)
 * @param buffer  Output buffer
 * @param size    Size of output buffer
 * 
 * @return Number of bytes written, or negative on error
 * 
 * @pre buffer != NULL
 * @pre size >= 64
 * 
 * @post buffer contains formatted reply
 */
ssize_t ftp_format_reply(ftp_reply_code_t code,
                          const char *message,
                          char *buffer,
                          size_t size);

/**
 * @brief Get default message for reply code
 * 
 * @param code FTP reply code
 * 
 * @return Default message string, or NULL if unknown
 */
const char* ftp_get_default_reply_message(ftp_reply_code_t code);

#endif /* FTP_PROTOCOL_H */
