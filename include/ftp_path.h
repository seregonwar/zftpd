/**
 * @file ftp_path.h
 * @brief Secure path validation and normalization
 * 
 * @author SeregonWar
 * @version 1.0.0
 * @date 2025-02-13
 * 
 * SECURITY: Prevents directory traversal attacks (../, symlinks, etc.)
 * ALGORITHM: Stack-based path component processing
 * 
 * SAFETY CLASSIFICATION: Security-critical module
 * STANDARDS: MISRA C:2012, CERT C, ISO C11
 */

#ifndef FTP_PATH_H
#define FTP_PATH_H

#include "ftp_types.h"

/*===========================================================================*
 * PATH VALIDATION
 *===========================================================================*/

/**
 * @brief Normalize path (remove .., ., //)
 * 
 * SECURITY: Essential for preventing directory traversal attacks
 * 
 * ALGORITHM:
 * 1. Split path into components (delimiter: '/')
 * 2. Process each component:
 *    - "." -> skip (current directory)
 *    - ".." -> pop stack (parent directory)
 *    - other -> push to stack
 * 3. Reconstruct path from stack
 * 
 * EXAMPLES:
 *   "/home/user/../admin" -> "/home/admin"
 *   "/home/./user" -> "/home/user"
 *   "/home//user" -> "/home/user"
 *   "/../etc/passwd" -> "/etc/passwd"
 * 
 * @param path   Input path (null-terminated)
 * @param output Output buffer for normalized path
 * @param size   Size of output buffer
 * 
 * @return FTP_OK on success, negative error code on failure
 * @retval FTP_OK Path normalized successfully
 * @retval FTP_ERR_INVALID_PARAM Invalid parameters
 * @retval FTP_ERR_PATH_TOO_LONG Path exceeds buffer size
 * 
 * @pre path != NULL
 * @pre output != NULL
 * @pre size >= FTP_PATH_MAX
 * 
 * @post output contains normalized path (absolute)
 * @post output is null-terminated
 * 
 * @note WCET: O(n) where n = strlen(path)
 * @note Thread-safety: Safe (no shared state)
 * 
 * @warning Does not resolve symlinks (use realpath for that)
 */
ftp_error_t ftp_path_normalize(const char *path,
                                char *output,
                                size_t size);

/**
 * @brief Resolve path relative to session CWD
 * 
 * Converts relative paths to absolute paths:
 * - Absolute path ("/foo") -> keep as-is
 * - Relative path ("foo") -> prepend CWD
 * 
 * @param session Client session (for CWD context)
 * @param path    User-supplied path
 * @param output  Output buffer for resolved path
 * @param size    Size of output buffer
 * 
 * @return FTP_OK on success, negative error code on failure
 * 
 * @pre session != NULL
 * @pre path != NULL
 * @pre output != NULL
 * @pre size >= FTP_PATH_MAX
 * 
 * @post output contains absolute, normalized path
 */
ftp_error_t ftp_path_resolve(const ftp_session_t *session,
                              const char *path,
                              char *output,
                              size_t size);

/**
 * @brief Check if path is within server root
 * 
 * SECURITY: Ensures clients cannot escape chroot jail
 * 
 * @param path Absolute path to check
 * @param root Server root directory
 * 
 * @return 1 if path is within root, 0 if not, negative on error
 * 
 * @pre path != NULL
 * @pre root != NULL
 * @pre path is absolute (starts with '/')
 * @pre root is absolute (starts with '/')
 * 
 * @note WCET: O(min(strlen(path), strlen(root)))
 */
int ftp_path_is_within_root(const char *path, const char *root);

/**
 * @brief Validate path safety
 * 
 * Checks for:
 * - Null bytes (string injection)
 * - Excessive length
 * - Invalid characters
 * 
 * @param path Path to validate
 * 
 * @return 1 if safe, 0 if unsafe, negative on error
 * 
 * @pre path != NULL
 */
int ftp_path_is_safe(const char *path);

/**
 * @brief Get basename from path
 * 
 * EXAMPLE: "/home/user/file.txt" -> "file.txt"
 * 
 * @param path     Input path
 * @param basename Output buffer for basename
 * @param size     Size of output buffer
 * 
 * @return FTP_OK on success, negative error code on failure
 * 
 * @pre path != NULL
 * @pre basename != NULL
 * @pre size > 0
 * 
 * @post basename contains filename component
 */
ftp_error_t ftp_path_basename(const char *path,
                               char *basename,
                               size_t size);

/**
 * @brief Get directory name from path
 * 
 * EXAMPLE: "/home/user/file.txt" -> "/home/user"
 * 
 * @param path    Input path
 * @param dirname Output buffer for directory
 * @param size    Size of output buffer
 * 
 * @return FTP_OK on success, negative error code on failure
 * 
 * @pre path != NULL
 * @pre dirname != NULL
 * @pre size > 0
 */
ftp_error_t ftp_path_dirname(const char *path,
                              char *dirname,
                              size_t size);

/**
 * @brief Join two path components
 * 
 * EXAMPLE: ("/home/user", "file.txt") -> "/home/user/file.txt"
 * 
 * @param base   Base path
 * @param append Path to append
 * @param output Output buffer
 * @param size   Size of output buffer
 * 
 * @return FTP_OK on success, negative error code on failure
 * 
 * @pre base != NULL
 * @pre append != NULL
 * @pre output != NULL
 * @pre size >= FTP_PATH_MAX
 * 
 * @post output contains joined path
 * @post output is normalized
 */
ftp_error_t ftp_path_join(const char *base,
                           const char *append,
                           char *output,
                           size_t size);

#endif /* FTP_PATH_H */
