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
 * @file ftp_protocol.c
 * @brief FTP protocol parsing and command dispatch implementation
 *
 * @author SeregonWar
 * @version 1.0.0
 * @date 2026-02-13
 *
 */

#include "ftp_protocol.h"
#include "ftp_commands.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

/*===========================================================================*
 * COMMAND TABLE
 *===========================================================================*/

/**
 * FTP command lookup table
 *
 * DESIGN: Sorted alphabetically for future binary search optimization
 * NOTE: All command names MUST be uppercase
 */
static const ftp_command_entry_t command_table[] = {
    /* Authentication and control */
    {"USER", cmd_USER, FTP_ARGS_REQUIRED},
    {"PASS", cmd_PASS, FTP_ARGS_OPTIONAL},
    {"QUIT", cmd_QUIT, FTP_ARGS_NONE},
    {"NOOP", cmd_NOOP, FTP_ARGS_NONE},

    /* Navigation */
    {"CWD", cmd_CWD, FTP_ARGS_REQUIRED},
    {"CDUP", cmd_CDUP, FTP_ARGS_NONE},
    {"PWD", cmd_PWD, FTP_ARGS_NONE},

    /* Directory listing */
    {"LIST", cmd_LIST, FTP_ARGS_OPTIONAL},
    {"NLST", cmd_NLST, FTP_ARGS_OPTIONAL},
#if FTP_ENABLE_MLST
    {"MLSD", cmd_MLSD, FTP_ARGS_OPTIONAL},
    {"MLST", cmd_MLST, FTP_ARGS_OPTIONAL},
#endif

    /* File transfer */
    {"RETR", cmd_RETR, FTP_ARGS_REQUIRED},
    {"STOR", cmd_STOR, FTP_ARGS_REQUIRED},
    {"APPE", cmd_APPE, FTP_ARGS_REQUIRED},
#if FTP_ENABLE_REST
    {"REST", cmd_REST, FTP_ARGS_REQUIRED},
#endif

    /* File management */
    {"DELE", cmd_DELE, FTP_ARGS_REQUIRED},
    {"RMD", cmd_RMD, FTP_ARGS_REQUIRED},
    {"MKD", cmd_MKD, FTP_ARGS_REQUIRED},
    {"RNFR", cmd_RNFR, FTP_ARGS_REQUIRED},
    {"RNTO", cmd_RNTO, FTP_ARGS_REQUIRED},

    /* Data connection */
    {"PORT", cmd_PORT, FTP_ARGS_REQUIRED},
    {"PASV", cmd_PASV, FTP_ARGS_NONE},

/* Information */
#if FTP_ENABLE_SIZE
    {"SIZE", cmd_SIZE, FTP_ARGS_REQUIRED},
#endif
#if FTP_ENABLE_MDTM
    {"MDTM", cmd_MDTM, FTP_ARGS_REQUIRED},
#endif
    {"STAT", cmd_STAT, FTP_ARGS_OPTIONAL},
    {"SYST", cmd_SYST, FTP_ARGS_NONE},
    {"FEAT", cmd_FEAT, FTP_ARGS_NONE},
    {"HELP", cmd_HELP, FTP_ARGS_OPTIONAL},

    /* Transfer parameters */
    {"TYPE", cmd_TYPE, FTP_ARGS_REQUIRED},
    {"MODE", cmd_MODE, FTP_ARGS_REQUIRED},
    {"STRU", cmd_STRU, FTP_ARGS_REQUIRED},

/* Encryption */
#if FTP_ENABLE_CRYPTO
    {"AUTH", cmd_AUTH, FTP_ARGS_REQUIRED},
#endif
};

static const size_t command_table_size =
    sizeof(command_table) / sizeof(command_table[0]);

/*===========================================================================*
 * DEFAULT REPLY MESSAGES
 *===========================================================================*/

/**
 * Default reply messages for standard codes
 */
static const char *get_default_message(ftp_reply_code_t code) {
  switch (code) {
  /* 1xx - Positive Preliminary */
  case FTP_REPLY_150_FILE_OK:
    return "File status okay; about to open data connection.";

  /* 2xx - Positive Completion */
  case FTP_REPLY_200_OK:
    return "Command okay.";
  case FTP_REPLY_211_SYSTEM_STATUS:
    return "System status.";
  case FTP_REPLY_214_HELP:
    return "Help message.";
  case FTP_REPLY_215_SYSTEM_TYPE:
    return "UNIX Type: L8";
  case FTP_REPLY_220_SERVICE_READY:
    return "Service ready for new user.";
  case FTP_REPLY_221_GOODBYE:
    return "Service closing control connection.";
  case FTP_REPLY_225_DATA_OPEN:
    return "Data connection open; no transfer in progress.";
  case FTP_REPLY_226_TRANSFER_COMPLETE:
    return "Closing data connection. Transfer complete.";
  case FTP_REPLY_230_LOGGED_IN:
    return "User logged in, proceed.";
  case FTP_REPLY_250_FILE_ACTION_OK:
    return "Requested file action okay, completed.";

  /* 3xx - Positive Intermediate */
  case FTP_REPLY_331_NEED_PASSWORD:
    return "User name okay, need password.";
  case FTP_REPLY_350_PENDING:
    return "Requested file action pending further information.";

  /* 4xx - Transient Negative */
  case FTP_REPLY_421_SERVICE_UNAVAIL:
    return "Service not available, closing control connection.";
  case FTP_REPLY_425_CANT_OPEN_DATA:
    return "Can't open data connection.";
  case FTP_REPLY_426_TRANSFER_ABORTED:
    return "Connection closed; transfer aborted.";
  case FTP_REPLY_450_FILE_UNAVAILABLE:
    return "Requested file action not taken.";
  case FTP_REPLY_451_LOCAL_ERROR:
    return "Requested action aborted: local error.";

  /* 5xx - Permanent Negative */
  case FTP_REPLY_500_SYNTAX_ERROR:
    return "Syntax error, command unrecognized.";
  case FTP_REPLY_501_SYNTAX_ARGS:
    return "Syntax error in parameters or arguments.";
  case FTP_REPLY_502_NOT_IMPLEMENTED:
    return "Command not implemented.";
  case FTP_REPLY_503_BAD_SEQUENCE:
    return "Bad sequence of commands.";
  case FTP_REPLY_530_NOT_LOGGED_IN:
    return "Not logged in.";
  case FTP_REPLY_550_FILE_ERROR:
    return "Requested action not taken. File unavailable.";
  case FTP_REPLY_553_FILENAME_INVALID:
    return "Requested action not taken. File name not allowed.";

  default:
    return "Unknown reply code.";
  }
}

