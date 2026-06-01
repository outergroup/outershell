#ifndef OUTER_SHELL_DOWNLOADER_H
#define OUTER_SHELL_DOWNLOADER_H

#include <stdbool.h>
#include <stddef.h>

bool outer_shell_download_url_to_file(const char *url,
                                      const char *path,
                                      char *error,
                                      size_t error_size);

bool outer_shell_fetch_url_text(const char *url,
                                char *out,
                                size_t out_size,
                                char *error,
                                size_t error_size);

#endif
