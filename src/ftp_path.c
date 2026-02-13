/**
 * @file ftp_path.c
 * @brief Secure path validation and normalization implementation
 * 
 * @author SeregonWar
 * @version 1.0.0
 * @date 2025-02-13
 * 
 * SAFETY CLASSIFICATION: Security-critical module
 * STANDARDS: MISRA C:2012, CERT C, ISO C11
 */

#include "ftp_path.h"
#include <string.h>
#include <ctype.h>

/* Maximum number of path components (for stack allocation) */
#define MAX_PATH_COMPONENTS 128U

/*===========================================================================*
 * PATH NORMALIZATION
 *===========================================================================*/

/**
 * @brief Normalize path to canonical form
 * 
 * DESIGN RATIONALE:
 * - Stack-based processing avoids dynamic allocation
 * - Single-pass algorithm is efficient (O(n))
 * - Handles all edge cases: .., ., //, trailing slashes
 */
ftp_error_t ftp_path_normalize(const char *path,
                                char *output,
                                size_t size)
{
    /* Validate parameters */
    if ((path == NULL) || (output == NULL)) {
        return FTP_ERR_INVALID_PARAM;
    }
    
    if (size == 0U) {
        return FTP_ERR_INVALID_PARAM;
    }
    
    /* Check path length */
    size_t path_len = strlen(path);
    if (path_len >= FTP_PATH_MAX) {
        return FTP_ERR_PATH_TOO_LONG;
    }
    
    if (path_len == 0U) {
        /* Empty path -> root */
        if (size < 2U) {
            return FTP_ERR_PATH_TOO_LONG;
        }
        output[0] = '/';
        output[1] = '\0';
        return FTP_OK;
    }
    
    /* Component stack for path processing */
    const char *components[MAX_PATH_COMPONENTS];
    size_t depth = 0U;
    
    /* Working buffer for tokenization */
    char temp[FTP_PATH_MAX];
    if (path_len >= sizeof(temp)) {
        return FTP_ERR_PATH_TOO_LONG;
    }
    
    /* Copy path to working buffer */
    memcpy(temp, path, path_len + 1U);
    
    /* Split path into components and process */
    char *token = strtok(temp, "/");
    while (token != NULL) {
        if (strcmp(token, ".") == 0) {
            /* Current directory reference: skip */
        } else if (strcmp(token, "..") == 0) {
            /* Parent directory: pop stack if not at root */
            if (depth > 0U) {
                depth--;
            }
        } else if (strlen(token) > 0U) {
            /* Regular component: push to stack */
            if (depth >= MAX_PATH_COMPONENTS) {
                return FTP_ERR_PATH_TOO_LONG; /* Path too deep */
            }
            components[depth] = token;
            depth++;
        }
        
        token = strtok(NULL, "/");
    }
    
    /* Reconstruct normalized path */
    if (depth == 0U) {
        /* Root directory */
        if (size < 2U) {
            return FTP_ERR_PATH_TOO_LONG;
        }
        output[0] = '/';
        output[1] = '\0';
        return FTP_OK;
    }
    
    /* Build path from components */
    size_t offset = 0U;
    for (size_t i = 0U; i < depth; i++) {
        size_t comp_len = strlen(components[i]);
        
        /* Check if buffer has space: '/' + component + '\0' */
        if ((offset + comp_len + 2U) > size) {
            return FTP_ERR_PATH_TOO_LONG;
        }
        
        /* Add separator */
        output[offset] = '/';
        offset++;
        
        /* Add component */
        memcpy(output + offset, components[i], comp_len);
        offset += comp_len;
    }
    
    /* Null-terminate */
    output[offset] = '\0';
    
    return FTP_OK;
}

/*===========================================================================*
 * PATH RESOLUTION
 *===========================================================================*/

/**
 * @brief Resolve path relative to session CWD
 */
ftp_error_t ftp_path_resolve(const ftp_session_t *session,
                              const char *path,
                              char *output,
                              size_t size)
{
    /* Validate parameters */
    if ((session == NULL) || (path == NULL) || (output == NULL)) {
        return FTP_ERR_INVALID_PARAM;
    }
    
    if (size < FTP_PATH_MAX) {
        return FTP_ERR_INVALID_PARAM;
    }
    
    /* Check path length */
    size_t path_len = strlen(path);
    if (path_len >= FTP_PATH_MAX) {
        return FTP_ERR_PATH_TOO_LONG;
    }
    
    char temp[FTP_PATH_MAX];
    
    if ((path_len > 0U) && (path[0] == '/')) {
        /* Absolute path: use as-is */
        if (path_len >= sizeof(temp)) {
            return FTP_ERR_PATH_TOO_LONG;
        }
        memcpy(temp, path, path_len + 1U);
    } else {
        /* Relative path: prepend CWD */
        size_t cwd_len = strlen(session->cwd);
        
        /* Check combined length */
        if ((cwd_len + path_len + 2U) >= sizeof(temp)) {
            return FTP_ERR_PATH_TOO_LONG;
        }
        
        /* Build combined path: CWD + '/' + path */
        memcpy(temp, session->cwd, cwd_len);
        temp[cwd_len] = '/';
        memcpy(temp + cwd_len + 1U, path, path_len + 1U);
    }
    
    /* Normalize the path */
    return ftp_path_normalize(temp, output, size);
}

/*===========================================================================*
 * PATH SECURITY CHECKS
 *===========================================================================*/