/*===========================================================================*
 * COMMAND PARSING
 *===========================================================================*/

/**
 * @brief Parse FTP command line
 */
ftp_error_t ftp_parse_command_line(const char *line, char *command, char *args,
                                   size_t cmd_size, size_t args_size) {
  /* Validate parameters */
  if ((line == NULL) || (command == NULL)) {
    return FTP_ERR_INVALID_PARAM;
  }

  if (cmd_size == 0U) {
    return FTP_ERR_INVALID_PARAM;
  }

  /* Find first space (separator between command and args) */
  const char *space = strchr(line, ' ');
  size_t cmd_len;

  if (space == NULL) {
    /* No arguments: entire line is command */
    cmd_len = strlen(line);
  } else {
    /* Command ends at space */
    cmd_len = (size_t)(space - line);
  }

  /* Validate command length */
  if ((cmd_len == 0U) || (cmd_len >= cmd_size)) {
    return FTP_ERR_PROTOCOL;
  }

  /* Copy command and convert to uppercase */
  for (size_t i = 0U; i < cmd_len; i++) {
    command[i] = (char)toupper((unsigned char)line[i]);
  }
  command[cmd_len] = '\0';

  /* Extract arguments if present */
  if ((space != NULL) && (args != NULL)) {
    /* Skip leading whitespace */
    const char *arg_start = space + 1;
    while ((*arg_start != '\0') && isspace((unsigned char)*arg_start)) {
      arg_start++;
    }

    /* Copy arguments */
    size_t arg_len = strlen(arg_start);

    /* Trim trailing whitespace */
    while ((arg_len > 0U) && isspace((unsigned char)arg_start[arg_len - 1U])) {
      arg_len--;
    }

    if (arg_len >= args_size) {
      return FTP_ERR_PROTOCOL; /* Arguments too long */
    }

    if (arg_len > 0U) {
      memcpy(args, arg_start, arg_len);
      args[arg_len] = '\0';
    } else {
      args[0] = '\0'; /* Empty arguments */
    }
  } else if (args != NULL) {
    args[0] = '\0';
  }

  return FTP_OK;
}

/**
 * @brief Find command in lookup table
 */
const ftp_command_entry_t *ftp_find_command(const char *name) {
  if (name == NULL) {
    return NULL;
  }

  /* Linear search (table is small, ~30 entries) */
  for (size_t i = 0U; i < command_table_size; i++) {
    if (strcmp(name, command_table[i].name) == 0) {
      return &command_table[i];
    }
  }

  return NULL; /* Command not found */
}

/**
 * @brief Validate command arguments
 */
ftp_error_t ftp_validate_command_args(const ftp_command_entry_t *cmd,
                                      const char *args) {
  if (cmd == NULL) {
    return FTP_ERR_INVALID_PARAM;
  }

  int has_args = (args != NULL) && (args[0] != '\0');

  switch (cmd->args_req) {
  case FTP_ARGS_NONE:
    if (has_args) {
      return FTP_ERR_PROTOCOL; /* Args not allowed */
    }
    break;

  case FTP_ARGS_REQUIRED:
    if (!has_args) {
      return FTP_ERR_PROTOCOL; /* Args required */
    }
    break;

  case FTP_ARGS_OPTIONAL:
    /* Both cases allowed */
    break;

  default:
    return FTP_ERR_PROTOCOL; /* Invalid args_req value */
  }

  return FTP_OK;
}

/**
 * @brief Get command table
 */
const ftp_command_entry_t *ftp_get_command_table(size_t *count) {
  if (count != NULL) {
    *count = command_table_size;
  }

  return command_table;
}

/*===========================================================================*
 * REPLY FORMATTING
 *===========================================================================*/

/**
 * @brief Format FTP reply
 */
ssize_t ftp_format_reply(ftp_reply_code_t code, const char *message,
                         char *buffer, size_t size) {
  /* Validate parameters */
  if (buffer == NULL) {
    return FTP_ERR_INVALID_PARAM;
  }

  if (size < 64U) {
    return FTP_ERR_INVALID_PARAM; /* Buffer too small */
  }

  /* Use default message if none provided */
  const char *msg = message;
  if (msg == NULL) {
    msg = get_default_message(code);
  }

  /* Format: "CODE Message\r\n" */
  int n = snprintf(buffer, size, "%d %s\r\n", (int)code, msg);

  if ((n < 0) || ((size_t)n >= size)) {
    return FTP_ERR_INVALID_PARAM; /* Format error or truncation */
  }

  return (ssize_t)n;
}

/**
 * @brief Get default reply message
 */
const char *ftp_get_default_reply_message(ftp_reply_code_t code) {
  return get_default_message(code);
}