/**
 * @brief Check if path is within root directory
 */
int ftp_path_is_within_root(const char *path, const char *root)
{
    /* Validate parameters */
    if ((path == NULL) || (root == NULL)) {
        return FTP_ERR_INVALID_PARAM;
    }
    
    /* Both must be absolute paths */
    if ((path[0] != '/') || (root[0] != '/')) {
        return 0; /* Not absolute */
    }
    
    size_t root_len = strlen(root);
    size_t path_len = strlen(path);
    
    /* Special case: root is "/" (allows everything) */
    if ((root_len == 1U) && (root[0] == '/')) {
        return 1;
    }
    
    /* Path must be at least as long as root */
    if (path_len < root_len) {
        return 0;
    }
    
    /* Check if path starts with root */
    if (strncmp(path, root, root_len) != 0) {
        return 0;
    }
    
    /* Ensure proper boundary (prevent "/home" matching "/homeother") */
    if (path_len > root_len) {
        if (path[root_len] != '/') {
            return 0;
        }
    }
    
    return 1; /* Path is within root */
}

/**
 * @brief Validate path safety
 */
int ftp_path_is_safe(const char *path)
{
    if (path == NULL) {
        return 0;
    }
    
    size_t len = strlen(path);
    
    /* Check length */
    if (len >= FTP_PATH_MAX) {
        return 0;
    }
    
    /* Check for null bytes (string injection) */
    if (memchr(path, '\0', len) != (path + len)) {
        return 0; /* Embedded null */
    }
    
    /* Validate characters */
    for (size_t i = 0U; i < len; i++) {
        unsigned char c = (unsigned char)path[i];
        
        /* Allow: alphanumeric, slash, dot, dash, underscore */
        if (!isalnum(c) && (c != '/') && (c != '.') && 
            (c != '-') && (c != '_') && (c != ' ')) {
            /* Potentially dangerous character */
            return 0;
        }
    }
    
    return 1; /* Safe */
}

/*===========================================================================*
 * PATH MANIPULATION
 *===========================================================================*/

/**
 * @brief Extract basename from path
 */
ftp_error_t ftp_path_basename(const char *path,
                               char *basename,
                               size_t size)
{
    /* Validate parameters */
    if ((path == NULL) || (basename == NULL)) {
        return FTP_ERR_INVALID_PARAM;
    }
    
    if (size == 0U) {
        return FTP_ERR_INVALID_PARAM;
    }
    
    /* Find last slash */
    const char *last_slash = strrchr(path, '/');
    const char *name;
    
    if (last_slash == NULL) {
        /* No slash: entire path is basename */
        name = path;
    } else {
        /* Basename is after last slash */
        name = last_slash + 1;
    }
    
    /* Copy basename */
    size_t name_len = strlen(name);
    if ((name_len + 1U) > size) {
        return FTP_ERR_PATH_TOO_LONG;
    }
    
    memcpy(basename, name, name_len + 1U);
    
    return FTP_OK;
}

/**
 * @brief Extract directory name from path
 */
ftp_error_t ftp_path_dirname(const char *path,
                              char *dirname,
                              size_t size)
{
    /* Validate parameters */
    if ((path == NULL) || (dirname == NULL)) {
        return FTP_ERR_INVALID_PARAM;
    }
    
    if (size == 0U) {
        return FTP_ERR_INVALID_PARAM;
    }
    
    /* Find last slash */
    const char *last_slash = strrchr(path, '/');
    
    if (last_slash == NULL) {
        /* No slash: current directory */
        if (size < 2U) {
            return FTP_ERR_PATH_TOO_LONG;
        }
        dirname[0] = '.';
        dirname[1] = '\0';
        return FTP_OK;
    }
    
    if (last_slash == path) {
        /* Root directory */
        if (size < 2U) {
            return FTP_ERR_PATH_TOO_LONG;
        }
        dirname[0] = '/';
        dirname[1] = '\0';
        return FTP_OK;
    }
    
    /* Copy directory part */
    size_t dir_len = (size_t)(last_slash - path);
    if ((dir_len + 1U) > size) {
        return FTP_ERR_PATH_TOO_LONG;
    }
    
    memcpy(dirname, path, dir_len);
    dirname[dir_len] = '\0';
    
    return FTP_OK;
}

/**
 * @brief Join two path components
 */
ftp_error_t ftp_path_join(const char *base,
                           const char *append,
                           char *output,
                           size_t size)
{
    /* Validate parameters */
    if ((base == NULL) || (append == NULL) || (output == NULL)) {
        return FTP_ERR_INVALID_PARAM;
    }
    
    if (size < FTP_PATH_MAX) {
        return FTP_ERR_INVALID_PARAM;
    }
    
    size_t base_len = strlen(base);
    size_t append_len = strlen(append);
    
    /* Check combined length (base + '/' + append + '\0') */
    if ((base_len + append_len + 2U) >= size) {
        return FTP_ERR_PATH_TOO_LONG;
    }
    
    char temp[FTP_PATH_MAX];
    
    /* Copy base */
    memcpy(temp, base, base_len);
    
    /* Add separator if needed */
    if ((base_len > 0U) && (base[base_len - 1U] != '/')) {
        temp[base_len] = '/';
        base_len++;
    }
    
    /* Append second component */
    memcpy(temp + base_len, append, append_len + 1U);
    
    /* Normalize the joined path */
    return ftp_path_normalize(temp, output, size);
}
